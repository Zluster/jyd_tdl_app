#include "tdl_app/nn_yolov8.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
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

namespace tdl_app {
namespace {

constexpr int kInputChannels = 3;
constexpr int kRegMax = 16;
constexpr int kBoxChannels = 4 * kRegMax;
constexpr int kObbAngleChannels = 1;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

float sigmoid(float value) {
  return 1.0f / (1.0f + std::exp(-value));
}

float intersectionOverUnion(const Box &lhs, const Box &rhs) {
  const float x1 = std::max(lhs.x1, rhs.x1);
  const float y1 = std::max(lhs.y1, rhs.y1);
  const float x2 = std::min(lhs.x2, rhs.x2);
  const float y2 = std::min(lhs.y2, rhs.y2);
  const float w = std::max(0.0f, x2 - x1);
  const float h = std::max(0.0f, y2 - y1);
  const float inter = w * h;
  const float area_l = std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
  const float area_r = std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
  const float denom = area_l + area_r - inter;
  if (denom <= 0.0f) {
    return 0.0f;
  }
  return inter / denom;
}

bool isOrientedBox(const Box &box) {
  return box.landmarks.size() == 4;
}

float contourAreaAbs(const std::vector<cv::Point2f> &points) {
  if (points.size() < 3) {
    return 0.0f;
  }
  return std::fabs(static_cast<float>(cv::contourArea(points)));
}

bool cornersFromBox(const Box &box, std::vector<cv::Point2f> *corners) {
  if (!corners || box.landmarks.size() != 4) {
    return false;
  }
  corners->clear();
  corners->reserve(4);
  for (const auto &landmark : box.landmarks) {
    corners->emplace_back(landmark.x, landmark.y);
  }
  return true;
}

float orientedIntersectionOverUnion(const Box &lhs, const Box &rhs) {
  std::vector<cv::Point2f> lhs_corners;
  std::vector<cv::Point2f> rhs_corners;
  if (!cornersFromBox(lhs, &lhs_corners) || !cornersFromBox(rhs, &rhs_corners)) {
    return intersectionOverUnion(lhs, rhs);
  }

  std::vector<cv::Point2f> intersection;
  const int relation = cv::rotatedRectangleIntersection(
      cv::minAreaRect(lhs_corners), cv::minAreaRect(rhs_corners), intersection);
  if (relation == cv::INTERSECT_NONE) {
    return 0.0f;
  }

  const float lhs_area = contourAreaAbs(lhs_corners);
  const float rhs_area = contourAreaAbs(rhs_corners);
  float inter_area = 0.0f;
  if (relation == cv::INTERSECT_FULL) {
    inter_area = std::min(lhs_area, rhs_area);
  } else {
    inter_area = contourAreaAbs(intersection);
  }
  const float denom = lhs_area + rhs_area - inter_area;
  if (denom <= 0.0f) {
    return 0.0f;
  }
  return inter_area / denom;
}

std::vector<Box> nonMaxSuppression(const std::vector<Box> &boxes, float iou_threshold) {
  std::vector<int> order(boxes.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](int lhs, int rhs) { return boxes[lhs].score > boxes[rhs].score; });

  std::vector<Box> kept;
  std::vector<bool> removed(boxes.size(), false);
  for (size_t i = 0; i < order.size(); ++i) {
    const int index = order[i];
    if (removed[index]) {
      continue;
    }
    kept.push_back(boxes[index]);
    for (size_t j = i + 1; j < order.size(); ++j) {
      const int other = order[j];
      if (removed[other]) {
        continue;
      }
      if (boxes[index].class_id != boxes[other].class_id) {
        continue;
      }
      const float iou =
          (isOrientedBox(boxes[index]) || isOrientedBox(boxes[other]))
              ? orientedIntersectionOverUnion(boxes[index], boxes[other])
              : intersectionOverUnion(boxes[index], boxes[other]);
      if (iou > iou_threshold) {
        removed[other] = true;
      }
    }
  }
  return kept;
}

bool parseInputShape(const bm_shape_t &shape, int *height, int *width,
                     std::string *error) {
  if (!height || !width) {
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
    return true;
  }
  if (shape.dims[3] == kInputChannels) {
    *height = shape.dims[1];
    *width = shape.dims[2];
    return true;
  }
  setError(error, "unable to infer YOLOv8 input layout from tensor shape");
  return false;
}

template <typename T>
T clampCast(float value) {
  const float low = static_cast<float>(std::numeric_limits<T>::lowest());
  const float high = static_cast<float>(std::numeric_limits<T>::max());
  value = std::max(low, std::min(high, value));
  return static_cast<T>(std::lrint(value));
}

}  // namespace

class NnYolov8::CustomRuntime {
 public:
  struct LaunchProfile {
    double bmrt_ms = 0.0;
    double output_copy_decode_ms = 0.0;
  };

