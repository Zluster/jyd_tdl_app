#include "tdl_app/single_object_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <iostream>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/vpss_preprocessor.hpp"
#include "cvi_comm_video.h"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kTrackerInputChannels = 3;
constexpr float kTemplateOffset = 0.2f;
constexpr float kSearchOffset = 2.0f;
constexpr float kMinResponseScore = 0.2f;

float clampFloat(float value, float low, float high) {
  return std::max(low, std::min(value, high));
}


Box clampBox(const Box &box, int width, int height) {
  Box out = box;
  out.x1 = clampFloat(out.x1, 0.0f, static_cast<float>(std::max(0, width - 1)));
  out.y1 = clampFloat(out.y1, 0.0f, static_cast<float>(std::max(0, height - 1)));
  out.x2 = clampFloat(out.x2, out.x1 + 1.0f, static_cast<float>(std::max(1, width)));
  out.y2 = clampFloat(out.y2, out.y1 + 1.0f, static_cast<float>(std::max(1, height)));
  return out;
}

bool makeTrackerRoi(const Box &box, float offset, int image_width,
                    int image_height,
                    bmrt_runtime::VpssPreprocessor::Roi *roi,
                    std::string *error) {
  if (!roi || image_width <= 1 || image_height <= 1 || !box.valid()) {
    bmrt_runtime::setError(error, "invalid tracker hardware ROI request");
    return false;
  }
  const float box_width = std::max(1.0f, box.width());
  const float box_height = std::max(1.0f, box.height());
  const int requested_x = static_cast<int>(box.x1 - box_width * offset);
  const int requested_y = static_cast<int>(box.y1 - box_height * offset);
  const int requested_width = std::max(
      2, static_cast<int>(box_width * (1.0f + 2.0f * offset)));
  const int requested_height = std::max(
      2, static_cast<int>(box_height * (1.0f + 2.0f * offset)));
  const int x = std::max(0, std::min(requested_x, image_width - 1));
  const int y = std::max(0, std::min(requested_y, image_height - 1));
  roi->x = x;
  roi->y = y;
  roi->width = std::max(1, std::min(requested_width, image_width - x));
  roi->height = std::max(1, std::min(requested_height, image_height - y));
  return true;
}

cv::Mat cropTrackerPatch(const cv::Mat &image,
                         const bmrt_runtime::VpssPreprocessor::Roi &roi,
                         int output_width, int output_height) {
  if (image.empty() || output_width <= 0 || output_height <= 0 ||
      roi.width <= 0 || roi.height <= 0) {
    return cv::Mat();
  }
  const cv::Rect crop(roi.x, roi.y, roi.width, roi.height);
  cv::Mat patch = image(crop);
  cv::resize(patch, patch, cv::Size(output_width, output_height), 0, 0,
             cv::INTER_LINEAR);
  return patch;
}

void writeImageToTensor(const cv::Mat &image, bool rgb_input, bool nchw_layout,
                        const std::vector<float> &mean,
                        const std::vector<float> &scale,
                        std::vector<float> *tensor) {
  cv::Mat prepared;
  if (rgb_input) {
    cv::cvtColor(image, prepared, cv::COLOR_BGR2RGB);
  } else {
    prepared = image;
  }

  tensor->assign(static_cast<size_t>(image.cols * image.rows *
                                     kTrackerInputChannels),
                 0.0f);
  if (nchw_layout) {
    size_t index = 0;
    for (int c = 0; c < kTrackerInputChannels; ++c) {
      for (int y = 0; y < prepared.rows; ++y) {
        for (int x = 0; x < prepared.cols; ++x) {
          const float value = prepared.at<cv::Vec3b>(y, x)[c];
          (*tensor)[index++] = (value - mean[c]) * scale[c];
        }
      }
    }
    return;
  }

  size_t index = 0;
  for (int y = 0; y < prepared.rows; ++y) {
    for (int x = 0; x < prepared.cols; ++x) {
      const cv::Vec3b pixel = prepared.at<cv::Vec3b>(y, x);
      for (int c = 0; c < kTrackerInputChannels; ++c) {
        (*tensor)[index++] =
            (static_cast<float>(pixel[c]) - mean[c]) * scale[c];
      }
    }
  }
}

struct MultiInputOutputTensor {
  bm_shape_t shape{};
  std::vector<float> data;
};

