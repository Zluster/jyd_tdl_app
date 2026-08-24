#include "tdl_app/nn_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
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

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

bool debugEnabled() {
  const char *value = std::getenv("JYD_APP_CLASSIFIER_DEBUG");
  return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

template <typename... Args>
void debugLog(const char *fmt, Args... args) {
  if (!debugEnabled()) {
    return;
  }
  std::fprintf(stderr, "[classifier-debug] ");
  std::fprintf(stderr, fmt, args...);
  std::fprintf(stderr, "\n");
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
  setError(error, "unable to infer classifier input layout from tensor shape");
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

bool wantsSoftmax(const ModelDescriptor &descriptor) {
  const auto it = descriptor.extra.find("apply_softmax");
  if (it == descriptor.extra.end()) {
    return false;
  }
  const std::string value = toUpper(it->second);
  return value == "1" || value == "TRUE" || value == "YES" ||
         value == "ON";
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

class NnClassifier::CustomRuntime {
 public:
  CustomRuntime() = default;
  ~CustomRuntime() { close(); }

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    close();
    hardware_error_.clear();

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
      setError(error, "classifier runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->output_num < 1) {
      setError(error, "classifier runtime requires at least one output");
      return false;
    }
    if (net_info_->stage_num < 1) {
      setError(error, "invalid network stage info");
      return false;
    }

    if (!parseInputShape(net_info_->stages[0].input_shapes[0], &input_height_,
                         &input_width_, &nchw_layout_, error)) {
      return false;
    }

    descriptor_ = descriptor;
    mean_ = expandChannelValues(descriptor.mean, 0.0f);
    scale_ = expandChannelValues(descriptor.scale, 1.0f);
    labels_ = descriptor.labels;
    softmax_output_ = wantsSoftmax(descriptor);
    input_dtype_ = net_info_->input_dtypes[0];
    if (nchw_layout_ &&
        (input_dtype_ == BM_INT8 || input_dtype_ == BM_UINT8)) {
      bmrt_runtime::VpssPreprocessor::Config vpss_config;
      vpss_config.width = input_width_;
      vpss_config.height = input_height_;
      vpss_config.rgb = wantsRgbInput(descriptor_);
      vpss_config.input_dtype = input_dtype_;
      vpss_config.input_scale =
          net_info_->input_scales ? net_info_->input_scales[0] : 1.0f;
      vpss_config.input_zero_point = net_info_->input_zero_point
                                         ? net_info_->input_zero_point[0]
                                         : 0;
      for (int i = 0; i < kInputChannels; ++i) {
        vpss_config.mean[static_cast<size_t>(i)] = mean_[static_cast<size_t>(i)];
        vpss_config.scale[static_cast<size_t>(i)] = scale_[static_cast<size_t>(i)];
      }
      std::unique_ptr<bmrt_runtime::VpssPreprocessor> vpss(
          new bmrt_runtime::VpssPreprocessor());
      if (vpss->open(handle_, vpss_config, &hardware_error_)) {
        hardware_preprocessor_ = std::move(vpss);
      }
    }
    if (hardware_preprocessor_ && !allocateOutputBuffers(&hardware_error_)) {
      hardware_preprocessor_.reset();
    }
    debugLog("open model=%s input=%dx%d layout=%s input_dtype=%d input_scale=%.8f input_zp=%d output_dtype=%d output_scale=%.8f output_zp=%d labels=%d",
             resolveModelPath(descriptor).c_str(), input_width_, input_height_,
             nchw_layout_ ? "NCHW" : "NHWC", static_cast<int>(input_dtype_),
             net_info_->input_scales ? net_info_->input_scales[0] : 1.0f,
             net_info_->input_zero_point ? net_info_->input_zero_point[0] : 0,
             static_cast<int>(net_info_->output_dtypes[0]),
             net_info_->output_scales ? net_info_->output_scales[0] : 1.0f,
             net_info_->output_zero_point ? net_info_->output_zero_point[0] : 0,
             static_cast<int>(labels_.size()));
    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!opened_) {
      setError(error, "classifier runtime is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }

    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      setError(error, "failed to read image: " + image_path);
      return false;
    }

    std::vector<float> input_tensor;
    preprocess(image, &input_tensor);

    std::vector<std::vector<float>> outputs;
    if (!launch(input_tensor, &outputs, error)) {
      return false;
    }
    return buildResult(outputs, options, result, error);
  }

  bool inferFrame(const Frame &frame, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!opened_) {
      setError(error, "classifier runtime is not initialized");
      return false;
    }
    if (!frame.native) {
      setError(error, "classifier frame has no native VIDEO_FRAME_INFO_S");
      return false;
    }
    if (!hardware_preprocessor_) {
      const std::string reason = hardware_error_.empty()
                                     ? "require NCHW int8/uint8 model input"
                                     : hardware_error_;
      setError(error, "classifier VPSS input is unavailable: " + reason);
      return false;
    }
    if (!hardware_preprocessor_->preprocess(frame.native, error)) {
      return false;
    }

    std::vector<std::vector<float>> outputs;
    if (!launchDevice(hardware_preprocessor_->inputMemory(), &outputs, error)) {
      return false;
    }
    return buildResult(outputs, options, result, error);
  }

 private:
  void close() {
    hardware_preprocessor_.reset();
    releaseOutputBuffers();
    if (runtime_) {
      bmrt_runtime::destroyRuntime(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bmrt_runtime::releaseDevice(&handle_);
    }
    net_info_ = nullptr;
    opened_ = false;
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
        setError(error, "bm_malloc_device_byte failed for classifier output");
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

  bool launch(const std::vector<float> &input_tensor,
              std::vector<std::vector<float>> *outputs,
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
    if (debugEnabled() && !input_tensor.empty()) {
      const auto mm = std::minmax_element(input_tensor.begin(), input_tensor.end());
      debugLog("input tensor range=[%.6f,%.6f] v0=%.6f v1=%.6f v2=%.6f",
               *mm.first, *mm.second, input_tensor[0],
               input_tensor.size() > 1 ? input_tensor[1] : 0.0f,
               input_tensor.size() > 2 ? input_tensor[2] : 0.0f);
    }

    if (input_dtype_ == BM_FLOAT32) {
      input_ptrs[0] = const_cast<float *>(input_tensor.data());
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      int q_min = 127;
      int q_max = -128;
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<int8_t>(q);
        q_min = std::min(q_min, static_cast<int>(dst[i]));
        q_max = std::max(q_max, static_cast<int>(dst[i]));
      }
      debugLog("quantized int8 range=[%d,%d] q0=%d q1=%d q2=%d",
               q_min, q_max, static_cast<int>(dst[0]),
               input_tensor.size() > 1 ? static_cast<int>(dst[1]) : 0,
               input_tensor.size() > 2 ? static_cast<int>(dst[2]) : 0);
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      int q_min = 255;
      int q_max = 0;
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<uint8_t>(q);
        q_min = std::min(q_min, static_cast<int>(dst[i]));
        q_max = std::max(q_max, static_cast<int>(dst[i]));
      }
      debugLog("quantized uint8 range=[%d,%d] q0=%d q1=%d q2=%d",
               q_min, q_max, static_cast<int>(dst[0]),
               input_tensor.size() > 1 ? static_cast<int>(dst[1]) : 0,
               input_tensor.size() > 2 ? static_cast<int>(dst[2]) : 0);
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "classifier runtime does not support this input dtype");
      return false;
    }

    std::vector<std::vector<uint8_t>> output_bytes(
        net_info_->output_num, std::vector<uint8_t>());
    std::vector<void *> output_ptrs(net_info_->output_num, nullptr);
    std::vector<bm_shape_t> output_shapes(static_cast<size_t>(net_info_->output_num),
                                          bm_shape_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[i].resize(net_info_->max_output_bytes[i]);
      output_ptrs[i] = output_bytes[i].data();
      std::memset(&output_shapes[i], 0, sizeof(output_shapes[i]));
    }

    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      setError(error, "bmrt_launch_data failed");
      return false;
    }

    outputs->clear();
    outputs->reserve(net_info_->output_num);
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      for (int d = 0; d < output_shapes[i].num_dims; ++d) {
        element_count *= static_cast<size_t>(output_shapes[i].dims[d]);
      }
      std::vector<float> decoded(element_count, 0.0f);
      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;

      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *raw =
            reinterpret_cast<const float *>(output_bytes[i].data());
        decoded.assign(raw, raw + element_count);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *raw =
            reinterpret_cast<const int8_t *>(output_bytes[i].data());
        for (size_t j = 0; j < element_count; ++j) {
          decoded[j] = (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *raw =
            reinterpret_cast<const uint8_t *>(output_bytes[i].data());
        for (size_t j = 0; j < element_count; ++j) {
          decoded[j] = (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "classifier runtime does not support this output dtype");
        return false;
      }
      if (debugEnabled() && !decoded.empty()) {
        const auto mm =
            std::minmax_element(decoded.begin(), decoded.end());
        debugLog("output[%d] range=[%.6f,%.6f] v0=%.6f v1=%.6f v2=%.6f",
                 i, *mm.first, *mm.second, decoded[0],
                 decoded.size() > 1 ? decoded[1] : 0.0f,
                 decoded.size() > 2 ? decoded[2] : 0.0f);
      }
      outputs->push_back(std::move(decoded));
    }
    return true;
  }

  bool launchDevice(bm_device_mem_t input_memory,
                    std::vector<std::vector<float>> *outputs,
                    std::string *error) {
    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    bm_tensor_t input_tensor{};
    bmrt_tensor_with_device(&input_tensor, input_memory, input_dtype_, input_shape);
    if (output_memories_.size() !=
        static_cast<size_t>(net_info_->output_num)) {
      setError(error, "classifier output buffers are not initialized");
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

    outputs->clear();
    outputs->reserve(static_cast<size_t>(net_info_->output_num));
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
        setError(error, "bm_memcpy_d2s failed for classifier output");
        return false;
      }

      std::vector<float> decoded(element_count, 0.0f);
      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;
      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *values = reinterpret_cast<const float *>(raw.data());
        decoded.assign(values, values + element_count);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *values = reinterpret_cast<const int8_t *>(raw.data());
        for (size_t j = 0; j < element_count; ++j) {
          decoded[j] =
              (static_cast<int>(values[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *values = reinterpret_cast<const uint8_t *>(raw.data());
        for (size_t j = 0; j < element_count; ++j) {
          decoded[j] =
              (static_cast<int>(values[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "classifier runtime does not support this output dtype");
        return false;
      }
      outputs->push_back(std::move(decoded));
    }
    return true;
  }

  bool buildResult(const std::vector<std::vector<float>> &outputs,
                   const InferOptions &options, AlgorithmResult *result,
                   std::string *error) const {
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }
    if (outputs.empty() || outputs[0].empty()) {
      setError(error, "classifier runtime produced no output");
      return false;
    }

    *result = AlgorithmResult{};
    result->labels = labels_;
    std::vector<float> scores = outputs[0];
    if (softmax_output_) {
      applySoftmax(&scores);
    }
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int lhs, int rhs) { return scores[lhs] > scores[rhs]; });
    const int count = std::min(std::max(1, options.top_k),
                               static_cast<int>(order.size()));
    for (int i = 0; i < count; ++i) {
      ClassificationItem item;
      item.class_id = order[static_cast<size_t>(i)];
      item.score = scores[static_cast<size_t>(item.class_id)];
      result->classes.push_back(item);
    }
    return true;
  }

  void applySoftmax(std::vector<float> *scores) const {
    if (!scores || scores->empty()) {
      return;
    }
    const float max_value = *std::max_element(scores->begin(), scores->end());
    float sum = 0.0f;
    for (float &score : *scores) {
      score = std::exp(score - max_value);
      sum += score;
    }
    if (sum <= 0.0f) {
      return;
    }
    for (float &score : *scores) {
      score /= sum;
    }
  }

  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  std::vector<std::string> labels_;
  int input_height_ = 0;
  int input_width_ = 0;
  bool nchw_layout_ = true;
  bool softmax_output_ = false;
  bm_data_type_t input_dtype_ = BM_FLOAT32;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::vector<bm_device_mem_t> output_memories_;
  std::string hardware_error_;
  std::mutex infer_mutex_;
  bool opened_ = false;
};

NnClassifier::NnClassifier(std::string model_type)
    : model_type_(std::move(model_type)) {}

NnClassifier::~NnClassifier() = default;

TaskType NnClassifier::task() const { return TaskType::Classification; }

std::string NnClassifier::modelType() const { return model_type_; }

bool NnClassifier::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    setError(error, "classifier requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnClassifier::load(EngineConfig config, std::string *error) {
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

bool NnClassifier::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnClassifier::classify(const std::string &image_path,
                            const InferOptions &options,
                            AlgorithmResult *result, std::string *error) {
  return predict(image_path, options, result, error);
}

bool NnClassifier::predict(const std::string &image_path,
                           const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnClassifier::predictFrame(const Frame &frame, const InferOptions &options,
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
    return custom_runtime_->inferFrame(frame, options, result, error);
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, options, result, error);
  }
  setError(error, "classifier frame has neither native data nor image_path");
  return false;
}

}  // namespace tdl_app
