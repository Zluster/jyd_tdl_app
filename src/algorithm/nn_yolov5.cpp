#include "tdl_app/nn_yolov5.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace tdl_app {
namespace {

constexpr int kMaxDims = 8;
constexpr int kInputChannels = 3;
constexpr int kBoxElementCount = 5;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

bool debugEnabled() {
  const char *value = std::getenv("TDL_APP_YOLOV5_DEBUG");
  return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

template <typename... Args>
void debugLog(const char *fmt, Args... args) {
  if (!debugEnabled()) {
    return;
  }
  std::fprintf(stderr, "[yolov5-debug] ");
  std::fprintf(stderr, fmt, args...);
  std::fprintf(stderr, "\n");
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

bool copyPackedRgbToBgr(const VIDEO_FRAME_S &vf, unsigned char *mapped,
                        int width, int height, bool input_is_bgr,
                        cv::Mat *image) {
  cv::Mat output(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    const unsigned char *src = mapped + y * vf.u32Stride[0];
    unsigned char *dst = output.ptr<unsigned char>(y);
    if (input_is_bgr) {
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
  return true;
}

bool copyPlanarRgbToBgr(const VIDEO_FRAME_S &vf, unsigned char *mapped,
                        int width, int height, bool input_is_bgr,
                        cv::Mat *image) {
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
      if (input_is_bgr) {
        dst[x] = cv::Vec3b(src0[x], src1[x], src2[x]);
      } else {
        dst[x] = cv::Vec3b(src2[x], src1[x], src0[x]);
      }
    }
  }
  *image = std::move(output);
  return true;
}

bool videoFrameToBgrMat(const VIDEO_FRAME_INFO_S &video_frame, cv::Mat *image,
                        std::string *error) {
  if (!image) {
    setError(error, "output image pointer is null");
    return false;
  }
  const auto &vf = video_frame.stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);
  if (width <= 0 || height <= 0) {
    setError(error, "invalid frame size");
    return false;
  }

  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    setError(error, "frame buffer length is zero");
    return false;
  }

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    setError(error, "CVI_SYS_Mmap failed");
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  bool ok = true;
  if (format == PIXEL_FORMAT_BGR_888) {
    ok = copyPackedRgbToBgr(vf, mapped, width, height, true, image);
  } else if (format == PIXEL_FORMAT_RGB_888) {
    ok = copyPackedRgbToBgr(vf, mapped, width, height, false, image);
  } else if (format == PIXEL_FORMAT_BGR_888_PLANAR) {
    ok = copyPlanarRgbToBgr(vf, mapped, width, height, true, image);
  } else if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
    ok = copyPlanarRgbToBgr(vf, mapped, width, height, false, image);
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
    const int code = format == PIXEL_FORMAT_NV21
                         ? cv::COLOR_YUV2BGR_NV21
                         : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(yuv, *image, code);
  } else {
    ok = false;
    setError(error,
             "custom YOLOv5 runtime only supports RGB/BGR/NV12/NV21/YUV400 frame input");
  }

  CVI_SYS_Munmap(mapped, map_size);
  if (!ok || image->empty()) {
    setError(error, "failed to convert frame to BGR image");
    return false;
  }
  return true;
}

bool frameToBgrMat(const Frame &frame, cv::Mat *image, std::string *error) {
  if (!frame.native) {
    setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
    return false;
  }
  auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
  return videoFrameToBgrMat(*video, image, error);
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
                     bool *is_nchw, std::string *error) {
  if (!height || !width || !is_nchw) {
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
    *is_nchw = true;
    return true;
  }
  if (shape.dims[3] == kInputChannels) {
    *height = shape.dims[1];
    *width = shape.dims[2];
    *is_nchw = false;
    return true;
  }

  setError(error, "unable to infer YOLOv5 input layout from tensor shape");
  return false;
}

std::vector<std::array<float, 2>> parseAnchors(const ModelDescriptor &descriptor) {
  std::vector<std::array<float, 2>> anchors;
  for (size_t i = 0; i + 1 < descriptor.anchors.size(); i += 2) {
    anchors.push_back({descriptor.anchors[i], descriptor.anchors[i + 1]});
  }
  return anchors;
}