struct TrackerRuntimeProfile {
  double inference_ms = 0.0;
  double output_copy_ms = 0.0;
};

double elapsedMs(const std::chrono::steady_clock::time_point &begin,
                 const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

class MultiInputSession {
 public:
  ~MultiInputSession() { close(); }

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
      bmrt_runtime::setError(error, "bmrt_create failed");
      return false;
    }

    const std::string model_path = resolveModelPath(descriptor);
    if (!bmrt_load_bmodel(runtime_, model_path.c_str())) {
      bmrt_runtime::setError(error, "bmrt_load_bmodel failed: " + model_path);
      return false;
    }

    const char **net_names = nullptr;
    bmrt_get_network_names(runtime_, &net_names);
    if (!net_names || bmrt_get_network_number(runtime_) <= 0) {
      bmrt_runtime::setError(error, "bmodel has no network");
      if (net_names) {
        std::free(net_names);
      }
      return false;
    }
    net_name_ = net_names[0];
    std::free(net_names);

    net_info_ = bmrt_get_network_info(runtime_, net_name_.c_str());
    if (!net_info_) {
      bmrt_runtime::setError(error, "bmrt_get_network_info failed");
      return false;
    }
    if (net_info_->stage_num < 1) {
      bmrt_runtime::setError(error, "invalid network stage info");
      return false;
    }
    if (net_info_->input_num != 2) {
      bmrt_runtime::setError(error, "single object tracker expects exactly two inputs");
      return false;
    }
    if (net_info_->output_num < 2) {
      bmrt_runtime::setError(error, "single object tracker expects at least two outputs");
      return false;
    }

    input_heights_.resize(net_info_->input_num);
    input_widths_.resize(net_info_->input_num);
    input_nchw_.resize(net_info_->input_num);
    input_dtypes_.resize(net_info_->input_num);
    for (int i = 0; i < net_info_->input_num; ++i) {
      bool nchw = true;
      if (!bmrt_runtime::parseInputShape(net_info_->stages[0].input_shapes[i],
                                         &input_heights_[i], &input_widths_[i],
                                         &nchw, error)) {
        return false;
      }
      input_nchw_[i] = nchw ? 1 : 0;
      input_dtypes_[i] = net_info_->input_dtypes[i];
    }
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
        bmrt_runtime::setError(error,
                               "failed to allocate persistent tracker output");
        close();
        return false;
      }
    }
    opened_ = true;
    return true;
  }

  void close() {
    if (handle_) {
      for (bm_device_mem_t &memory : output_memories_) {
        if (memory.size > 0) {
          bm_free_device(handle_, memory);
          memory = bm_device_mem_t{};
        }
      }
    }
    output_memories_.clear();
    if (runtime_) {
      bmrt_runtime::destroyRuntime(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bmrt_runtime::releaseDevice(&handle_);
    }
    net_info_ = nullptr;
    net_name_.clear();
    input_heights_.clear();
    input_widths_.clear();
    input_nchw_.clear();
    input_dtypes_.clear();
    opened_ = false;
  }

  bool opened() const { return opened_; }
  const bm_net_info_t *netInfo() const { return net_info_; }
  int inputNum() const { return net_info_ ? net_info_->input_num : 0; }
  int inputHeight(int index) const { return input_heights_.at(index); }
  int inputWidth(int index) const { return input_widths_.at(index); }
  bool inputNchw(int index) const { return input_nchw_.at(index) != 0; }
  bm_handle_t handle() const { return handle_; }
  bm_data_type_t inputDtype(int index) const { return input_dtypes_.at(index); }
  float inputScale(int index) const {
    return net_info_ && net_info_->input_scales ? net_info_->input_scales[index]
                                                : 1.0f;
  }
  int inputZeroPoint(int index) const {
    return net_info_ && net_info_->input_zero_point
               ? net_info_->input_zero_point[index]
               : 0;
  }

  bool launch(const std::vector<std::vector<float>> &inputs,
              std::vector<MultiInputOutputTensor> *outputs,
              std::string *error) const {
    if (!opened_) {
      bmrt_runtime::setError(error, "runtime session is not initialized");
      return false;
    }
    if (!outputs) {
      bmrt_runtime::setError(error, "output tensor vector is null");
      return false;
    }
    if (static_cast<int>(inputs.size()) != net_info_->input_num) {
      bmrt_runtime::setError(error, "tracker input count mismatch");
      return false;
    }

    std::vector<std::vector<uint8_t>> input_bytes(
        static_cast<size_t>(net_info_->input_num), std::vector<uint8_t>());
    std::vector<void *> input_ptrs(static_cast<size_t>(net_info_->input_num), nullptr);
    std::vector<bm_shape_t> input_shapes(static_cast<size_t>(net_info_->input_num),
                                         bm_shape_t{});

    for (int i = 0; i < net_info_->input_num; ++i) {
      input_shapes[static_cast<size_t>(i)] = net_info_->stages[0].input_shapes[i];
      const float input_scale =
          net_info_->input_scales ? net_info_->input_scales[i] : 1.0f;
      const int input_zero_point =
          net_info_->input_zero_point ? net_info_->input_zero_point[i] : 0;
      const bm_data_type_t dtype = input_dtypes_[static_cast<size_t>(i)];
      if (dtype == BM_FLOAT32) {
        input_ptrs[static_cast<size_t>(i)] =
            const_cast<float *>(inputs[static_cast<size_t>(i)].data());
      } else if (dtype == BM_INT8) {
        input_bytes[static_cast<size_t>(i)].resize(inputs[static_cast<size_t>(i)].size());
        auto *dst = reinterpret_cast<int8_t *>(
            input_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < inputs[static_cast<size_t>(i)].size(); ++j) {
          const float q = bmrt_runtime::quantizeInputValue(
              inputs[static_cast<size_t>(i)][j], input_scale, input_zero_point);
          dst[j] = bmrt_runtime::clampCast<int8_t>(q);
        }
        input_ptrs[static_cast<size_t>(i)] =
            input_bytes[static_cast<size_t>(i)].data();
      } else if (dtype == BM_UINT8) {
        input_bytes[static_cast<size_t>(i)].resize(inputs[static_cast<size_t>(i)].size());
        auto *dst = reinterpret_cast<uint8_t *>(
            input_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < inputs[static_cast<size_t>(i)].size(); ++j) {
          const float q = bmrt_runtime::quantizeInputValue(
              inputs[static_cast<size_t>(i)][j], input_scale, input_zero_point);
          dst[j] = bmrt_runtime::clampCast<uint8_t>(q);
        }
        input_ptrs[static_cast<size_t>(i)] =
            input_bytes[static_cast<size_t>(i)].data();
      } else {
        bmrt_runtime::setError(error, "tracker runtime does not support this input dtype");
        return false;
      }
    }

    std::vector<std::vector<uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num), std::vector<uint8_t>());
    std::vector<void *> output_ptrs(static_cast<size_t>(net_info_->output_num), nullptr);
    std::vector<bm_shape_t> output_shapes(static_cast<size_t>(net_info_->output_num),
                                          bm_shape_t{});

    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[static_cast<size_t>(i)].resize(net_info_->max_output_bytes[i]);
      output_ptrs[static_cast<size_t>(i)] =
          output_bytes[static_cast<size_t>(i)].data();
      std::memset(&output_shapes[static_cast<size_t>(i)], 0, sizeof(bm_shape_t));
    }

    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs.data(),
                          input_shapes.data(), net_info_->input_num,
                          output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      bmrt_runtime::setError(error, "bmrt_launch_data failed");
      return false;
    }

    outputs->clear();
    outputs->reserve(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      const bm_shape_t &shape = output_shapes[static_cast<size_t>(i)];
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }

      MultiInputOutputTensor out;
      out.shape = shape;
      out.data.resize(element_count, 0.0f);
      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;

      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *raw = reinterpret_cast<const float *>(
            output_bytes[static_cast<size_t>(i)].data());
        out.data.assign(raw, raw + element_count);
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *raw = reinterpret_cast<const int8_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else {
        bmrt_runtime::setError(error, "tracker runtime does not support this output dtype");
        return false;
      }
      outputs->push_back(std::move(out));
    }
    return true;
  }

  bool launchDevice(const std::vector<bm_device_mem_t> &inputs,
                    std::vector<MultiInputOutputTensor> *outputs,
                    TrackerRuntimeProfile *profile,
                    std::string *error) const {
    if (!opened_ || !outputs ||
        inputs.size() != static_cast<size_t>(net_info_->input_num) ||
        output_memories_.size() != static_cast<size_t>(net_info_->output_num)) {
      bmrt_runtime::setError(error, "invalid tracker device launch state");
      return false;
    }
    std::vector<bm_tensor_t> input_tensors(inputs.size(), bm_tensor_t{});
    for (int i = 0; i < net_info_->input_num; ++i) {
      bmrt_tensor_with_device(&input_tensors[static_cast<size_t>(i)],
                              inputs[static_cast<size_t>(i)],
                              net_info_->input_dtypes[i],
                              net_info_->stages[0].input_shapes[i]);
    }
    std::vector<bm_tensor_t> output_tensors(
        static_cast<size_t>(net_info_->output_num), bm_tensor_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      bmrt_tensor_with_device(&output_tensors[static_cast<size_t>(i)],
                              output_memories_[static_cast<size_t>(i)],
                              net_info_->output_dtypes[i],
                              net_info_->stages[0].output_shapes[i]);
    }
    const auto inference_begin = std::chrono::steady_clock::now();
    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), input_tensors.data(),
                               net_info_->input_num, output_tensors.data(),
                               net_info_->output_num, true, false) ||
        bm_thread_sync(handle_) != BM_SUCCESS) {
      bmrt_runtime::setError(error, "tracker device launch failed");
      return false;
    }
    if (profile) {
      profile->inference_ms =
          elapsedMs(inference_begin, std::chrono::steady_clock::now());
    }

    const auto output_begin = std::chrono::steady_clock::now();
    outputs->clear();
    outputs->reserve(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      const bm_shape_t &shape = output_tensors[static_cast<size_t>(i)].shape;
      const size_t count = bmrt_shape_count(&shape);
      const size_t bytes = count * bmrt_data_type_size(net_info_->output_dtypes[i]);
      std::vector<uint8_t> raw(bytes);
      if (bm_memcpy_d2s(handle_, raw.data(),
                        output_tensors[static_cast<size_t>(i)].device_mem) !=
          BM_SUCCESS) {
        bmrt_runtime::setError(error, "tracker output copy failed");
        return false;
      }
      MultiInputOutputTensor out;
      out.shape = shape;
      out.data.resize(count);
      const float scale = net_info_->output_scales ? net_info_->output_scales[i]
                                                    : 1.0f;
      const int zero = net_info_->output_zero_point
                           ? net_info_->output_zero_point[i]
                           : 0;
      if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *values = reinterpret_cast<const int8_t *>(raw.data());
        for (size_t j = 0; j < count; ++j) out.data[j] = (values[j] - zero) * scale;
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *values = raw.data();
        for (size_t j = 0; j < count; ++j) out.data[j] = (values[j] - zero) * scale;
      } else if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *values = reinterpret_cast<const float *>(raw.data());
        std::copy(values, values + count, out.data.begin());
      } else {
        bmrt_runtime::setError(error, "unsupported tracker output dtype");
        return false;
      }
      outputs->push_back(std::move(out));
    }
    if (profile) {
      profile->output_copy_ms =
          elapsedMs(output_begin, std::chrono::steady_clock::now());
    }
    return true;
  }

 private:
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  std::vector<int> input_heights_;
  std::vector<int> input_widths_;
  std::vector<std::uint8_t> input_nchw_;
  std::vector<bm_data_type_t> input_dtypes_;
  std::vector<bm_device_mem_t> output_memories_;
  bool opened_ = false;
};

}  // namespace

