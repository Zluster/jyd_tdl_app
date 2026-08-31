#include "tdl_app/nn_feature.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"
#include "algorithm/private/bmrt_utils.hpp"
#include "bmlib_runtime.h"
#include "bmruntime_interface.h"

namespace tdl_app {
namespace {

constexpr int kInputChannels = 3;

bool featureTraceEnabled() {
  const char *value = std::getenv("TDL_FEATURE_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

bool featureProfileEnabled() {
  const char *value = std::getenv("TDL_BENCH_PROFILE");
  return value && value[0] != '\0' && value[0] != '0';
}

double elapsedMs(std::chrono::steady_clock::time_point begin,
                 std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

void featureTrace(const char *message) {
  if (featureTraceEnabled()) {
    std::fprintf(stderr, "[feature] %s\n", message);
  }
}

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

template <typename T>
T clampCast(float value) {
  const float low = static_cast<float>(std::numeric_limits<T>::lowest());
  const float high = static_cast<float>(std::numeric_limits<T>::max());
  value = std::max(low, std::min(high, value));
  return static_cast<T>(std::lrint(value));
}

std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool parseInputShape(const bm_shape_t &shape, int *height, int *width,
                     bool *nchw, std::string *error) {
  if (!height || !width || !nchw) {
    setError(error, "input shape output pointer is null");
    return false;
  }
  if (shape.num_dims != 4) {
    setError(error, "only 4D input tensor is supported");
    return false;
  }
  if (shape.dims[1] == kInputChannels) {
    *height = shape.dims[2];
    *width = shape.dims[3];
    *nchw = true;
    return true;
  }
  if (shape.dims[3] == kInputChannels) {
    *height = shape.dims[1];
    *width = shape.dims[2];
    *nchw = false;
    return true;
  }
  setError(error, "unable to infer feature input layout from tensor shape");
  return false;
}

std::vector<float> expandChannelValues(const std::vector<float> &values,
                                       float default_value) {
  if (values.empty()) {
    return std::vector<float>(kInputChannels, default_value);
  }
  if (values.size() == 1) {
    return std::vector<float>(kInputChannels, values[0]);
  }
  std::vector<float> expanded = values;
  expanded.resize(kInputChannels, expanded.back());
  return expanded;
}

bool wantsRgbInput(const ModelDescriptor &descriptor) {
  return toUpper(descriptor.input_type) != "BGR";
}

bool wantsL2Normalize(const ModelDescriptor &descriptor) {
  const std::string value = toUpper(descriptor.normalize);
  return value == "L2" || value == "TRUE" || value == "1";
}

bool toVpssRoi(const Box &box, int image_width, int image_height,
               bmrt_runtime::VpssPreprocessor::Roi *roi,
               std::string *error) {
  if (!roi || image_width <= 0 || image_height <= 0 || box.x2 <= box.x1 ||
      box.y2 <= box.y1) {
    setError(error, "feature ROI is invalid");
    return false;
  }
  const int x = std::max(0, std::min(static_cast<int>(box.x1), image_width - 1));
  const int y = std::max(0, std::min(static_cast<int>(box.y1), image_height - 1));
  const int right = std::max(x + 1, std::min(static_cast<int>(box.x2), image_width));
  const int bottom = std::max(y + 1, std::min(static_cast<int>(box.y2), image_height));
  roi->x = x;
  roi->y = y;
  roi->width = right - x;
  roi->height = bottom - y;
  return true;
}

float quantizeInputValue(float value, float input_scale,
                         int input_zero_point) {
  if (input_scale == 0.0f) {
    return value;
  }
  if (std::fabs(input_scale) > 1.0f) {
    return value * input_scale + input_zero_point;
  }
  return value / input_scale + input_zero_point;
}

}  // namespace

class NnFeature::CustomRuntime {
 public:
  CustomRuntime() = default;
  ~CustomRuntime() { close(); }

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    close();

    if (!bmrt_runtime::acquireDevice(&handle_, error)) {
      return false;
    }

    if (!config.bmrt_firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.bmrt_firmware.c_str(), 0);
    }

    runtime_ = bmrt_runtime::createRuntime(handle_);
    if (!runtime_) {
      setError(error, "bmrt_create failed");
      return false;
    }

    if (!bmrt_load_bmodel(runtime_, resolveModelPath(descriptor).c_str())) {
      setError(error, "bmrt_load_bmodel failed: " + resolveModelPath(descriptor));
      return false;
    }

    const char **net_names = nullptr;
    bmrt_get_network_names(runtime_, &net_names);
    if (!net_names || bmrt_get_network_number(runtime_) <= 0) {
      setError(error, "bmodel has no network");
      if (net_names) {
        std::free(net_names);
      }
      return false;
    }

    net_name_ = net_names[0];
    std::free(net_names);

    net_info_ = bmrt_get_network_info(runtime_, net_name_.c_str());
    if (!net_info_) {
      setError(error, "bmrt_get_network_info failed");
      return false;
    }
    if (net_info_->input_num != 1) {
      setError(error, "feature runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->output_num < 1) {
      setError(error, "feature runtime requires at least one output");
      return false;
    }

    if (!parseInputShape(net_info_->stages[0].input_shapes[0], &input_height_,
                         &input_width_, &nchw_layout_, error)) {
      return false;
    }

    descriptor_ = descriptor;
    // The W4BF16 CLIP export is validated for file-image feature banks.  Its
    // camera path is disabled below until a compact INT8 feature model exists.
    host_launch_from_device_ =
        descriptor_.model_type == "FEATURE_CLIP_IMAGE";
    if (featureTraceEnabled()) {
      std::fprintf(stderr,
                   "[feature] open model_type=%s input=%dx%d dtype=%d "
                   "host_launch=%d\n",
                   descriptor_.model_type.c_str(), input_width_, input_height_,
                   static_cast<int>(net_info_->input_dtypes[0]),
                   host_launch_from_device_ ? 1 : 0);
    }
    mean_ = expandChannelValues(descriptor.mean, 0.0f);
    scale_ = expandChannelValues(descriptor.scale, 1.0f);
    l2_normalize_ = wantsL2Normalize(descriptor);
    input_dtype_ = net_info_->input_dtypes[0];
    // Offline image inference does not need VPSS. Defer its device-side
    // allocation until a native camera frame is actually processed.
    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    (void)options;
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!opened_) {
      setError(error, "feature runtime is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }

    if (cached_image_path_ != image_path || cached_image_tensor_.empty()) {
      cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
      if (image.empty()) {
        setError(error, "failed to read image: " + image_path);
        return false;
      }
      preprocess(image, &cached_image_tensor_);
      cached_image_path_ = image_path;
    }
    featureTrace("image feature input prepared");

    std::vector<float> embedding;
    if (!launch(cached_image_tensor_, &embedding, error)) {
      return false;
    }
    featureTrace("image feature launch complete");

    if (l2_normalize_) {
      normalizeEmbedding(&embedding);
    }

    *result = AlgorithmResult{};
    result->feature = std::move(embedding);
    if (featureTraceEnabled()) {
      std::fprintf(stderr, "[feature] image feature dimensions=%zu\n",
                   result->feature.size());
    }
    featureTrace("image feature result returned");
    return true;
  }

  bool inferFrame(const Frame &frame, const Box *roi, AlgorithmResult *result,
                  std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    const auto total_begin = std::chrono::steady_clock::now();
    if (!opened_) {
      setError(error, "feature runtime is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }
    if (!frame.native) {
      setError(error, "feature frame has no native VIDEO_FRAME_INFO_S");
      return false;
    }
    if (!ensureHardwarePreprocessor(error)) {
      return false;
    }
    const auto setup_end = std::chrono::steady_clock::now();
    if (host_launch_from_device_) {
      setError(error,
               "FEATURE_CLIP_IMAGE camera inference is unsupported on CV184X; "
               "use it for offline feature banks or provide a compact INT8 "
               "generic embedding bmodel");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    bmrt_runtime::VpssPreprocessor::Roi hardware_roi;
    if (roi &&
        !toVpssRoi(*roi, static_cast<int>(video->stVFrame.u32Width),
                   static_cast<int>(video->stVFrame.u32Height), &hardware_roi,
                   error)) {
      return false;
    }
    const auto preprocess_begin = std::chrono::steady_clock::now();
    if (!hardware_preprocessor_->preprocess(frame.native,
                                             roi ? &hardware_roi : nullptr,
                                             error)) {
      return false;
    }
    const auto preprocess_end = std::chrono::steady_clock::now();
    featureTrace("VPSS feature preprocess complete");

    std::vector<float> embedding;
    bool launched = false;
    const auto launch_begin = std::chrono::steady_clock::now();
    if (input_dtype_ == BM_FLOAT32) {
      const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
      const size_t input_elements = bmrt_shape_count(&input_shape);
      const size_t expected_elements = static_cast<size_t>(input_width_) *
                                       input_height_ * kInputChannels;
      if (input_elements != expected_elements) {
        setError(error, "float feature model input shape is not 3-channel NCHW");
        return false;
      }
      host_frame_input_bytes_.resize(input_elements);
      if (input_elements == 0 ||
          bm_memcpy_d2s(handle_, host_frame_input_bytes_.data(),
                         hardware_preprocessor_->inputMemory()) != BM_SUCCESS) {
        setError(error, "bm_memcpy_d2s failed for VPSS feature input");
        return false;
      }
      host_frame_input_tensor_.resize(input_elements);
      const size_t plane_size = static_cast<size_t>(input_width_) * input_height_;
      for (int channel = 0; channel < kInputChannels; ++channel) {
        const size_t offset = static_cast<size_t>(channel) * plane_size;
        for (size_t index = 0; index < plane_size; ++index) {
          host_frame_input_tensor_[offset + index] =
              (static_cast<float>(host_frame_input_bytes_[offset + index]) -
               mean_[static_cast<size_t>(channel)]) *
              scale_[static_cast<size_t>(channel)];
        }
      }
      featureTrace("VPSS feature input normalized on host");
      launched = launch(host_frame_input_tensor_, &embedding, error);
    } else {
      launched = launchDevice(hardware_preprocessor_->inputMemory(), &embedding,
                              error);
    }
    if (!launched) {
      return false;
    }
    const auto launch_end = std::chrono::steady_clock::now();
    const auto normalize_begin = std::chrono::steady_clock::now();
    if (l2_normalize_) {
      normalizeEmbedding(&embedding);
    }
    const auto normalize_end = std::chrono::steady_clock::now();
    featureTrace("VPSS feature embedding normalized");
    *result = AlgorithmResult{};
    result->feature = std::move(embedding);
    if (featureProfileEnabled()) {
      std::fprintf(stderr,
                   "[profile] feature face: total=%.3f ms setup=%.3f "
                   "vpss=%.3f launch=%.3f normalize=%.3f\n",
                   elapsedMs(total_begin, normalize_end),
                   elapsedMs(total_begin, setup_end),
                   elapsedMs(preprocess_begin, preprocess_end),
                   elapsedMs(launch_begin, launch_end),
                   elapsedMs(normalize_begin, normalize_end));
    }
    featureTrace("VPSS feature result returned");
    return true;
  }

 private:
  bool ensureHardwarePreprocessor(std::string *error) {
    if (hardware_preprocessor_) return true;
    if (!nchw_layout_ ||
        (input_dtype_ != BM_INT8 && input_dtype_ != BM_UINT8 &&
         input_dtype_ != BM_FLOAT32)) {
      setError(error,
               "feature model does not support VPSS input; require NCHW int8/uint8/float32");
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = input_width_;
    vpss_config.height = input_height_;
    vpss_config.rgb = wantsRgbInput(descriptor_);
    const bool float_input = input_dtype_ == BM_FLOAT32;
    // VPSS only emits 8-bit tensors. Float-input bmodels use its raw planar
    // RGB output; normalization is applied before the host BMRT launch.
    vpss_config.input_dtype = float_input ? BM_UINT8 : input_dtype_;
    vpss_config.input_scale =
        float_input ? 1.0f
                    : (net_info_->input_scales ? net_info_->input_scales[0]
                                                : 1.0f);
    vpss_config.input_zero_point =
        float_input ? 0
                    : (net_info_->input_zero_point
                           ? net_info_->input_zero_point[0]
                           : 0);
    for (int i = 0; i < kInputChannels; ++i) {
      vpss_config.mean[static_cast<size_t>(i)] =
          float_input ? 0.0f : mean_[static_cast<size_t>(i)];
      vpss_config.scale[static_cast<size_t>(i)] =
          float_input ? 1.0f : scale_[static_cast<size_t>(i)];
    }
    std::unique_ptr<bmrt_runtime::VpssPreprocessor> vpss(
        new bmrt_runtime::VpssPreprocessor());
    if (!vpss->open(handle_, vpss_config, error)) return false;
    hardware_preprocessor_ = std::move(vpss);
    if (!float_input && !allocateOutputBuffers(error)) {
      hardware_preprocessor_.reset();
      return false;
    }
    featureTrace("VPSS feature preprocessor opened");
    return true;
  }

  void close() noexcept {
    try {
      closeImpl();
    } catch (const std::exception &exception) {
      std::fprintf(stderr, "feature close ignored during shutdown: %s\n",
                   exception.what());
    } catch (...) {
      std::fprintf(stderr,
                   "feature close ignored an unknown shutdown exception\n");
    }
  }

 private:
  void closeImpl() {
    featureTrace("feature close begin");
    hardware_preprocessor_.reset();
    featureTrace("feature VPSS released");
    releaseOutputBuffers();
    cached_image_path_.clear();
    cached_image_tensor_.clear();
    host_input_bytes_.clear();
    host_frame_input_bytes_.clear();
    host_frame_input_tensor_.clear();
    host_output_bytes_.clear();
    host_output_ptrs_.clear();
    host_output_shapes_.clear();
    if (runtime_) {
      featureTrace("feature BMRT destroy begin");
      try {
        bmrt_runtime::destroyRuntime(runtime_);
      } catch (const std::exception &exception) {
        std::fprintf(stderr, "feature bmrt_destroy ignored during shutdown: %s\n",
                     exception.what());
      } catch (...) {
        std::fprintf(stderr,
                     "feature bmrt_destroy ignored an unknown shutdown exception\n");
      }
      runtime_ = nullptr;
      featureTrace("feature BMRT destroyed");
    }
    if (handle_) {
      featureTrace("feature device free begin");
      bmrt_runtime::releaseDevice(&handle_);
      featureTrace("feature device freed");
    }
    net_info_ = nullptr;
    opened_ = false;
    featureTrace("feature close end");
  }

  bool allocateOutputBuffers(std::string *error) {
    releaseOutputBuffers();
    output_memories_.resize(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t bytes = net_info_->max_output_bytes[i];
      if (bytes == 0) {
        bytes = bmrt_shape_count(&net_info_->stages[0].output_shapes[i]) *
                bmrt_data_type_size(net_info_->output_dtypes[i]);
      }
      if (bytes == 0 ||
          bm_malloc_device_byte(handle_, &output_memories_[static_cast<size_t>(i)],
                                bytes) != BM_SUCCESS) {
        setError(error, "bm_malloc_device_byte failed for feature output");
        releaseOutputBuffers();
        return false;
      }
    }
    return true;
  }

  void releaseOutputBuffers() {
    if (!handle_) {
      output_memories_.clear();
      return;
    }
    for (bm_device_mem_t &memory : output_memories_) {
      if (memory.size > 0) {
        bm_free_device(handle_, memory);
        memory = bm_device_mem_t{};
      }
    }
    output_memories_.clear();
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor) const {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(input_width_, input_height_), 0, 0,
               cv::INTER_LINEAR);

    cv::Mat prepared;
    if (wantsRgbInput(descriptor_)) {
      cv::cvtColor(resized, prepared, cv::COLOR_BGR2RGB);
    } else {
      prepared = resized;
    }

    tensor->assign(static_cast<size_t>(input_width_ * input_height_ * kInputChannels),
                   0.0f);
    if (nchw_layout_) {
      size_t index = 0;
      for (int c = 0; c < kInputChannels; ++c) {
        for (int y = 0; y < input_height_; ++y) {
          for (int x = 0; x < input_width_; ++x) {
            const float value = prepared.at<cv::Vec3b>(y, x)[c];
            (*tensor)[index++] = (value - mean_[c]) * scale_[c];
          }
        }
      }
      return;
    }

    size_t index = 0;
    for (int y = 0; y < input_height_; ++y) {
      for (int x = 0; x < input_width_; ++x) {
        const cv::Vec3b pixel = prepared.at<cv::Vec3b>(y, x);
        for (int c = 0; c < kInputChannels; ++c) {
          (*tensor)[index++] = (static_cast<float>(pixel[c]) - mean_[c]) * scale_[c];
        }
      }
    }
  }

  bool launch(const std::vector<float> &input_tensor, std::vector<float> *embedding,
              std::string *error) {
    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    std::vector<uint8_t> input_bytes;
    void *input_ptrs[1] = {nullptr};
    bm_shape_t input_shapes[1];
    input_shapes[0] = input_shape;

    const float input_scale =
        net_info_->input_scales ? net_info_->input_scales[0] : 1.0f;
    const int input_zero_point =
        net_info_->input_zero_point ? net_info_->input_zero_point[0] : 0;

    if (input_dtype_ == BM_FLOAT32) {
      input_ptrs[0] = const_cast<float *>(input_tensor.data());
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<uint8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "feature runtime does not support this input dtype");
      return false;
    }

    std::vector<std::vector<uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num));
    std::vector<void *> output_ptrs(static_cast<size_t>(net_info_->output_num),
                                    nullptr);
    std::vector<bm_shape_t> output_shapes(static_cast<size_t>(net_info_->output_num),
                                          bm_shape_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[static_cast<size_t>(i)].resize(net_info_->max_output_bytes[i]);
      output_ptrs[static_cast<size_t>(i)] = output_bytes[static_cast<size_t>(i)].data();
      std::memset(&output_shapes[static_cast<size_t>(i)], 0,
                  sizeof(output_shapes[static_cast<size_t>(i)]));
    }

    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      setError(error, "bmrt_launch_data failed");
      return false;
    }

    embedding->clear();
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      const bm_shape_t &output_shape =
          output_shapes[static_cast<size_t>(i)];
      for (int d = 0; d < output_shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(output_shape.dims[d]);
      }
      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;

      const size_t current = embedding->size();
      embedding->resize(current + element_count);
      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *raw =
            reinterpret_cast<const float *>(
                output_bytes[static_cast<size_t>(i)].data());
        std::copy(raw, raw + element_count, embedding->begin() + current);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *raw =
            reinterpret_cast<const int8_t *>(
                output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *raw =
            reinterpret_cast<const uint8_t *>(
                output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "feature runtime does not support this output dtype");
        return false;
      }
    }
    return true;
  }

  bool launchDevice(bm_device_mem_t input_memory,
                    std::vector<float> *embedding, std::string *error) {
    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    bm_tensor_t input_tensor{};
    bmrt_tensor_with_device(&input_tensor, input_memory, input_dtype_, input_shape);
    if (output_memories_.size() !=
        static_cast<size_t>(net_info_->output_num)) {
      setError(error, "feature output buffers are not initialized");
      return false;
    }
    std::vector<bm_tensor_t> device_outputs(
        static_cast<size_t>(net_info_->output_num), bm_tensor_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      bmrt_tensor_with_device(
          &device_outputs[static_cast<size_t>(i)],
          output_memories_[static_cast<size_t>(i)], net_info_->output_dtypes[i],
          net_info_->stages[0].output_shapes[i]);
    }

    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), &input_tensor, 1,
                               device_outputs.data(), net_info_->output_num,
                               true, false)) {
      setError(error, "bmrt_launch_tensor_ex failed");
      return false;
    }
    if (bm_thread_sync(handle_) != BM_SUCCESS) {
      setError(error, "bm_thread_sync failed");
      return false;
    }