template <typename T>
T clampCast(float value) {
  const float low = static_cast<float>(std::numeric_limits<T>::lowest());
  const float high = static_cast<float>(std::numeric_limits<T>::max());
  value = std::max(low, std::min(high, value));
  return static_cast<T>(std::lrint(value));
}

}  // namespace

class NnYolov5::CustomRuntime {
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

    const std::string model_path = resolveModelPath(descriptor);
    debugLog("open model_path=%s firmware=%s", model_path.c_str(),
             config.bmrt_firmware.c_str());
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
      setError(error, "custom YOLOv5 runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->stage_num < 1) {
      setError(error, "invalid network stage info");
      return false;
    }

    if (!parseInputShape(net_info_->stages[0].input_shapes[0], &input_height_,
                         &input_width_, &input_is_nchw_, error)) {
      return false;
    }
    input_dtype_ = net_info_->input_dtypes[0];
    output_count_ = net_info_->output_num;
    if (output_count_ <= 0) {
      setError(error, "custom YOLOv5 runtime has no outputs");
      return false;
    }

    anchors_ = parseAnchors(descriptor);
    if (anchors_.size() != 9) {
      setError(error, "YOLOv5 custom descriptor must provide 9 anchors");
      return false;
    }
    labels_ = descriptor.labels;
    debugLog("open input=%dx%d layout=%s input_dtype=%d outputs=%d labels=%d",
             input_width_, input_height_, input_is_nchw_ ? "NCHW" : "NHWC",
             static_cast<int>(input_dtype_), output_count_,
             static_cast<int>(labels_.size()));
    opened_ = true;
    return true;
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    if (!opened_) {
      setError(error, "custom YOLOv5 runtime is not initialized");
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
    debugLog("inferImage path=%s size=%dx%d", image_path.c_str(), image.cols,
             image.rows);

    return inferMat(image, options, result, error);
  }

