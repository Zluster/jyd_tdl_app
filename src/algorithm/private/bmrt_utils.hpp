#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace bmrt_runtime {

constexpr int kInputChannels = 3;

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

inline std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return value;
}

inline bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

template <typename T>
inline T clampCast(float value) {
  const float low = static_cast<float>(std::numeric_limits<T>::lowest());
  const float high = static_cast<float>(std::numeric_limits<T>::max());
  value = std::max(low, std::min(high, value));
  return static_cast<T>(std::lrint(value));
}

inline bool parseInputShape(const bm_shape_t &shape, int *height, int *width,
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
  setError(error, "unable to infer input layout from tensor shape");
  return false;
}

inline std::vector<float> expandChannelValues(const std::vector<float> &values,
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

inline bool wantsRgbInput(const ModelDescriptor &descriptor,
                          bool default_rgb = true) {
  if (descriptor.input_type.empty()) {
    return default_rgb;
  }
  return toUpper(descriptor.input_type) != "BGR";
}

inline void writeImageToTensor(const cv::Mat &image, bool rgb_input,
                               bool nchw_layout,
                               const std::vector<float> &mean,
                               const std::vector<float> &scale,
                               std::vector<float> *tensor) {
  cv::Mat prepared;
  if (rgb_input) {
    cv::cvtColor(image, prepared, cv::COLOR_BGR2RGB);
  } else {
    prepared = image;
  }

  tensor->assign(static_cast<size_t>(image.cols * image.rows * kInputChannels),
                 0.0f);
  if (nchw_layout) {
    size_t index = 0;
    for (int c = 0; c < kInputChannels; ++c) {
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
      for (int c = 0; c < kInputChannels; ++c) {
        (*tensor)[index++] =
            (static_cast<float>(pixel[c]) - mean[c]) * scale[c];
      }
    }
  }
}

inline cv::Rect clampRoi(const Box &box, int image_width, int image_height) {
  if (image_width <= 0 || image_height <= 0) {
    return cv::Rect();
  }
  const int x1 =
      std::max(0, std::min(static_cast<int>(std::floor(box.x1)), image_width - 1));
  const int y1 =
      std::max(0, std::min(static_cast<int>(std::floor(box.y1)), image_height - 1));
  const int x2 =
      std::max(0, std::min(static_cast<int>(std::ceil(box.x2)), image_width));
  const int y2 =
      std::max(0, std::min(static_cast<int>(std::ceil(box.y2)), image_height));
  const int width = std::max(1, x2 - x1);
  const int height = std::max(1, y2 - y1);
  return cv::Rect(x1, y1, width, height);
}

struct OutputTensor {
  bm_shape_t shape{};
  std::vector<float> data;
};

class Session {
 public:
  Session() = default;
  ~Session() { close(); }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

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
      setError(error, "runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->output_num < 1) {
      setError(error, "runtime requires at least one output");
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

    input_dtype_ = net_info_->input_dtypes[0];
    opened_ = true;
    return true;
  }

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
    net_name_.clear();
    input_height_ = 0;
    input_width_ = 0;
    nchw_layout_ = true;
    input_dtype_ = BM_FLOAT32;
    opened_ = false;
  }

  bool launch(const std::vector<float> &input_tensor,
              std::vector<OutputTensor> *outputs, std::string *error) const {
    if (!opened_) {
      setError(error, "runtime session is not initialized");
      return false;
    }
    if (!outputs) {
      setError(error, "output tensor vector is null");
      return false;
    }

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
            input_scale == 0.0f ? input_tensor[i]
                                : input_tensor[i] / input_scale + input_zero_point;
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            input_scale == 0.0f ? input_tensor[i]
                                : input_tensor[i] / input_scale + input_zero_point;
        dst[i] = clampCast<uint8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "runtime does not support this input dtype");
      return false;
    }

    std::vector<std::vector<uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num), std::vector<uint8_t>());
    std::vector<void *> output_ptrs(static_cast<size_t>(net_info_->output_num),
                                    nullptr);
    std::vector<bm_shape_t> output_shapes(
        static_cast<size_t>(net_info_->output_num), bm_shape_t{});

    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[static_cast<size_t>(i)].resize(net_info_->max_output_bytes[i]);
      output_ptrs[static_cast<size_t>(i)] =
          output_bytes[static_cast<size_t>(i)].data();
      std::memset(&output_shapes[static_cast<size_t>(i)], 0, sizeof(bm_shape_t));
    }

    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      setError(error, "bmrt_launch_data failed");
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

      OutputTensor out;
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
        setError(error, "runtime does not support this output dtype");
        return false;
      }
      outputs->push_back(std::move(out));
    }

    return true;
  }

  const bm_net_info_t *netInfo() const { return net_info_; }
  int inputHeight() const { return input_height_; }
  int inputWidth() const { return input_width_; }
  bool nchwLayout() const { return nchw_layout_; }
  bool opened() const { return opened_; }

 private:
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  int input_height_ = 0;
  int input_width_ = 0;
  bool nchw_layout_ = true;
  bm_data_type_t input_dtype_ = BM_FLOAT32;
  bool opened_ = false;
};

}  // namespace bmrt_runtime
}  // namespace tdl_app