    embedding->clear();
    for (int i = 0; i < net_info_->output_num; ++i) {
      const bm_shape_t &shape = device_outputs[static_cast<size_t>(i)].shape;
      size_t element_count = 1;
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      const size_t output_bytes =
          element_count * bmrt_data_type_size(net_info_->output_dtypes[i]);
      std::vector<uint8_t> raw(output_bytes);
      if (bm_memcpy_d2s(handle_, raw.data(),
                         device_outputs[static_cast<size_t>(i)].device_mem) !=
          BM_SUCCESS) {
        setError(error, "bm_memcpy_d2s failed for feature output");
        return false;
      }

      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;
      const size_t current = embedding->size();
      embedding->resize(current + element_count);
      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *values = reinterpret_cast<const float *>(raw.data());
        std::copy(values, values + element_count, embedding->begin() + current);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *values = reinterpret_cast<const int8_t *>(raw.data());
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(values[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *values = reinterpret_cast<const uint8_t *>(raw.data());
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(values[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "feature runtime does not support this output dtype");
        return false;
      }
    }
    return true;
  }

  bool launchQuantized(const std::vector<uint8_t> &input,
                       std::vector<float> *embedding, std::string *error) {
    if (input_dtype_ != BM_INT8 && input_dtype_ != BM_UINT8) {
      setError(error, "quantized host launch requires int8/uint8 input");
      return false;
    }
    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    const size_t expected =
        bmrt_shape_count(&input_shape) * bmrt_data_type_size(input_dtype_);
    if (expected == 0 || input.size() != expected) {
      setError(error, "VPSS feature input size does not match model input");
      return false;
    }
    void *input_ptrs[1] = {const_cast<uint8_t *>(input.data())};
    bm_shape_t input_shapes[1] = {input_shape};
    host_output_bytes_.resize(static_cast<size_t>(net_info_->output_num));
    host_output_ptrs_.resize(static_cast<size_t>(net_info_->output_num));
    host_output_shapes_.resize(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      host_output_bytes_[static_cast<size_t>(i)].resize(
          net_info_->max_output_bytes[i]);
      host_output_ptrs_[static_cast<size_t>(i)] =
          host_output_bytes_[static_cast<size_t>(i)].data();
      std::memset(&host_output_shapes_[static_cast<size_t>(i)], 0,
                  sizeof(host_output_shapes_[static_cast<size_t>(i)]));
    }
    featureTrace("BMRT host launch from VPSS input");
    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, host_output_ptrs_.data(), host_output_shapes_.data(),
                          net_info_->output_num, true)) {
      setError(error, "bmrt_launch_data failed for VPSS feature input");
      return false;
    }
    featureTrace("BMRT host launch complete");
    embedding->clear();
    for (int i = 0; i < net_info_->output_num; ++i) {
      const bm_shape_t &shape = host_output_shapes_[static_cast<size_t>(i)];
      if (featureTraceEnabled()) {
        std::fprintf(stderr, "[feature] host output %d dims=%d", i,
                     shape.num_dims);
        for (int d = 0; d < shape.num_dims && d < BM_MAX_DIMS_NUM; ++d) {
          std::fprintf(stderr, " %d", shape.dims[d]);
        }
        std::fprintf(stderr, "\n");
      }
      if (shape.num_dims <= 0 || shape.num_dims > BM_MAX_DIMS_NUM) {
        setError(error, "BMRT returned an invalid feature output shape");
        return false;
      }
      size_t element_count = 1;
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      if (element_count == 0 ||
          element_count * bmrt_data_type_size(net_info_->output_dtypes[i]) >
              host_output_bytes_[static_cast<size_t>(i)].size()) {
        setError(error, "BMRT feature output size is invalid");
        return false;
      }
      const size_t current = embedding->size();
      embedding->resize(current + element_count);
      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;
      const uint8_t *raw = host_output_bytes_[static_cast<size_t>(i)].data();
      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *values = reinterpret_cast<const float *>(raw);
        std::copy(values, values + element_count, embedding->begin() + current);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *values = reinterpret_cast<const int8_t *>(raw);
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(values[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        for (size_t j = 0; j < element_count; ++j) {
          (*embedding)[current + j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "feature runtime does not support this output dtype");
        return false;
      }
    }
    featureTrace("VPSS feature host output parsed");
    return true;
  }

  void normalizeEmbedding(std::vector<float> *embedding) const {
    if (!embedding || embedding->empty()) {
      return;
    }
    float norm = 0.0f;
    for (float value : *embedding) {
      norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm <= 0.0f) {
      return;
    }
    for (float &value : *embedding) {
      value /= norm;
    }
  }

  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  int input_height_ = 0;
  int input_width_ = 0;
  bool nchw_layout_ = true;
  bool l2_normalize_ = false;
  bm_data_type_t input_dtype_ = BM_FLOAT32;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::vector<bm_device_mem_t> output_memories_;
  std::string cached_image_path_;
  std::vector<float> cached_image_tensor_;
  std::vector<uint8_t> host_input_bytes_;
  std::vector<uint8_t> host_frame_input_bytes_;
  std::vector<float> host_frame_input_tensor_;
  std::vector<std::vector<uint8_t>> host_output_bytes_;
  std::vector<void *> host_output_ptrs_;
  std::vector<bm_shape_t> host_output_shapes_;
  std::mutex infer_mutex_;
  bool host_launch_from_device_ = false;
  bool opened_ = false;
};

NnFeature::NnFeature(std::string model_type) : model_type_(std::move(model_type)) {}

NnFeature::~NnFeature() = default;

TaskType NnFeature::task() const { return TaskType::Feature; }

std::string NnFeature::modelType() const { return model_type_; }

bool NnFeature::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    setError(error, "feature extractor requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnFeature::load(EngineConfig config, std::string *error) {
  // Release the previous model and its private VPSS group before reloading.
  initialized_ = false;
  custom_runtime_.reset();
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
    return false;
  }