class SingleObjectTracker::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    reset();

    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    std::string model_type = requested_model_type;
    if (model_type.empty()) model_type = config.model_type;
    if (model_type.empty()) model_type = descriptor_.model_type;
    if (model_type.empty()) model_type = "TRACKING_FEARTRACK";
    if (descriptor_.input_type.empty()) {
      descriptor_.input_type = "rgb";
    }

    EngineConfig engine_config;
    engine_config.model_descriptor_file = config.model_spec;
    engine_config.model_dir = config.model_dir;
    engine_config.bmrt_firmware = config.firmware;
    if (!session_.open(engine_config, descriptor_, error)) {
      return false;
    }

    if (!buildOutputIndices(error)) {
      session_.close();
      return false;
    }

    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f / 255.0f);
    if (!openHardwarePreprocessors(error)) {
      session_.close();
      return false;
    }
    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    model_type_ = model_type;
    return true;
  }

  bool initialize(const std::string &image_path, const Box &target,
                  std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return initializeImage(image, target, error);
  }

  bool initializeFrame(const Frame &frame, const Box &target,
                       std::string *error) {
    if (!session_.opened()) {
      bmrt_runtime::setError(error, "single object tracker is not initialized");
      return false;
    }
    if (!frame.image_path.empty()) {
      cv::Mat image = cv::imread(frame.image_path, cv::IMREAD_COLOR);
      if (image.empty()) {
        bmrt_runtime::setError(error, "failed to read image: " + frame.image_path);
        return false;
      }
      return initializeImage(image, target, error);
    }
    return initializeNativeFrame(frame, target, error);
  }

  bool run(const std::string &image_path, SingleObjectTrackingResult *result,
           std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return trackImage(image, result, error);
  }

  bool runFrame(const Frame &frame, SingleObjectTrackingResult *result,
                std::string *error) {
    if (!ready()) {
      bmrt_runtime::setError(error, "single object tracker target is not initialized");
      return false;
    }
    if (!frame.image_path.empty()) {
      cv::Mat image = cv::imread(frame.image_path, cv::IMREAD_COLOR);
      if (image.empty()) {
        bmrt_runtime::setError(error, "failed to read image: " + frame.image_path);
        return false;
      }
      return trackImage(image, result, error);
    }
    return trackNativeFrame(frame, result, error);
  }

  bool initialized() const { return session_.opened(); }
  bool ready() const { return initialized_ && current_box_.valid(); }
  std::string modelType() const { return model_type_; }
  Box currentBox() const { return current_box_; }

  void reset() {
    template_preprocessor_.reset();
    search_preprocessor_.reset();
    session_.close();
    descriptor_ = ModelDescriptor{};
    mean_.clear();
    scale_.clear();
    model_type_.clear();
    current_box_ = Box{};
    initialized_ = false;
  }

 private:
  bool openHardwarePreprocessors(std::string *error) {
    if (!session_.inputNchw(0) || !session_.inputNchw(1) ||
        (session_.inputDtype(0) != BM_INT8 &&
         session_.inputDtype(0) != BM_UINT8) ||
        (session_.inputDtype(1) != BM_INT8 &&
         session_.inputDtype(1) != BM_UINT8)) {
      bmrt_runtime::setError(
          error, "FearTrack hardware path requires NCHW INT8/UINT8 inputs");
      return false;
    }
    auto make_config = [&](int index) {
      bmrt_runtime::VpssPreprocessor::Config config;
      config.width = session_.inputWidth(index);
      config.height = session_.inputHeight(index);
      config.rgb = bmrt_runtime::wantsRgbInput(descriptor_);
      config.input_dtype = session_.inputDtype(index);
      config.input_scale = session_.inputScale(index);
      config.input_zero_point = session_.inputZeroPoint(index);
      config.mean = {{mean_[0], mean_[1], mean_[2]}};
      config.scale = {{scale_[0], scale_[1], scale_[2]}};
      return config;
    };
    std::unique_ptr<bmrt_runtime::VpssPreprocessor> template_preprocessor(
        new bmrt_runtime::VpssPreprocessor());
    std::unique_ptr<bmrt_runtime::VpssPreprocessor> search_preprocessor(
        new bmrt_runtime::VpssPreprocessor());
    if (!template_preprocessor->open(session_.handle(), make_config(0), error) ||
        !search_preprocessor->open(session_.handle(), make_config(1), error)) {
      return false;
    }
    template_preprocessor_ = std::move(template_preprocessor);
    search_preprocessor_ = std::move(search_preprocessor);
    return true;
  }

  bool initializeNativeFrame(const Frame &frame, const Box &target,
                             std::string *error) {
    if (!frame.native || !template_preprocessor_) {
      bmrt_runtime::setError(error, "FearTrack native template input is unavailable");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    current_box_ = clampBox(target, width, height);
    if (!current_box_.valid()) {
      bmrt_runtime::setError(error, "initial target box is invalid");
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Roi roi;
    if (!makeTrackerRoi(current_box_, kTemplateOffset, width, height, &roi,
                        error) ||
        !template_preprocessor_->preprocess(frame.native, &roi, error)) {
      return false;
    }
    initialized_ = true;
    return true;
  }

  bool trackNativeFrame(const Frame &frame, SingleObjectTrackingResult *result,
                        std::string *error) {
    if (!frame.native || !search_preprocessor_) {
      bmrt_runtime::setError(error, "FearTrack native search input is unavailable");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    bmrt_runtime::VpssPreprocessor::Roi roi;
    const auto total_begin = std::chrono::steady_clock::now();
    const auto preprocess_begin = total_begin;
    if (!makeTrackerRoi(current_box_, kSearchOffset, width, height, &roi,
                        error) ||
        !search_preprocessor_->preprocess(frame.native, &roi, error)) {
      return false;
    }
    const double preprocess_ms =
        elapsedMs(preprocess_begin, std::chrono::steady_clock::now());
    std::vector<MultiInputOutputTensor> outputs;
    TrackerRuntimeProfile runtime_profile;
    const std::vector<bm_device_mem_t> inputs{
        template_preprocessor_->inputMemory(),
        search_preprocessor_->inputMemory()};
    if (!session_.launchDevice(inputs, &outputs, &runtime_profile, error)) {
      return false;
    }
    const auto postprocess_begin = std::chrono::steady_clock::now();
    if (!decodeOutputs(outputs, width, height, roi, result, error)) {
      return false;
    }
    result->preprocess_ms = preprocess_ms;
    result->inference_ms = runtime_profile.inference_ms;
    result->output_copy_ms = runtime_profile.output_copy_ms;
    result->postprocess_ms =
        elapsedMs(postprocess_begin, std::chrono::steady_clock::now());
    result->total_ms = elapsedMs(total_begin, std::chrono::steady_clock::now());
    return true;
  }

  bool decodeOutputs(const std::vector<MultiInputOutputTensor> &outputs,
                     int image_width, int image_height,
                     const bmrt_runtime::VpssPreprocessor::Roi &search_roi,
                     SingleObjectTrackingResult *result,
                     std::string *error) {
    if (!result || cls_output_index_ < 0 || bbox_output_index_ < 0 ||
        static_cast<size_t>(std::max(cls_output_index_, bbox_output_index_)) >=
            outputs.size()) {
      bmrt_runtime::setError(error, "invalid FearTrack output state");
      return false;
    }
    const auto &cls = outputs[static_cast<size_t>(cls_output_index_)];
    const auto &bbox = outputs[static_cast<size_t>(bbox_output_index_)];
    if (cls.shape.num_dims != 4 || bbox.shape.num_dims != 4) {
      bmrt_runtime::setError(error, "tracker output rank is invalid");
      return false;
    }
    const int score_h = cls.shape.dims[2];
    const int score_w = cls.shape.dims[3];
    const int bbox_h = bbox.shape.dims[2];
    const int bbox_w = bbox.shape.dims[3];
    if (score_h <= 0 || score_w <= 0 || score_h != bbox_h || score_w != bbox_w) {
      bmrt_runtime::setError(error, "tracker output map size mismatch");
      return false;
    }
    if (bbox.shape.dims[1] < 4 || cls.shape.dims[1] < 1) {
      bmrt_runtime::setError(error, "tracker output channels are invalid");
      return false;
    }
    const size_t map_size = static_cast<size_t>(bbox_h * bbox_w);
    int selected_x = -1;
    int selected_y = -1;
    float selected_score = 0.0f;
    // FearTrack trains bbox_Add as logarithmic distances from a stride-16 grid
    // point. Select the local 5x5 response average used by the model's SDK.
    for (int y = 2; y < score_h - 2; ++y) {
      for (int x = 2; x < score_w - 2; ++x) {
        const float center_score = cls.data[static_cast<size_t>(y * score_w + x)];
        if (center_score <= kMinResponseScore) {
          continue;
        }
        float average_score = 0.0f;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            average_score +=
                cls.data[static_cast<size_t>((y + dy) * score_w + x + dx)];
          }
        }
        average_score /= 25.0f;
        if (average_score > selected_score) {
          selected_score = average_score;
          selected_x = x;
          selected_y = y;
        }
      }
    }
    result->clear();
    result->search_width = session_.inputWidth(1);
    result->search_height = session_.inputHeight(1);
    if (selected_x < 0 || selected_y < 0) {
      result->box = current_box_;
      return true;
    }
    const size_t selected = static_cast<size_t>(selected_y * score_w + selected_x);
    const float grid_x = static_cast<float>(selected_x *
                                            (session_.inputWidth(1) / score_w));
    const float grid_y = static_cast<float>(selected_y *
                                            (session_.inputHeight(1) / score_h));
    const float x1 = grid_x - std::exp(bbox.data[0 * map_size + selected]);
    const float y1 = grid_y - std::exp(bbox.data[1 * map_size + selected]);
    const float x2 = grid_x + std::exp(bbox.data[2 * map_size + selected]);
    const float y2 = grid_y + std::exp(bbox.data[3 * map_size + selected]);
    const float scale_x = static_cast<float>(search_roi.width) /
                          std::max(1, session_.inputWidth(1));
    const float scale_y = static_cast<float>(search_roi.height) /
                          std::max(1, session_.inputHeight(1));
    Box next;
    next.x1 = search_roi.x + x1 * scale_x;
    next.y1 = search_roi.y + y1 * scale_y;
    next.x2 = search_roi.x + x2 * scale_x;
    next.y2 = search_roi.y + y2 * scale_y;
    next.score = selected_score;
    next.class_id = 0;
    current_box_ = clampBox(next, image_width, image_height);
    result->box = current_box_;
    result->confidence = selected_score;
    result->tracked = true;
    result->response_x = selected_x;
    result->response_y = selected_y;
    return true;
  }

  bool buildOutputIndices(std::string *error) {
    const bm_net_info_t *net_info = session_.netInfo();
    cls_output_index_ = -1;
    bbox_output_index_ = -1;
    score_map_width_ = 0;
    score_map_height_ = 0;
    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = net_info->stages[0].output_shapes[i];
      if (shape.num_dims != 4) {
        continue;
      }
      if (shape.dims[1] == 1 && cls_output_index_ < 0) {
        cls_output_index_ = i;
        score_map_height_ = shape.dims[2];
        score_map_width_ = shape.dims[3];
      } else if (shape.dims[1] == 4 && bbox_output_index_ < 0) {
        bbox_output_index_ = i;
      }
    }
    if (cls_output_index_ < 0 || bbox_output_index_ < 0) {
      bmrt_runtime::setError(error, "single object tracker outputs are incomplete");
      return false;
    }
    return true;
  }

  bool initializeImage(const cv::Mat &image, const Box &target,
                       std::string *error) {
    if (!session_.opened()) {
      bmrt_runtime::setError(error, "single object tracker is not initialized");
      return false;
    }
    if (image.empty()) {
      bmrt_runtime::setError(error, "initialize image is empty");
      return false;
    }
    current_box_ = clampBox(target, image.cols, image.rows);
    if (!current_box_.valid()) {
      bmrt_runtime::setError(error, "initial target box is invalid");
      return false;
    }

    bmrt_runtime::VpssPreprocessor::Roi roi;
    if (!makeTrackerRoi(current_box_, kTemplateOffset, image.cols, image.rows,
                        &roi, error)) {
      return false;
    }
    cv::Mat patch = cropTrackerPatch(image, roi, session_.inputWidth(0),
                                     session_.inputHeight(0));
    if (patch.empty()) {
      bmrt_runtime::setError(error, "failed to crop tracker template patch");
      return false;
    }
    writeImageToTensor(patch, bmrt_runtime::wantsRgbInput(descriptor_),
                       session_.inputNchw(0), mean_, scale_, &template_tensor_);
    initialized_ = true;
    return true;
  }

  bool trackImage(const cv::Mat &image, SingleObjectTrackingResult *result,
                  std::string *error) {
    if (!ready()) {
      bmrt_runtime::setError(error, "single object tracker target is not initialized");
      return false;
    }
    if (!result) {
      bmrt_runtime::setError(error, "tracker result pointer is null");
      return false;
    }

    const auto total_begin = std::chrono::steady_clock::now();
    const auto preprocess_begin = total_begin;
    bmrt_runtime::VpssPreprocessor::Roi roi;
    if (!makeTrackerRoi(current_box_, kSearchOffset, image.cols, image.rows,
                        &roi, error)) {
      return false;
    }
    cv::Mat patch = cropTrackerPatch(image, roi, session_.inputWidth(1),
                                     session_.inputHeight(1));
    if (patch.empty()) {
      bmrt_runtime::setError(error, "failed to crop tracker search patch");
      return false;
    }

    std::vector<float> search_tensor;
    writeImageToTensor(patch, bmrt_runtime::wantsRgbInput(descriptor_),
                       session_.inputNchw(1), mean_, scale_, &search_tensor);

    std::vector<std::vector<float>> inputs(2);
    inputs[0] = template_tensor_;
    inputs[1] = std::move(search_tensor);
    const double preprocess_ms =
        elapsedMs(preprocess_begin, std::chrono::steady_clock::now());

    std::vector<MultiInputOutputTensor> outputs;
    const auto inference_begin = std::chrono::steady_clock::now();
    if (!session_.launch(inputs, &outputs, error)) {
      return false;
    }
    const double inference_ms =
        elapsedMs(inference_begin, std::chrono::steady_clock::now());
    const auto postprocess_begin = std::chrono::steady_clock::now();
    if (!decodeOutputs(outputs, image.cols, image.rows, roi, result, error)) {
      return false;
    }
    result->preprocess_ms = preprocess_ms;
    result->inference_ms = inference_ms;
    result->postprocess_ms =
        elapsedMs(postprocess_begin, std::chrono::steady_clock::now());
    result->total_ms = elapsedMs(total_begin, std::chrono::steady_clock::now());
    return true;
  }

  MultiInputSession session_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  std::vector<float> template_tensor_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> template_preprocessor_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> search_preprocessor_;
  std::string model_type_;
  Box current_box_;
  int cls_output_index_ = -1;
  int bbox_output_index_ = -1;
  int score_map_width_ = 0;
  int score_map_height_ = 0;
  bool initialized_ = false;
};