  struct ProfileStats {
    int count = 0;
    double preprocess_ms = 0.0;
    double launch_total_ms = 0.0;
    double bmrt_ms = 0.0;
    double output_copy_decode_ms = 0.0;
    double decode_nms_ms = 0.0;
    double total_ms = 0.0;
  };

  struct OutputBranch {
    int stride = 0;
    int feat_w = 0;
    int feat_h = 0;
    int bbox_index = -1;
    int cls_index = -1;
    int angle_index = -1;
    int cls_offset = 0;
  };

  struct RawOutput {
    bm_shape_t shape{};
    bm_data_type_t dtype = BM_FLOAT32;
    float scale = 1.0f;
    int zero_point = 0;
    std::vector<uint8_t> data;
  };

  CustomRuntime() = default;
  ~CustomRuntime() { close(); }

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    close();

    bm_status_t status = bm_dev_request(&handle_, 0);
    if (status != BM_SUCCESS) {
      setError(error, "bm_dev_request failed");
      return false;
    }

    if (!config.bmrt_firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.bmrt_firmware.c_str(), 0);
    }

    runtime_ = bmrt_create(handle_);
    if (!runtime_) {
      setError(error, "bmrt_create failed");
      return false;
    }

    labels_ = descriptor.labels;
    obb_mode_ = false;
    const auto obb_it = descriptor.extra.find("type");
    if (obb_it != descriptor.extra.end()) {
      obb_mode_ = toUpper(obb_it->second) == "OBB";
    }
    if (!obb_mode_) {
      obb_mode_ = startsWith(toUpper(descriptor.model_type), "YOLOV8_OBB");
    }
    const std::string model_path = resolveModelPath(descriptor);
    if (!bmrt_load_bmodel(runtime_, model_path.c_str())) {
      setError(error, "bmrt_load_bmodel failed: " + model_path);
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
      setError(error, "custom YOLOv8 runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->stage_num < 1) {
      setError(error, "invalid network stage info");
      return false;
    }

    if (!parseInputShape(net_info_->stages[0].input_shapes[0], &input_height_,
                         &input_width_, error)) {
      return false;
    }

    input_dtype_ = net_info_->input_dtypes[0];
    output_count_ = net_info_->output_num;
    if (output_count_ <= 0) {
      setError(error, "custom YOLOv8 runtime has no outputs");
      return false;
    }

    if (!buildBranches(error)) {
      return false;
    }

    descriptor_ = descriptor;
    if (net_info_->stages[0].input_shapes[0].dims[1] == kInputChannels &&
        (input_dtype_ == BM_INT8 || input_dtype_ == BM_UINT8)) {
      bmrt_runtime::VpssPreprocessor::Config vpss_config;
      vpss_config.width = input_width_;
      vpss_config.height = input_height_;
      vpss_config.rgb = toUpper(descriptor.input_type) != "BGR";
      vpss_config.keep_aspect_ratio = true;
      vpss_config.padding = {{114, 114, 114}};
      vpss_config.input_dtype = input_dtype_;
      // The legacy uint8 path passes model pixels through unchanged.
      vpss_config.input_scale =
          input_dtype_ == BM_UINT8
              ? 1.0f
              : (net_info_->input_scales ? net_info_->input_scales[0] : 1.0f);
      vpss_config.input_zero_point =
          input_dtype_ == BM_UINT8
              ? 0
              : (net_info_->input_zero_point ? net_info_->input_zero_point[0]
                                              : 0);
      vpss_config.mean = {{0.0f, 0.0f, 0.0f}};
      vpss_config.scale = {{1.0f, 1.0f, 1.0f}};
      std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor(
          new bmrt_runtime::VpssPreprocessor());
      if (preprocessor->open(handle_, vpss_config, &hardware_error_)) {
        hardware_preprocessor_ = std::move(preprocessor);
        if (!allocateOutputBuffers(error)) {
          return false;
        }
      }
    } else {
      hardware_error_ = "require NCHW int8/uint8 model input";
    }

    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!opened_) {
      setError(error, "custom YOLOv8 runtime is not initialized");
      return false;
    }
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      setError(error, "failed to read image: " + image_path);
      return false;
    }
    return inferMat(image, options, result, error);
  }

  bool inferMat(const cv::Mat &image, const InferOptions &options,
                AlgorithmResult *result, std::string *error) {
    if (!opened_) {
      setError(error, "custom YOLOv8 runtime is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }
    if (image.empty()) {
      setError(error, "input image is empty");
      return false;
    }

    std::vector<uint8_t> input_data;
    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    preprocess(image, &input_data, &ratio, &top, &left);

    std::vector<std::vector<float>> outputs;
    std::vector<bm_shape_t> output_shapes;
    if (!launch(input_data, &outputs, &output_shapes, error)) {
      return false;
    }

    *result = AlgorithmResult{};
    result->labels = labels_;
    result->boxes = decode(outputs, output_shapes, image.cols, image.rows, ratio,
                           top, left, options.threshold, options.iou_threshold);
    return true;
  }

  bool inferFrame(const Frame &frame, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!opened_) {
      setError(error, "custom YOLOv8 runtime is not initialized");
      return false;
    }
    if (!result || !frame.native) {
      setError(error, "YOLOv8 frame/result pointer is null");
      return false;
    }
    if (!hardware_preprocessor_) {
      const std::string reason = hardware_error_.empty()
                                     ? "VPSS preprocessor is unavailable"
                                     : hardware_error_;
      setError(error, "YOLOv8 VPSS input is unavailable: " + reason);
      return false;
    }
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    const int source_width = static_cast<int>(video->stVFrame.u32Width);
    const int source_height = static_cast<int>(video->stVFrame.u32Height);
    if (source_width <= 0 || source_height <= 0) {
      setError(error, "YOLOv8 source frame dimensions are invalid");
      return false;
    }
    const bool profile_enabled = profileEnabled();
    const auto total_begin = std::chrono::steady_clock::now();
    const float ratio = std::min(
        static_cast<float>(input_height_) / source_height,
        static_cast<float>(input_width_) / source_width);
    const int resized_width = static_cast<int>(std::round(source_width * ratio));
    const int resized_height = static_cast<int>(std::round(source_height * ratio));
    const int top = (input_height_ - resized_height) / 2;
    const int left = (input_width_ - resized_width) / 2;
    const auto preprocess_begin = std::chrono::steady_clock::now();
    if (!hardware_preprocessor_->preprocess(video, error)) {
      return false;
    }
    const auto preprocess_end = std::chrono::steady_clock::now();

    std::vector<RawOutput> outputs;
    LaunchProfile launch_profile;
    const auto launch_begin = std::chrono::steady_clock::now();
    if (!launchDevice(hardware_preprocessor_->inputMemory(), &outputs,
                      error,
                      profile_enabled ? &launch_profile : nullptr)) {
      return false;
    }
    const auto launch_end = std::chrono::steady_clock::now();
    *result = AlgorithmResult{};
    result->labels = labels_;
    const auto decode_begin = std::chrono::steady_clock::now();
    result->boxes = decodeRaw(outputs, source_width, source_height, ratio, top,
                              left, options.threshold, options.iou_threshold);
    const auto decode_end = std::chrono::steady_clock::now();
    if (profile_enabled) {
      profile_.count++;
      profile_.preprocess_ms += elapsedMs(preprocess_begin, preprocess_end);
      profile_.launch_total_ms += elapsedMs(launch_begin, launch_end);
      profile_.bmrt_ms += launch_profile.bmrt_ms;
      profile_.output_copy_decode_ms += launch_profile.output_copy_decode_ms;
      profile_.decode_nms_ms += elapsedMs(decode_begin, decode_end);
      profile_.total_ms += elapsedMs(total_begin, decode_end);
    }
    return true;
  }

 private:
  void close() {
    printProfile();
    profile_ = ProfileStats{};
    hardware_preprocessor_.reset();
    releaseOutputBuffers();
    if (runtime_) {
      bmrt_destroy(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bm_dev_free(handle_);
      handle_ = nullptr;
    }
    opened_ = false;
    net_info_ = nullptr;
    branches_.clear();
    num_classes_ = 0;
  }

  bool allocateOutputBuffers(std::string *error) {
    releaseOutputBuffers();
    output_memories_.resize(static_cast<size_t>(output_count_));
    for (int i = 0; i < output_count_; ++i) {
      size_t bytes = net_info_->max_output_bytes[i];
      if (bytes == 0) {
        bytes = bmrt_shape_count(&net_info_->stages[0].output_shapes[i]) *
                bmrt_data_type_size(net_info_->output_dtypes[i]);
      }
      if (bytes == 0 ||
          bm_malloc_device_byte(handle_, &output_memories_[static_cast<size_t>(i)],
                                bytes) != BM_SUCCESS) {
        setError(error, "bm_malloc_device_byte failed for YOLOv8 output");
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

  bool buildBranches(std::string *error) {
    branches_.clear();
    num_classes_ = static_cast<int>(labels_.size());

    struct TempBranch {
      int stride = 0;
      int feat_w = 0;
      int feat_h = 0;
      int bbox_index = -1;
      int cls_index = -1;
      int angle_index = -1;
      int cls_offset = 0;
    };

    std::vector<TempBranch> temp;
    const auto &stage = net_info_->stages[0];
    for (int i = 0; i < output_count_; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (shape.num_dims != 4) {
        setError(error, "custom YOLOv8 runtime expects 4D outputs");
        return false;
      }
      const int channel = shape.dims[1];
      const int feat_h = shape.dims[2];
      const int feat_w = shape.dims[3];
      if (feat_h <= 0 || feat_w <= 0) {
        setError(error, "invalid output feature map shape");
        return false;
      }
      const int stride_h = input_height_ / feat_h;
      const int stride_w = input_width_ / feat_w;
      if (stride_h != stride_w || stride_h <= 0) {
        setError(error, "custom YOLOv8 runtime found invalid stride");
        return false;
      }

      auto it = std::find_if(temp.begin(), temp.end(), [&](const TempBranch &branch) {
        return branch.stride == stride_h;
      });
      if (it == temp.end()) {
        temp.push_back(TempBranch{stride_h, feat_w, feat_h, -1, -1, -1, 0});
        it = temp.end() - 1;
      }

      if (obb_mode_) {
        if (channel == kBoxChannels) {
          it->bbox_index = i;
        } else if (channel == kObbAngleChannels) {
          it->angle_index = i;
        } else if (num_classes_ > 0 && channel == num_classes_) {
          it->cls_index = i;
          it->cls_offset = 0;
        } else {
          num_classes_ = channel;
          it->cls_index = i;
          it->cls_offset = 0;
        }
      } else if (channel == kBoxChannels) {
        it->bbox_index = i;
      } else if (num_classes_ > 0 && channel == num_classes_) {
        it->cls_index = i;
        it->cls_offset = 0;
      } else if (num_classes_ > 0 && channel == kBoxChannels + num_classes_) {
        it->bbox_index = i;
        it->cls_index = i;
        it->cls_offset = kBoxChannels;
      } else if (num_classes_ == 0) {
        if (channel == kBoxChannels) {
          it->bbox_index = i;
        } else {
          num_classes_ = channel;
          it->cls_index = i;
          it->cls_offset = 0;
        }
      } else {
        setError(error, "unexpected YOLOv8 branch channel: " + std::to_string(channel));
        return false;
      }
    }

    if (num_classes_ <= 0) {
      setError(error, "unable to infer YOLOv8 class count");
      return false;
    }

    for (auto &branch : temp) {
      if (branch.bbox_index < 0 || branch.cls_index < 0 ||
          (obb_mode_ && branch.angle_index < 0)) {
        setError(error, "incomplete YOLOv8 output branches");
        return false;
      }
      branches_.push_back(OutputBranch{branch.stride, branch.feat_w, branch.feat_h,
                                       branch.bbox_index, branch.cls_index,
                                       branch.angle_index,
                                       branch.cls_offset});
    }

    std::sort(branches_.begin(), branches_.end(),
              [](const OutputBranch &lhs, const OutputBranch &rhs) {
                return lhs.stride < rhs.stride;
              });
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<uint8_t> *data, float *ratio,
                  int *top, int *left) {
    *ratio = std::min(static_cast<float>(input_height_) / image.rows,
                      static_cast<float>(input_width_) / image.cols);
    const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
    const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));
    *top = (input_height_ - resized_h) / 2;
    *left = (input_width_ - resized_w) / 2;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0,
               cv::INTER_LINEAR);

    cv::Mat padded(input_height_, input_width_, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(*left, *top, resized_w, resized_h)));

    cv::Mat rgb;
    cv::cvtColor(padded, rgb, cv::COLOR_BGR2RGB);

    data->assign(static_cast<size_t>(input_height_ * input_width_ * kInputChannels), 0);
    size_t index = 0;
    for (int c = 0; c < kInputChannels; ++c) {
      for (int y = 0; y < input_height_; ++y) {
        for (int x = 0; x < input_width_; ++x) {
          (*data)[index++] = rgb.at<cv::Vec3b>(y, x)[c];
        }
      }
    }
  }

  bool launch(const std::vector<uint8_t> &input_data,
              std::vector<std::vector<float>> *outputs,
              std::vector<bm_shape_t> *output_shapes,
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

    if (input_dtype_ == BM_UINT8) {
      input_ptrs[0] = const_cast<uint8_t *>(input_data.data());
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_data.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_data.size(); ++i) {
        const float q = input_scale == 0.0f
                            ? static_cast<float>(input_data[i])
                            : static_cast<float>(input_data[i]) / input_scale + input_zero_point;
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_FLOAT32) {
      input_bytes.resize(input_data.size() * sizeof(float));
      auto *dst = reinterpret_cast<float *>(input_bytes.data());
      for (size_t i = 0; i < input_data.size(); ++i) {
        dst[i] = static_cast<float>(input_data[i]) / 255.0f;
      }
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "custom YOLOv8 runtime does not support this input dtype yet");
      return false;
    }

    std::vector<std::vector<uint8_t>> output_bytes(
        output_count_, std::vector<uint8_t>());
    std::vector<void *> output_ptrs(output_count_, nullptr);
    output_shapes->assign(static_cast<size_t>(output_count_), bm_shape_t{});

    for (int i = 0; i < output_count_; ++i) {
      output_bytes[i].resize(net_info_->max_output_bytes[i]);
      output_ptrs[i] = output_bytes[i].data();
      std::memset(&(*output_shapes)[i], 0, sizeof((*output_shapes)[i]));
    }

    const bool launched =
        bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                         1, output_ptrs.data(), output_shapes->data(),
                         output_count_, true);
    if (!launched) {
      setError(error, "bmrt_launch_data failed");
      return false;
    }

    outputs->clear();
    outputs->reserve(output_count_);
    for (int i = 0; i < output_count_; ++i) {
      size_t element_count = 1;
      for (int d = 0; d < (*output_shapes)[i].num_dims; ++d) {
        element_count *= static_cast<size_t>((*output_shapes)[i].dims[d]);
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
        setError(error, "custom YOLOv8 runtime does not support this output dtype yet");
        return false;
      }
      outputs->push_back(std::move(decoded));
    }
    return true;
  }

  bool launchDevice(bm_device_mem_t input_memory,
                    std::vector<RawOutput> *outputs, std::string *error,
                    LaunchProfile *profile) {
    if (output_memories_.size() != static_cast<size_t>(output_count_)) {
      setError(error, "YOLOv8 output buffers are not initialized");
      return false;
    }
    bm_tensor_t input_tensor{};
    bmrt_tensor_with_device(&input_tensor, input_memory, input_dtype_,
                            net_info_->stages[0].input_shapes[0]);
    std::vector<bm_tensor_t> device_outputs(
        static_cast<size_t>(output_count_), bm_tensor_t{});
    for (int i = 0; i < output_count_; ++i) {
      bmrt_tensor_with_device(&device_outputs[static_cast<size_t>(i)],
                              output_memories_[static_cast<size_t>(i)],
                              net_info_->output_dtypes[i],
                              net_info_->stages[0].output_shapes[i]);
    }
    const auto bmrt_begin = std::chrono::steady_clock::now();
    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), &input_tensor, 1,
                               device_outputs.data(), output_count_, true,
                               false)) {
      setError(error, "bmrt_launch_tensor_ex failed");
      return false;
    }
    if (bm_thread_sync(handle_) != BM_SUCCESS) {
      setError(error, "bm_thread_sync failed");
      return false;
    }
    if (profile) {
      profile->bmrt_ms = elapsedMs(bmrt_begin, std::chrono::steady_clock::now());
    }

    const auto output_copy_decode_begin = std::chrono::steady_clock::now();
    outputs->clear();
    outputs->reserve(static_cast<size_t>(output_count_));
    for (int i = 0; i < output_count_; ++i) {
      const bm_shape_t &shape = device_outputs[static_cast<size_t>(i)].shape;
      size_t element_count = 1;
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      const size_t bytes =
          element_count * bmrt_data_type_size(net_info_->output_dtypes[i]);
      RawOutput output;
      output.shape = shape;
      output.dtype = net_info_->output_dtypes[i];
      output.scale = net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      output.zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;
      output.data.resize(bytes);
      if (bm_memcpy_d2s(handle_, output.data.data(),
                         device_outputs[static_cast<size_t>(i)].device_mem) !=
          BM_SUCCESS) {
        setError(error, "bm_memcpy_d2s failed for YOLOv8 output");
        return false;
      }
      outputs->push_back(std::move(output));
    }
    if (profile) {
      profile->output_copy_decode_ms =
          elapsedMs(output_copy_decode_begin, std::chrono::steady_clock::now());
    }
    return true;
  }

  static bool profileEnabled() {
    const char *value = std::getenv("TDL_BENCH_PROFILE");
    return value && value[0] != '\0' && value[0] != '0';
  }

  static double elapsedMs(std::chrono::steady_clock::time_point begin,
                          std::chrono::steady_clock::time_point end) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                   .count()) /
           1000.0;
  }

  void printProfile() const {
    if (!profileEnabled() || profile_.count <= 0) {
      return;
    }
    const double count = static_cast<double>(profile_.count);
    std::cerr << std::fixed << std::setprecision(3)
              << "[profile] yolov8 avg over " << profile_.count
              << " runs: total=" << profile_.total_ms / count
              << " ms, vpss_preprocess=" << profile_.preprocess_ms / count
              << " ms, bmrt_sync=" << profile_.bmrt_ms / count
              << " ms, output_copy="
              << profile_.output_copy_decode_ms / count
              << " ms, decode_nms=" << profile_.decode_nms_ms / count
              << " ms, launch_total=" << profile_.launch_total_ms / count
              << " ms\n";
  }

  std::vector<float> decodeDfl(const std::vector<float> &bbox, int anchor_index,
                               int anchor_count) const {
    std::vector<float> values(4, 0.0f);
    for (int side = 0; side < 4; ++side) {
      const int offset = side * kRegMax;
      float max_logit = -std::numeric_limits<float>::infinity();
      for (int i = 0; i < kRegMax; ++i) {
        const float logit = bbox[(offset + i) * anchor_count + anchor_index];
        if (logit > max_logit) {
          max_logit = logit;
        }
      }
      float sum = 0.0f;
      float weighted = 0.0f;
      for (int i = 0; i < kRegMax; ++i) {
        const float expv = std::exp(bbox[(offset + i) * anchor_count + anchor_index] - max_logit);
        sum += expv;
        weighted += expv * static_cast<float>(i);
      }
      values[side] = sum > 0.0f ? weighted / sum : 0.0f;
    }
    return values;
  }

  Point mapPoint(float x, float y, int image_width, int image_height, float ratio,
                 int top, int left) const {
    Point point;
    point.x = (x - static_cast<float>(left)) / ratio;
    point.y = (y - static_cast<float>(top)) / ratio;
    point.x = std::max(0.0f, std::min(point.x, static_cast<float>(image_width)));
    point.y = std::max(0.0f, std::min(point.y, static_cast<float>(image_height)));
    return point;
  }

  Box decodeOrientedBox(const std::vector<float> &distances, float angle, float grid_x,
                        float grid_y, const OutputBranch &branch, int image_width,
                        int image_height, float ratio, int top, int left,
                        int class_id, float score) const {
    const float xf = (distances[2] - distances[0]) * 0.5f;
    const float yf = (distances[3] - distances[1]) * 0.5f;
    const float cos_a = std::cos(angle);
    const float sin_a = std::sin(angle);
    const float center_x = (grid_x + xf * cos_a - yf * sin_a) * branch.stride;
    const float center_y = (grid_y + xf * sin_a + yf * cos_a) * branch.stride;
    const float width = (distances[0] + distances[2]) * branch.stride;
    const float height = (distances[1] + distances[3]) * branch.stride;
    const float half_w = width * 0.5f;
    const float half_h = height * 0.5f;

    static const float kCornerSigns[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

    Box box;
    box.class_id = class_id;
    box.score = score;
    box.x1 = static_cast<float>(image_width);
    box.y1 = static_cast<float>(image_height);
    box.x2 = 0.0f;
    box.y2 = 0.0f;
    box.landmarks.reserve(4);

    for (const auto &sign : kCornerSigns) {
      const float local_x = sign[0] * half_w;
      const float local_y = sign[1] * half_h;
      const float x = center_x + local_x * cos_a - local_y * sin_a;
      const float y = center_y + local_x * sin_a + local_y * cos_a;
      Point mapped = mapPoint(x, y, image_width, image_height, ratio, top, left);
      box.x1 = std::min(box.x1, mapped.x);
      box.y1 = std::min(box.y1, mapped.y);
      box.x2 = std::max(box.x2, mapped.x);
      box.y2 = std::max(box.y2, mapped.y);
      box.landmarks.push_back(mapped);
    }
    return box;
  }

  std::vector<Box> decode(const std::vector<std::vector<float>> &outputs,
                          const std::vector<bm_shape_t> &output_shapes,
                          int image_width, int image_height, float ratio, int top,
                          int left, float threshold, float iou_threshold) const {
    (void)output_shapes;

    std::vector<Box> boxes;
    const float inverse_threshold =
        threshold <= 0.0f ? -std::numeric_limits<float>::infinity()
                          : (threshold >= 1.0f
                                 ? std::numeric_limits<float>::infinity()
                                 : std::log(threshold / (1.0f - threshold)));

    for (const auto &branch : branches_) {
      const std::vector<float> &bbox_out = outputs[branch.bbox_index];
      const std::vector<float> &cls_out = outputs[branch.cls_index];
      const std::vector<float> *angle_out =
          obb_mode_ ? &outputs[branch.angle_index] : nullptr;
      const int anchor_count = branch.feat_w * branch.feat_h;

      for (int anchor = 0; anchor < anchor_count; ++anchor) {
        int best_class = -1;
        float best_logit = -std::numeric_limits<float>::infinity();
        for (int cls = 0; cls < num_classes_; ++cls) {
          const int channel = branch.cls_offset + cls;
          const float logit = cls_out[channel * anchor_count + anchor];
          if (logit > best_logit) {
            best_logit = logit;
            best_class = cls;
          }
        }
        if (best_class < 0 || best_logit < inverse_threshold) {
          continue;
        }

        const float score = sigmoid(best_logit);
        if (score < threshold) {
          continue;
        }

        const std::vector<float> distances = decodeDfl(bbox_out, anchor, anchor_count);
        const int anchor_y = anchor / branch.feat_w;
        const int anchor_x = anchor % branch.feat_w;
        const float grid_x = static_cast<float>(anchor_x) + 0.5f;
        const float grid_y = static_cast<float>(anchor_y) + 0.5f;

        const float x1 = (grid_x - distances[0]) * branch.stride;
        const float y1 = (grid_y - distances[1]) * branch.stride;
        const float x2 = (grid_x + distances[2]) * branch.stride;
        const float y2 = (grid_y + distances[3]) * branch.stride;

        Box box;
        if (obb_mode_ && angle_out) {
          const float angle =
              (sigmoid((*angle_out)[anchor]) - 0.25f) *
              static_cast<float>(CV_PI);
          box = decodeOrientedBox(distances, angle, grid_x, grid_y, branch,
                                  image_width, image_height, ratio, top, left,
                                  best_class, score);
        } else {
          box.class_id = best_class;
          box.score = score;
          box.x1 = (x1 - left) / ratio;
          box.y1 = (y1 - top) / ratio;
          box.x2 = (x2 - left) / ratio;
          box.y2 = (y2 - top) / ratio;
          box.x1 = std::max(0.0f, std::min(box.x1, static_cast<float>(image_width)));
          box.y1 = std::max(0.0f, std::min(box.y1, static_cast<float>(image_height)));
          box.x2 = std::max(0.0f, std::min(box.x2, static_cast<float>(image_width)));
          box.y2 = std::max(0.0f, std::min(box.y2, static_cast<float>(image_height)));
        }
        boxes.push_back(box);
      }
    }

    return nonMaxSuppression(boxes, iou_threshold);
  }

  static bool isQuantized(const RawOutput &output) {
    return output.dtype == BM_INT8 || output.dtype == BM_UINT8;
  }

  static int rawQuantizedValue(const RawOutput &output, size_t index) {
    if (output.dtype == BM_INT8) {
      return static_cast<int>(reinterpret_cast<const int8_t *>(output.data.data())[index]);
    }
    return static_cast<int>(output.data[index]);
  }

  static float rawValue(const RawOutput &output, size_t index) {
    if (output.dtype == BM_INT8 || output.dtype == BM_UINT8) {
      return (rawQuantizedValue(output, index) - output.zero_point) * output.scale;
    }
    if (output.dtype == BM_FLOAT32) {
      return reinterpret_cast<const float *>(output.data.data())[index];
    }
    return 0.0f;
  }

  std::vector<float> decodeDflRaw(const RawOutput &bbox, int anchor_index,
                                  int anchor_count) const {
    std::vector<float> values(4, 0.0f);
    for (int side = 0; side < 4; ++side) {
      const int offset = side * kRegMax;
      float max_logit = -std::numeric_limits<float>::infinity();
      for (int i = 0; i < kRegMax; ++i) {
        const float logit = rawValue(
            bbox, static_cast<size_t>((offset + i) * anchor_count + anchor_index));
        max_logit = std::max(max_logit, logit);
      }
      float sum = 0.0f;
      float weighted = 0.0f;
      for (int i = 0; i < kRegMax; ++i) {
        const float logit = rawValue(
            bbox, static_cast<size_t>((offset + i) * anchor_count + anchor_index));
        const float expv = std::exp(logit - max_logit);
        sum += expv;
        weighted += expv * static_cast<float>(i);
      }
      values[side] = sum > 0.0f ? weighted / sum : 0.0f;
    }
    return values;
  }

  std::vector<Box> decodeRaw(const std::vector<RawOutput> &outputs,
                             int image_width, int image_height, float ratio,
                             int top, int left, float threshold,
                             float iou_threshold) const {
    std::vector<Box> boxes;
    const float inverse_threshold =
        threshold <= 0.0f ? -std::numeric_limits<float>::infinity()
                          : (threshold >= 1.0f
                                 ? std::numeric_limits<float>::infinity()
                                 : std::log(threshold / (1.0f - threshold)));

    for (const auto &branch : branches_) {
      const RawOutput &bbox_out = outputs[branch.bbox_index];
      const RawOutput &cls_out = outputs[branch.cls_index];
      const RawOutput *angle_out = obb_mode_ ? &outputs[branch.angle_index] : nullptr;
      const int anchor_count = branch.feat_w * branch.feat_h;
      const bool quantized_classes = isQuantized(cls_out) && cls_out.scale > 0.0f;
      int quantized_threshold = std::numeric_limits<int>::min();
      if (quantized_classes) {
        quantized_threshold = static_cast<int>(std::lround(
            inverse_threshold / cls_out.scale + cls_out.zero_point));
        const int low = cls_out.dtype == BM_INT8 ? -128 : 0;
        const int high = cls_out.dtype == BM_INT8 ? 127 : 255;
        quantized_threshold = std::max(low, std::min(high, quantized_threshold));
      }

      for (int anchor = 0; anchor < anchor_count; ++anchor) {
        int best_class = -1;
        float best_logit = -std::numeric_limits<float>::infinity();
        int best_quantized = std::numeric_limits<int>::min();
        for (int cls = 0; cls < num_classes_; ++cls) {
          const int channel = branch.cls_offset + cls;
          const size_t index = static_cast<size_t>(channel * anchor_count + anchor);
          if (quantized_classes) {
            const int value = rawQuantizedValue(cls_out, index);
            if (value > best_quantized) {
              best_quantized = value;
              best_class = cls;
            }
          } else {
            const float value = rawValue(cls_out, index);
            if (value > best_logit) {
              best_logit = value;
              best_class = cls;
            }
          }
        }
        if (best_class < 0 ||
            (quantized_classes ? best_quantized < quantized_threshold
                               : best_logit < inverse_threshold)) {
          continue;
        }
        if (quantized_classes) {
          best_logit =
              (best_quantized - cls_out.zero_point) * cls_out.scale;
        }
        const float score = sigmoid(best_logit);
        if (score < threshold) {
          continue;
        }

        const std::vector<float> distances =
            decodeDflRaw(bbox_out, anchor, anchor_count);
        const int anchor_y = anchor / branch.feat_w;
        const int anchor_x = anchor % branch.feat_w;
        const float grid_x = static_cast<float>(anchor_x) + 0.5f;
        const float grid_y = static_cast<float>(anchor_y) + 0.5f;
        const float x1 = (grid_x - distances[0]) * branch.stride;
        const float y1 = (grid_y - distances[1]) * branch.stride;
        const float x2 = (grid_x + distances[2]) * branch.stride;
        const float y2 = (grid_y + distances[3]) * branch.stride;

        Box box;
        if (obb_mode_ && angle_out) {
          const float angle =
              (sigmoid(rawValue(*angle_out, static_cast<size_t>(anchor))) - 0.25f) *
              static_cast<float>(CV_PI);
          box = decodeOrientedBox(distances, angle, grid_x, grid_y, branch,
                                  image_width, image_height, ratio, top, left,
                                  best_class, score);
        } else {
          box.class_id = best_class;
          box.score = score;
          box.x1 = std::max(0.0f, std::min((x1 - left) / ratio,
                                            static_cast<float>(image_width)));
          box.y1 = std::max(0.0f, std::min((y1 - top) / ratio,
                                            static_cast<float>(image_height)));
          box.x2 = std::max(0.0f, std::min((x2 - left) / ratio,
                                            static_cast<float>(image_width)));
          box.y2 = std::max(0.0f, std::min((y2 - top) / ratio,
                                            static_cast<float>(image_height)));
        }
        boxes.push_back(box);
      }
    }
    return nonMaxSuppression(boxes, iou_threshold);
  }

  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  int input_height_ = 0;
  int input_width_ = 0;
  int output_count_ = 0;
  int num_classes_ = 0;
  bm_data_type_t input_dtype_ = BM_UINT8;
  ModelDescriptor descriptor_;
  std::vector<std::string> labels_;
  std::vector<OutputBranch> branches_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::vector<bm_device_mem_t> output_memories_;
  std::string hardware_error_;
  std::mutex infer_mutex_;
  ProfileStats profile_;
  bool obb_mode_ = false;
  bool opened_ = false;
};