  bool inferMat(const cv::Mat &image, const InferOptions &options,
                AlgorithmResult *result, std::string *error) {
    if (!opened_) {
      setError(error, "custom YOLOv5 runtime is not initialized");
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

    std::vector<float> input_data;
    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    preprocess(image, &input_data, &ratio, &top, &left);
    debugLog("inferMat image=%dx%d threshold=%.4f iou=%.4f ratio=%.6f top=%d left=%d",
             image.cols, image.rows, options.threshold, options.iou_threshold,
             ratio, top, left);

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
  }

  void preprocess(const cv::Mat &image, std::vector<float> *data, float *ratio,
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

    data->assign(static_cast<size_t>(input_height_ * input_width_ * kInputChannels), 0.0f);
    size_t index = 0;
    if (input_is_nchw_) {
      for (int c = 0; c < kInputChannels; ++c) {
        for (int y = 0; y < input_height_; ++y) {
          for (int x = 0; x < input_width_; ++x) {
            (*data)[index++] = static_cast<float>(rgb.at<cv::Vec3b>(y, x)[c]);
          }
        }
      }
    } else {
      for (int y = 0; y < input_height_; ++y) {
        for (int x = 0; x < input_width_; ++x) {
          const cv::Vec3b pixel = rgb.at<cv::Vec3b>(y, x);
          for (int c = 0; c < kInputChannels; ++c) {
            (*data)[index++] = static_cast<float>(pixel[c]);
          }
        }
      }
    }
  }

  bool launch(const std::vector<float> &input_data,
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

    if (input_dtype_ == BM_FLOAT32) {
      input_bytes.resize(input_data.size() * sizeof(float));
      auto *dst = reinterpret_cast<float *>(input_bytes.data());
      for (size_t i = 0; i < input_data.size(); ++i) {
        dst[i] = input_data[i] / 255.0f;
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_data.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_data.size(); ++i) {
        const float q = input_scale == 0.0f
                            ? input_data[i]
                            : input_data[i] / input_scale + input_zero_point;
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_data.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_data.size(); ++i) {
        dst[i] = clampCast<uint8_t>(input_data[i]);
      }
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "custom YOLOv5 runtime does not support this input dtype yet");
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
          decoded[j] = (static_cast<int>(raw[j]) - output_zero_point) *
                       output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *raw =
            reinterpret_cast<const uint8_t *>(output_bytes[i].data());
        for (size_t j = 0; j < element_count; ++j) {
          decoded[j] = (static_cast<int>(raw[j]) - output_zero_point) *
                       output_scale;
        }
      } else {
        setError(error, "custom YOLOv5 runtime does not support this output dtype yet");
        return false;
      }
      if (!decoded.empty()) {
        float min_value = decoded[0];
        float max_value = decoded[0];
        for (size_t j = 1; j < decoded.size(); ++j) {
          min_value = std::min(min_value, decoded[j]);
          max_value = std::max(max_value, decoded[j]);
        }
        debugLog("output[%d] dims=%d,%d,%d,%d dtype=%d scale=%.8f zp=%d min=%.6f max=%.6f v0=%.6f v1=%.6f v2=%.6f",
                 i,
                 (*output_shapes)[i].num_dims > 0 ? (*output_shapes)[i].dims[0] : 0,
                 (*output_shapes)[i].num_dims > 1 ? (*output_shapes)[i].dims[1] : 0,
                 (*output_shapes)[i].num_dims > 2 ? (*output_shapes)[i].dims[2] : 0,
                 (*output_shapes)[i].num_dims > 3 ? (*output_shapes)[i].dims[3] : 0,
                 static_cast<int>(net_info_->output_dtypes[i]), output_scale,
                 output_zero_point, min_value, max_value, decoded[0],
                 decoded.size() > 1 ? decoded[1] : 0.0f,
                 decoded.size() > 2 ? decoded[2] : 0.0f);
      }
      outputs->push_back(std::move(decoded));
    }
    return true;
  }

  std::vector<Box> decode(const std::vector<std::vector<float>> &outputs,
                          const std::vector<bm_shape_t> &output_shapes,
                          int image_width, int image_height, float ratio, int top,
                          int left, float threshold, float iou_threshold) const {
    std::vector<Box> boxes;
    int candidates_above_threshold = 0;
    float max_score = 0.0f;
    const int classes =
        !labels_.empty() ? static_cast<int>(labels_.size()) : 80;
    const int per_anchor = kBoxElementCount + classes;

    struct SplitBranch {
      int ny = 0;
      int nx = 0;
      int bbox_index = -1;
      int obj_index = -1;
      int cls_index = -1;
    };

    std::vector<SplitBranch> split_branches;
    if (outputs.size() == output_shapes.size()) {
      for (size_t i = 0; i < output_shapes.size(); ++i) {
        const bm_shape_t &shape = output_shapes[i];
        if (shape.num_dims != 4 || shape.dims[0] != 3) {
          continue;
        }
        const int ny = shape.dims[1];
        const int nx = shape.dims[2];
        const int channels = shape.dims[3];
        if (ny <= 0 || nx <= 0) {
          continue;
        }
        int branch_index = -1;
        for (size_t j = 0; j < split_branches.size(); ++j) {
          if (split_branches[j].ny == ny && split_branches[j].nx == nx) {
            branch_index = static_cast<int>(j);
            break;
          }
        }
        if (branch_index < 0) {
          split_branches.push_back(SplitBranch{ny, nx, -1, -1, -1});
          branch_index = static_cast<int>(split_branches.size() - 1);
        }
        if (channels == 4) {
          split_branches[branch_index].bbox_index = static_cast<int>(i);
        } else if (channels == 1) {
          split_branches[branch_index].obj_index = static_cast<int>(i);
        } else if (channels == classes) {
          split_branches[branch_index].cls_index = static_cast<int>(i);
        }
      }
    }

    bool used_split_decode = false;
    for (const auto &branch : split_branches) {
      if (branch.bbox_index < 0 || branch.obj_index < 0 || branch.cls_index < 0) {
        continue;
      }
      used_split_decode = true;
      const int ny = branch.ny;
      const int nx = branch.nx;
      const int stride = input_height_ / ny;
      int anchor_group = 0;
      if (stride <= 8) {
        anchor_group = 0;
      } else if (stride <= 16) {
        anchor_group = 1;
      } else {
        anchor_group = 2;
      }

      const std::vector<float> &bbox = outputs[branch.bbox_index];
      const std::vector<float> &obj = outputs[branch.obj_index];
      const std::vector<float> &cls = outputs[branch.cls_index];
      debugLog("decode split branch ny=%d nx=%d stride=%d bbox=%d obj=%d cls=%d",
               ny, nx, stride, branch.bbox_index, branch.obj_index,
               branch.cls_index);

      for (int anchor_idx = 0; anchor_idx < 3; ++anchor_idx) {
        const auto &anchor = anchors_[anchor_group * 3 + anchor_idx];
        for (int y = 0; y < ny; ++y) {
          for (int x = 0; x < nx; ++x) {
            const size_t cell_index = static_cast<size_t>((anchor_idx * ny + y) * nx + x);
            const size_t bbox_offset = cell_index * 4;
            const float obj_score = sigmoid(obj[cell_index]);
            if (obj_score <= 0.0f) {
              continue;
            }

            int best_class = 0;
            float best_class_score = 0.0f;
            const size_t cls_offset = cell_index * static_cast<size_t>(classes);
            for (int cls_index = 0; cls_index < classes; ++cls_index) {
              const float cls_score = sigmoid(cls[cls_offset + cls_index]);
              if (cls_score > best_class_score) {
                best_class_score = cls_score;
                best_class = cls_index;
              }
            }

            const float score = obj_score * best_class_score;
            if (score > max_score) {
              max_score = score;
            }
            if (score < threshold) {
              continue;
            }
            ++candidates_above_threshold;

            const float cx =
                (sigmoid(bbox[bbox_offset + 0]) * 2.0f - 0.5f + x) * stride;
            const float cy =
                (sigmoid(bbox[bbox_offset + 1]) * 2.0f - 0.5f + y) * stride;
            const float w =
                std::pow(sigmoid(bbox[bbox_offset + 2]) * 2.0f, 2.0f) * anchor[0];
            const float h =
                std::pow(sigmoid(bbox[bbox_offset + 3]) * 2.0f, 2.0f) * anchor[1];

            Box box;
            box.class_id = best_class;
            box.score = score;
            box.x1 = ((cx - w * 0.5f) - left) / ratio;
            box.y1 = ((cy - h * 0.5f) - top) / ratio;
            box.x2 = ((cx + w * 0.5f) - left) / ratio;
            box.y2 = ((cy + h * 0.5f) - top) / ratio;
            box.x1 = std::max(0.0f, std::min(box.x1, static_cast<float>(image_width)));
            box.y1 = std::max(0.0f, std::min(box.y1, static_cast<float>(image_height)));
            box.x2 = std::max(0.0f, std::min(box.x2, static_cast<float>(image_width)));
            box.y2 = std::max(0.0f, std::min(box.y2, static_cast<float>(image_height)));
            boxes.push_back(box);
          }
        }
      }
    }

    if (!used_split_decode) {
      for (const auto &output : outputs) {
        if (output.empty()) {
          continue;
        }

      int ny = 0;
      int nx = 0;
      int stride = 0;
      const size_t count = output.size();
      if (count % (3 * per_anchor) != 0) {
        continue;
      }

      const int grid = static_cast<int>(count / (3 * per_anchor));
      for (int candidate = 1; candidate * candidate <= grid; ++candidate) {
        if (grid % candidate != 0) {
          continue;
        }
        const int other = grid / candidate;
        if (candidate > ny) {
          ny = candidate;
          nx = other;
        }
      }
      if (ny <= 0 || nx <= 0) {
        continue;
      }

      stride = input_height_ / ny;
      int anchor_group = 0;
      if (stride <= 8) {
        anchor_group = 0;
      } else if (stride <= 16) {
        anchor_group = 1;
      } else {
        anchor_group = 2;
      }

      for (int anchor_idx = 0; anchor_idx < 3; ++anchor_idx) {
        const auto &anchor = anchors_[anchor_group * 3 + anchor_idx];
        for (int y = 0; y < ny; ++y) {
          for (int x = 0; x < nx; ++x) {
            const size_t offset =
                (((anchor_idx * ny + y) * nx + x) * per_anchor);
            const float obj = sigmoid(output[offset + 4]);
            if (obj <= 0.0f) {
              continue;
            }

            int best_class = 0;
            float best_score = 0.0f;
            for (int cls = 0; cls < classes; ++cls) {
              const float cls_score = sigmoid(output[offset + 5 + cls]);
              if (cls_score > best_score) {
                best_score = cls_score;
                best_class = cls;
              }
            }

            const float score = obj * best_score;
            if (score > max_score) {
              max_score = score;
            }
            if (score < threshold) {
              continue;
            }
            ++candidates_above_threshold;

            const float cx = (sigmoid(output[offset + 0]) * 2.0f - 0.5f + x) * stride;
            const float cy = (sigmoid(output[offset + 1]) * 2.0f - 0.5f + y) * stride;
            const float w =
                std::pow(sigmoid(output[offset + 2]) * 2.0f, 2.0f) * anchor[0];
            const float h =
                std::pow(sigmoid(output[offset + 3]) * 2.0f, 2.0f) * anchor[1];

            Box box;
            box.class_id = best_class;
            box.score = score;
            box.x1 = ((cx - w * 0.5f) - left) / ratio;
            box.y1 = ((cy - h * 0.5f) - top) / ratio;
            box.x2 = ((cx + w * 0.5f) - left) / ratio;
            box.y2 = ((cy + h * 0.5f) - top) / ratio;
            box.x1 = std::max(0.0f, std::min(box.x1, static_cast<float>(image_width)));
            box.y1 = std::max(0.0f, std::min(box.y1, static_cast<float>(image_height)));
            box.x2 = std::max(0.0f, std::min(box.x2, static_cast<float>(image_width)));
            box.y2 = std::max(0.0f, std::min(box.y2, static_cast<float>(image_height)));
            boxes.push_back(box);
          }
        }
      }
      }
    }

    std::vector<Box> kept = nonMaxSuppression(boxes, iou_threshold);
    debugLog("decode raw_boxes=%d kept_boxes=%d max_score=%.6f threshold=%.4f",
             candidates_above_threshold, static_cast<int>(kept.size()),
             max_score, threshold);
    return kept;
  }

  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  int input_height_ = 0;
  int input_width_ = 0;
  bool input_is_nchw_ = true;
  int output_count_ = 0;
  bm_data_type_t input_dtype_ = BM_FLOAT32;
  std::vector<std::array<float, 2>> anchors_;
  std::vector<std::string> labels_;
  bool opened_ = false;
};

