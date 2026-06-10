#include "tdl_app/single_object_tracker.hpp"

#include <algorithm>
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
#include "algorithm/private/tdl_sdk_utils.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kTrackerInputChannels = 3;
constexpr float kContextAmount = 0.5f;
constexpr float kTemplateScale = 2.0f;
constexpr float kSearchScale = 4.0f;
constexpr float kWindowInfluence = 0.40f;
constexpr float kPenaltyK = 0.05f;
constexpr float kTrackLr = 0.35f;

bool trackerDebugEnabled() {
  const char *value = std::getenv("TDL_APP_TRACK_DEBUG");
  if (!value) {
    return false;
  }
  return std::string(value) != "0";
}

bool frameToBgrMat(const Frame &frame, cv::Mat *image, std::string *error) {
  if (!frame.native) {
    private_tdl_sdk::setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
    return false;
  }

  const auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
  const auto &vf = video->stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);
  if (width <= 0 || height <= 0) {
    private_tdl_sdk::setError(error, "invalid frame size");
    return false;
  }

  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    private_tdl_sdk::setError(error, "frame buffer length is zero");
    return false;
  }

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    private_tdl_sdk::setError(error, "CVI_SYS_Mmap failed");
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  bool ok = true;
  if (format == PIXEL_FORMAT_BGR_888 || format == PIXEL_FORMAT_RGB_888) {
    cv::Mat output(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
      const unsigned char *src = mapped + y * vf.u32Stride[0];
      unsigned char *dst = output.ptr<unsigned char>(y);
      if (format == PIXEL_FORMAT_BGR_888) {
        std::memcpy(dst, src, static_cast<size_t>(width) * 3);
      } else {
        for (int x = 0; x < width; ++x) {
          dst[x * 3 + 0] = src[x * 3 + 2];
          dst[x * 3 + 1] = src[x * 3 + 1];
          dst[x * 3 + 2] = src[x * 3 + 0];
        }
      }
    }
    *image = std::move(output);
  } else if (format == PIXEL_FORMAT_BGR_888_PLANAR ||
             format == PIXEL_FORMAT_RGB_888_PLANAR) {
    const unsigned char *plane0 = mapped;
    const unsigned char *plane1 = mapped + vf.u32Length[0];
    const unsigned char *plane2 = plane1 + vf.u32Length[1];
    cv::Mat output(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
      const unsigned char *src0 = plane0 + y * vf.u32Stride[0];
      const unsigned char *src1 = plane1 + y * vf.u32Stride[1];
      const unsigned char *src2 = plane2 + y * vf.u32Stride[2];
      cv::Vec3b *dst = output.ptr<cv::Vec3b>(y);
      for (int x = 0; x < width; ++x) {
        if (format == PIXEL_FORMAT_BGR_888_PLANAR) {
          dst[x] = cv::Vec3b(src0[x], src1[x], src2[x]);
        } else {
          dst[x] = cv::Vec3b(src2[x], src1[x], src0[x]);
        }
      }
    }
    *image = std::move(output);
  } else if (format == PIXEL_FORMAT_YUV_400) {
    cv::Mat gray(height, width, CV_8UC1);
    for (int y = 0; y < height; ++y) {
      std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
    }
    cv::cvtColor(gray, *image, cv::COLOR_GRAY2BGR);
  } else if (format == PIXEL_FORMAT_NV12 || format == PIXEL_FORMAT_NV21) {
    cv::Mat yuv(height + height / 2, width, CV_8UC1);
    unsigned char *y_base = mapped;
    unsigned char *uv_base = mapped + vf.u32Length[0];
    for (int y = 0; y < height; ++y) {
      std::memcpy(yuv.ptr(y), y_base + y * vf.u32Stride[0], width);
    }
    for (int y = 0; y < height / 2; ++y) {
      std::memcpy(yuv.ptr(height + y), uv_base + y * vf.u32Stride[1], width);
    }
    const int code = format == PIXEL_FORMAT_NV21 ? cv::COLOR_YUV2BGR_NV21
                                                 : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(yuv, *image, code);
  } else {
    ok = false;
    private_tdl_sdk::setError(
        error, "single object tracker only supports RGB/BGR/NV12/NV21/YUV400 frame input");
  }

  CVI_SYS_Munmap(mapped, map_size);
  if (!ok || image->empty()) {
    private_tdl_sdk::setError(error, "failed to convert frame to BGR image");
    return false;
  }
  return true;
}

float clampFloat(float value, float low, float high) {
  return std::max(low, std::min(value, high));
}

float changeRatio(float value) {
  if (value <= 0.0f) {
    return std::numeric_limits<float>::infinity();
  }
  return std::max(value, 1.0f / value);
}