  custom_runtime_.reset(new CustomRuntime());
  if (!custom_runtime_->open(config_, descriptor_, error)) {
    custom_runtime_.reset();
    return false;
  }
  if (error) {
    error->clear();
  }
  initialized_ = true;
  return true;
}

bool NnFeature::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnFeature::extract(const std::string &image_path, const InferOptions &options,
                        AlgorithmResult *result, std::string *error) {
  return predict(image_path, options, result, error);
}

bool NnFeature::predict(const std::string &image_path, const InferOptions &options,
                        AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnFeature::predictFrame(const Frame &frame, const InferOptions &options,
                             AlgorithmResult *result, std::string *error) {
  if (!initialized_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (!custom_runtime_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (frame.native) {
    return custom_runtime_->inferFrame(frame, nullptr, result, error);
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, options, result, error);
  }
  setError(error, "feature frame has neither native data nor image_path");
  return false;
}

bool NnFeature::predictFrameCrop(const Frame &frame, const Box &roi,
                                 const InferOptions &options,
                                 AlgorithmResult *result,
                                 std::string *error) {
  (void)options;
  if (!initialized_ || !custom_runtime_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (!frame.native) {
    setError(error, "feature crop requires a native frame");
    return false;
  }
  return custom_runtime_->inferFrame(frame, &roi, result, error);
}

}  // namespace tdl_app