SingleObjectTracker::SingleObjectTracker() = default;

SingleObjectTracker::SingleObjectTracker(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

SingleObjectTracker::~SingleObjectTracker() = default;

SingleObjectTracker::SingleObjectTracker(SingleObjectTracker &&other) noexcept =
    default;

SingleObjectTracker &SingleObjectTracker::operator=(
    SingleObjectTracker &&other) noexcept = default;

bool SingleObjectTracker::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_.reset(new Impl);
  }
  const bool ok =
      impl_->load(config_, requested_model_type_, &requested_model_type_, error);
  last_error_ = ok ? std::string() : (error ? *error : std::string());
  return ok;
}

bool SingleObjectTracker::load(const std::string &model_spec,
                               std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool SingleObjectTracker::load(const std::string &model_spec,
                               const std::string &firmware,
                               std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool SingleObjectTracker::load(const std::string &model_spec,
                               const std::string &firmware,
                               const std::string &model_dir,
                               std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool SingleObjectTracker::initialize(const std::string &image_path,
                                     const Box &target, std::string *error) {
  if (!impl_) {
    bmrt_runtime::setError(error, "single object tracker is not initialized");
    last_error_ = error ? *error : std::string();
    return false;
  }
  const bool ok = impl_->initialize(image_path, target, error);
  last_error_ = ok ? std::string() : (error ? *error : std::string());
  return ok;
}

bool SingleObjectTracker::initializeFrame(const Frame &frame, const Box &target,
                                          std::string *error) {
  if (!impl_) {
    bmrt_runtime::setError(error, "single object tracker is not initialized");
    last_error_ = error ? *error : std::string();
    return false;
  }
  const bool ok = impl_->initializeFrame(frame, target, error);
  last_error_ = ok ? std::string() : (error ? *error : std::string());
  return ok;
}

bool SingleObjectTracker::run(const std::string &image_path,
                              SingleObjectTrackingResult *result,
                              std::string *error) {
  if (!impl_) {
    bmrt_runtime::setError(error, "single object tracker is not initialized");
    last_error_ = error ? *error : std::string();
    return false;
  }
  const bool ok = impl_->run(image_path, result, error);
  last_error_ = ok ? std::string() : (error ? *error : std::string());
  return ok;
}

bool SingleObjectTracker::runFrame(const Frame &frame,
                                   SingleObjectTrackingResult *result,
                                   std::string *error) {
  if (!impl_) {
    bmrt_runtime::setError(error, "single object tracker is not initialized");
    last_error_ = error ? *error : std::string();
    return false;
  }
  const bool ok = impl_->runFrame(frame, result, error);
  last_error_ = ok ? std::string() : (error ? *error : std::string());
  return ok;
}

bool SingleObjectTracker::initialized() const {
  return impl_ && impl_->initialized();
}

bool SingleObjectTracker::ready() const { return impl_ && impl_->ready(); }

std::string SingleObjectTracker::modelType() const {
  return impl_ ? impl_->modelType() : requested_model_type_;
}

Box SingleObjectTracker::currentBox() const {
  return impl_ ? impl_->currentBox() : Box{};
}

void SingleObjectTracker::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
