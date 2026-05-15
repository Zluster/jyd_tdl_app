#include "tdl_app/nn_classifier.hpp"

#include <algorithm>
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

}  // namespace

class NnClassifier::CustomRuntime {
 public:
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
    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
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

    const int top_k = std::max(1, options.top_k);
    const int count = std::min(top_k, static_cast<int>(order.size()));
    for (int i = 0; i < count; ++i) {
      ClassificationItem item;
      item.class_id = order[i];
      item.score = scores[order[i]];
      result->classes.push_back(item);
    }
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
    net_info_ = nullptr;
    opened_ = false;
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

    if (input_dtype_ == BM_FLOAT32) {
      input_ptrs[0] = const_cast<float *>(input_tensor.data());
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q = input_scale == 0.0f
                            ? input_tensor[i]
                            : input_tensor[i] / input_scale + input_zero_point;
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q = input_scale == 0.0f
                            ? input_tensor[i]
                            : input_tensor[i] / input_scale + input_zero_point;
        dst[i] = clampCast<uint8_t>(q);
      }
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
      outputs->push_back(std::move(decoded));
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
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
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
  if (!initialized_ || !custom_runtime_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (frame.image_path.empty()) {
    setError(error, "classifier runtime currently supports image_path only");
    return false;
  }
  return custom_runtime_->inferImage(frame.image_path, options, result, error);
}

}  // namespace tdl_app