NnYolov8::NnYolov8(std::string model_type) : model_type_(std::move(model_type)) {}

NnYolov8::~NnYolov8() = default;

TaskType NnYolov8::task() const { return TaskType::Detection; }

std::string NnYolov8::modelType() const { return model_type_; }

bool NnYolov8::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    setError(error, "YOLOv8 runtime requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnYolov8::shouldUseCustomRuntime() const {
  if (descriptor_loaded_ && !descriptor_.runtime.empty() &&
      startsWith(toUpper(descriptor_.runtime), "YOLOV8")) {
    return !descriptor_.model_path.empty();
  }
  std::string model_name = model_type_;
  if (model_name.empty() && descriptor_loaded_) {
    model_name = descriptor_.model_type;
  }
  model_name = toUpper(model_name);
  if (!startsWith(model_name, "YOLOV8")) {
    return false;
  }
  return descriptor_loaded_ && !descriptor_.model_path.empty();
}

bool NnYolov8::load(EngineConfig config, std::string *error) {
  // Release the previous VPSS group/model before accepting a new config.
  initialized_ = false;
  custom_runtime_.reset();
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
    return false;
  }

  if (!shouldUseCustomRuntime()) {
    setError(error,
             "YOLOv8 descriptor is incomplete or model_type does not map to YOLOv8");
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

bool NnYolov8::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnYolov8::detect(const std::string &image_path, const InferOptions &options,
                      AlgorithmResult *result, std::string *error) {
  return predict(image_path, options, result, error);
}

bool NnYolov8::predict(const std::string &image_path, const InferOptions &options,
                       AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnYolov8::predictFrame(const Frame &frame, const InferOptions &options,
                            AlgorithmResult *result, std::string *error) {
  if (!initialized_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (!custom_runtime_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, options, result, error);
  }
  if (frame.native) {
    return custom_runtime_->inferFrame(frame, options, result, error);
  }
  setError(error, "YOLOv8 frame has neither native data nor image_path");
  return false;
}

}  // namespace tdl_app