NnYolov5::NnYolov5(std::string model_type) : model_type_(std::move(model_type)) {}

NnYolov5::~NnYolov5() = default;

TaskType NnYolov5::task() const { return TaskType::Detection; }

std::string NnYolov5::modelType() const { return model_type_; }

bool NnYolov5::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    setError(error, "YOLOv5 runtime requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnYolov5::shouldUseCustomRuntime() const {
  if (descriptor_loaded_ && !descriptor_.runtime.empty() &&
      startsWith(toUpper(descriptor_.runtime), "YOLOV5")) {
    return !descriptor_.model_path.empty();
  }
  std::string model_name = model_type_;
  if (model_name.empty() && descriptor_loaded_) {
    model_name = descriptor_.model_type;
  }
  model_name = toUpper(model_name);
  if (!startsWith(model_name, "YOLOV5")) {
    return false;
  }
  return descriptor_loaded_ && !descriptor_.model_path.empty();
}

bool NnYolov5::load(EngineConfig config, std::string *error) {
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
    return false;
  }

  if (!shouldUseCustomRuntime()) {
    setError(error,
             "YOLOv5 descriptor is incomplete or model_type does not map to YOLOv5");
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

bool NnYolov5::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnYolov5::detect(const std::string &image_path, const InferOptions &options,
                      AlgorithmResult *result, std::string *error) {
  return predict(image_path, options, result, error);
}

bool NnYolov5::predict(const std::string &image_path, const InferOptions &options,
                       AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnYolov5::predictFrame(const Frame &frame, const InferOptions &options,
                            AlgorithmResult *result, std::string *error) {
  if (!custom_runtime_ || !initialized_) {
    setError(error, "model is not initialized");
    return false;
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, options, result, error);
  }
  cv::Mat image;
  if (!frameToBgrMat(frame, &image, error)) {
    return false;
  }
  return custom_runtime_->inferMat(image, options, result, error);
}

}  // namespace tdl_app