float sizeWithContext(float width, float height) {
  const float pad = 0.5f * (width + height);
  return std::sqrt((width + pad) * (height + pad));
}

std::vector<float> buildHannWindow(int width, int height) {
  std::vector<float> out(static_cast<size_t>(width * height), 0.0f);
  if (width <= 1 || height <= 1) {
    std::fill(out.begin(), out.end(), 1.0f);
    return out;
  }

  std::vector<float> hann_x(static_cast<size_t>(width), 0.0f);
  std::vector<float> hann_y(static_cast<size_t>(height), 0.0f);
  for (int x = 0; x < width; ++x) {
    hann_x[static_cast<size_t>(x)] =
        0.5f * (1.0f - std::cos(2.0f * static_cast<float>(CV_PI) * x /
                                static_cast<float>(width - 1)));
  }
  for (int y = 0; y < height; ++y) {
    hann_y[static_cast<size_t>(y)] =
        0.5f * (1.0f - std::cos(2.0f * static_cast<float>(CV_PI) * y /
                                static_cast<float>(height - 1)));
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      out[static_cast<size_t>(y * width + x)] =
          hann_y[static_cast<size_t>(y)] * hann_x[static_cast<size_t>(x)];
    }
  }
  return out;
}

Box clampBox(const Box &box, int width, int height) {
  Box out = box;
  out.x1 = clampFloat(out.x1, 0.0f, static_cast<float>(std::max(0, width - 1)));
  out.y1 = clampFloat(out.y1, 0.0f, static_cast<float>(std::max(0, height - 1)));
  out.x2 = clampFloat(out.x2, out.x1 + 1.0f, static_cast<float>(std::max(1, width)));
  out.y2 = clampFloat(out.y2, out.y1 + 1.0f, static_cast<float>(std::max(1, height)));
  return out;
}

float cropSideLength(const Box &box, float scale) {
  const float w = std::max(1.0f, box.width());
  const float h = std::max(1.0f, box.height());
  const float context = kContextAmount * (w + h);
  const float size = std::sqrt((w + context) * (h + context));
  return std::max(2.0f, size * scale);
}

cv::Mat cropSquarePatch(const cv::Mat &image, float center_x, float center_y,
                        float side_length, int output_size) {
  if (image.empty() || output_size <= 0) {
    return cv::Mat();
  }
  cv::Mat patch;
  const int patch_side = std::max(2, static_cast<int>(std::round(side_length)));
  cv::getRectSubPix(image, cv::Size(patch_side, patch_side),
                    cv::Point2f(center_x, center_y), patch);
  if (patch.empty()) {
    return patch;
  }
  cv::resize(patch, patch, cv::Size(output_size, output_size), 0, 0,
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

class MultiInputSession {
 public:
  ~MultiInputSession() { close(); }

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    close();

    bm_status_t status = bm_dev_request(&handle_, 0);
    if (status != BM_SUCCESS) {
      bmrt_runtime::setError(error, "bm_dev_request failed");
      return false;
    }

    if (!config.bmrt_firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.bmrt_firmware.c_str(), 0);
    }

    runtime_ = bmrt_create(handle_);
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

 private:
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  std::vector<int> input_heights_;
  std::vector<int> input_widths_;
  std::vector<std::uint8_t> input_nchw_;
  std::vector<bm_data_type_t> input_dtypes_;
  bool opened_ = false;
};

}  // namespace

