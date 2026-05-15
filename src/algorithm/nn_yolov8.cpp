#include "tdl_app/nn_yolov8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"

namespace tdl_app {
namespace {

constexpr int kInputChannels = 3;
constexpr int kRegMax = 16;
constexpr int kBoxChannels = 4 * kRegMax;

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
      if (intersectionOverUnion(boxes[index], boxes[other]) > iou_threshold) {
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
  struct OutputBranch {
    int stride = 0;
    int feat_w = 0;
    int feat_h = 0;
    int bbox_index = -1;
    int cls_index = -1;
    int cls_offset = 0;
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

    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    if (!opened_) {
      setError(error, "custom YOLOv8 runtime is not initialized");
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

 private:
  void close() {
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

  bool buildBranches(std::string *error) {
    branches_.clear();
    num_classes_ = static_cast<int>(labels_.size());

    struct TempBranch {
      int stride = 0;
      int feat_w = 0;
      int feat_h = 0;
      int bbox_index = -1;
      int cls_index = -1;
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
        temp.push_back(TempBranch{stride_h, feat_w, feat_h, -1, -1, 0});
        it = temp.end() - 1;
      }

      if (channel == kBoxChannels) {
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
      if (branch.bbox_index < 0 || branch.cls_index < 0) {
        setError(error, "incomplete YOLOv8 output branches");
        return false;
      }
      branches_.push_back(OutputBranch{branch.stride, branch.feat_w, branch.feat_h,
                                       branch.bbox_index, branch.cls_index,
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
  std::vector<std::string> labels_;
  std::vector<OutputBranch> branches_;
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
  if (!custom_runtime_ || !initialized_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (frame.image_path.empty()) {
    setError(error, "custom YOLOv8 runtime currently supports image_path only");
    return false;
  }
  return custom_runtime_->inferImage(frame.image_path, options, result, error);
}

}  // namespace tdl_app