class SingleObjectTracker::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    reset();

    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "TRACKING_FEARTRACK", error);
    if (model_type.empty()) {
      return false;
    }

    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
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
      private_tdl_sdk::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return initializeImage(image, target, error);
  }

  bool initializeFrame(const Frame &frame, const Box &target,
                       std::string *error) {
    if (!session_.opened()) {
      private_tdl_sdk::setError(error, "single object tracker is not initialized");
      return false;
    }
    cv::Mat image;
    if (!frame.image_path.empty()) {
      image = cv::imread(frame.image_path, cv::IMREAD_COLOR);
      if (image.empty()) {
        private_tdl_sdk::setError(error, "failed to read image: " + frame.image_path);
        return false;
      }
    } else if (!frameToBgrMat(frame, &image, error)) {
      return false;
    }
    return initializeImage(image, target, error);
  }

  bool run(const std::string &image_path, SingleObjectTrackingResult *result,
           std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      private_tdl_sdk::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return trackImage(image, result, error);
  }

  bool runFrame(const Frame &frame, SingleObjectTrackingResult *result,
                std::string *error) {
    if (!ready()) {
      private_tdl_sdk::setError(error, "single object tracker target is not initialized");
      return false;
    }
    cv::Mat image;
    if (!frame.image_path.empty()) {
      image = cv::imread(frame.image_path, cv::IMREAD_COLOR);
      if (image.empty()) {
        private_tdl_sdk::setError(error, "failed to read image: " + frame.image_path);
        return false;
      }
    } else if (!frameToBgrMat(frame, &image, error)) {
      return false;
    }
    return trackImage(image, result, error);
  }

  bool initialized() const { return session_.opened(); }
  bool ready() const { return initialized_ && current_box_.valid(); }
  std::string modelType() const { return model_type_; }
  Box currentBox() const { return current_box_; }

  void reset() {
    session_.close();
    descriptor_ = ModelDescriptor{};
    mean_.clear();
    scale_.clear();
    model_type_.clear();
    current_box_ = Box{};
    initialized_ = false;
    template_side_ = 0.0f;
  }

 private:
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
      private_tdl_sdk::setError(error, "single object tracker outputs are incomplete");
      return false;
    }
    hann_window_ = buildHannWindow(score_map_width_, score_map_height_);
    return true;
  }

  bool initializeImage(const cv::Mat &image, const Box &target,
                       std::string *error) {
    if (!session_.opened()) {
      private_tdl_sdk::setError(error, "single object tracker is not initialized");
      return false;
    }
    if (image.empty()) {
      private_tdl_sdk::setError(error, "initialize image is empty");
      return false;
    }
    current_box_ = clampBox(target, image.cols, image.rows);
    if (!current_box_.valid()) {
      private_tdl_sdk::setError(error, "initial target box is invalid");
      return false;
    }

    const float cx = (current_box_.x1 + current_box_.x2) * 0.5f;
    const float cy = (current_box_.y1 + current_box_.y2) * 0.5f;
    template_side_ = cropSideLength(current_box_, kTemplateScale);
    cv::Mat patch = cropSquarePatch(image, cx, cy, template_side_,
                                    session_.inputWidth(0));
    if (patch.empty()) {
      private_tdl_sdk::setError(error, "failed to crop tracker template patch");
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
      private_tdl_sdk::setError(error, "single object tracker target is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(error, "tracker result pointer is null");
      return false;
    }

    const float cx = (current_box_.x1 + current_box_.x2) * 0.5f;
    const float cy = (current_box_.y1 + current_box_.y2) * 0.5f;
    const float search_side = cropSideLength(current_box_, kSearchScale);
    cv::Mat patch = cropSquarePatch(image, cx, cy, search_side,
                                    session_.inputWidth(1));
    if (patch.empty()) {
      private_tdl_sdk::setError(error, "failed to crop tracker search patch");
      return false;
    }

    std::vector<float> search_tensor;
    writeImageToTensor(patch, bmrt_runtime::wantsRgbInput(descriptor_),
                       session_.inputNchw(1), mean_, scale_, &search_tensor);

    std::vector<std::vector<float>> inputs(2);
    inputs[0] = template_tensor_;
    inputs[1] = std::move(search_tensor);

    std::vector<MultiInputOutputTensor> outputs;
    if (!session_.launch(inputs, &outputs, error)) {
      return false;
    }

    const MultiInputOutputTensor &cls = outputs[static_cast<size_t>(cls_output_index_)];
    const MultiInputOutputTensor &bbox = outputs[static_cast<size_t>(bbox_output_index_)];
    if (cls.shape.num_dims != 4 || bbox.shape.num_dims != 4) {
      private_tdl_sdk::setError(error, "tracker output rank is invalid");
      return false;
    }

    const int score_h = cls.shape.dims[2];
    const int score_w = cls.shape.dims[3];
    const int bbox_h = bbox.shape.dims[2];
    const int bbox_w = bbox.shape.dims[3];
    if (score_h <= 0 || score_w <= 0 || score_h != bbox_h || score_w != bbox_w) {
      private_tdl_sdk::setError(error, "tracker output map size mismatch");
      return false;
    }

    int best_index = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < score_h * score_w; ++i) {
      const float score = cls.data[static_cast<size_t>(i)];
      if (score > best_score) {
        best_score = score;
        best_index = i;
      }
    }
    if (best_index < 0) {
      private_tdl_sdk::setError(error, "tracker score map is empty");
      return false;
    }

    const int best_y = best_index / score_w;
    const int best_x = best_index % score_w;
    const float stride_x = search_side / static_cast<float>(score_w);
    const float stride_y = search_side / static_cast<float>(score_h);
    const size_t map_size = static_cast<size_t>(bbox_h * bbox_w);
    const float box_scale =
        search_side / static_cast<float>(std::max(1, session_.inputWidth(1)));
    const float current_width = std::max(1.0f, current_box_.width());
    const float current_height = std::max(1.0f, current_box_.height());

    int selected_index = -1;
    float selected_penalized_score = -std::numeric_limits<float>::infinity();
    float selected_score = 0.0f;
    float selected_left = 0.0f;
    float selected_top = 0.0f;
    float selected_right = 0.0f;
    float selected_bottom = 0.0f;
    float selected_penalty = 1.0f;

    for (int i = 0; i < score_h * score_w; ++i) {
      const float score = cls.data[static_cast<size_t>(i)];
      const float left =
          bbox.data[0 * map_size + static_cast<size_t>(i)] * box_scale;
      const float top =
          bbox.data[1 * map_size + static_cast<size_t>(i)] * box_scale;
      const float right =
          bbox.data[2 * map_size + static_cast<size_t>(i)] * box_scale;
      const float bottom =
          bbox.data[3 * map_size + static_cast<size_t>(i)] * box_scale;
      const float pred_width = std::max(1.0f, left + right);
      const float pred_height = std::max(1.0f, top + bottom);
      const float scale_penalty =
          changeRatio(sizeWithContext(pred_width, pred_height) /
                      sizeWithContext(current_width, current_height));
      const float ratio_penalty =
          changeRatio((current_width / current_height) /
                      (pred_width / pred_height));
      const float penalty =
          std::exp(-(scale_penalty * ratio_penalty - 1.0f) * kPenaltyK);
      const float window =
          i < static_cast<int>(hann_window_.size()) ? hann_window_[static_cast<size_t>(i)]
                                                    : 0.0f;
      const float penalized_score =
          penalty * score * (1.0f - kWindowInfluence) +
          window * kWindowInfluence;
      if (penalized_score > selected_penalized_score) {
        selected_penalized_score = penalized_score;
        selected_index = i;
        selected_score = score;
        selected_left = left;
        selected_top = top;
        selected_right = right;
        selected_bottom = bottom;
        selected_penalty = penalty;
      }
    }
    if (selected_index < 0) {
      private_tdl_sdk::setError(error, "tracker score map is empty");
      return false;
    }

    const int selected_y = selected_index / score_w;
    const int selected_x = selected_index % score_w;
    const float patch_center_x =
        (static_cast<float>(selected_x) + 0.5f) * stride_x - search_side * 0.5f;
    const float patch_center_y =
        (static_cast<float>(selected_y) + 0.5f) * stride_y - search_side * 0.5f;
    const float pred_width = std::max(1.0f, selected_left + selected_right);
    const float pred_height = std::max(1.0f, selected_top + selected_bottom);
    const float pred_center_x =
        cx + patch_center_x + (selected_right - selected_left) * 0.5f;
    const float pred_center_y =
        cy + patch_center_y + (selected_bottom - selected_top) * 0.5f;
    const float lr = clampFloat(selected_penalty * selected_score * kTrackLr,
                                0.0f, 1.0f);
    const float next_width =
        current_width * (1.0f - lr) + pred_width * lr;
    const float next_height =
        current_height * (1.0f - lr) + pred_height * lr;

    Box next;
    next.x1 = pred_center_x - next_width * 0.5f;
    next.y1 = pred_center_y - next_height * 0.5f;
    next.x2 = pred_center_x + next_width * 0.5f;
    next.y2 = pred_center_y + next_height * 0.5f;
    next.score = selected_score;
    next.class_id = 0;
    next = clampBox(next, image.cols, image.rows);
    current_box_ = next;

    result->clear();
    result->box = next;
    result->confidence = selected_score;
    result->search_width = session_.inputWidth(1);
    result->search_height = session_.inputHeight(1);

    if (trackerDebugEnabled()) {
      std::cout << "track debug: score_map=" << score_w << "x" << score_h
                << " best=(" << selected_x << "," << selected_y << ") score="
                << selected_score << " penalty=" << selected_penalty
                << " lr=" << lr
                << " bbox=(" << selected_left << "," << selected_top << ","
                << selected_right << "," << selected_bottom
                << ") box=(" << next.x1 << "," << next.y1 << "," << next.x2
                << "," << next.y2 << ")\n";
    }
    return true;
  }

  MultiInputSession session_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  std::vector<float> template_tensor_;
  std::string model_type_;
  Box current_box_;
  float template_side_ = 0.0f;
  int cls_output_index_ = -1;
  int bbox_output_index_ = -1;
  int score_map_width_ = 0;
  int score_map_height_ = 0;
  std::vector<float> hann_window_;
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
    private_tdl_sdk::setError(error, "single object tracker is not initialized");
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
    private_tdl_sdk::setError(error, "single object tracker is not initialized");
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
    private_tdl_sdk::setError(error, "single object tracker is not initialized");
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
    private_tdl_sdk::setError(error, "single object tracker is not initialized");
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
