#include "tdl_app/nn_scrfd.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"

namespace tdl_app {
namespace {

struct ScrfdBranch {
  int stride = 0;
  int feat_w = 0;
  int feat_h = 0;
  int num_anchors = 2;
  bool channel_last = false;
  int score_index = -1;
  int bbox_index = -1;
  int landmark_index = -1;
};

float modelToImageCoordinate(float value, int padding, float ratio,
                             int image_extent) {
  const float mapped = (value - static_cast<float>(padding)) / ratio;
  return std::max(0.0f,
                  std::min(mapped, static_cast<float>(image_extent)));
}

float intersectionOverUnion(const Box &lhs, const Box &rhs) {
  const float x1 = std::max(lhs.x1, rhs.x1);
  const float y1 = std::max(lhs.y1, rhs.y1);
  const float x2 = std::min(lhs.x2, rhs.x2);
  const float y2 = std::min(lhs.y2, rhs.y2);
  const float w = std::max(0.0f, x2 - x1);
  const float h = std::max(0.0f, y2 - y1);
  const float inter = w * h;
  const float area_l =
      std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
  const float area_r =
      std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
  const float denom = area_l + area_r - inter;
  return denom <= 0.0f ? 0.0f : inter / denom;
}

std::vector<Box> nonMaxSuppression(const std::vector<Box> &boxes,
                                   float iou_threshold) {
  std::vector<int> order(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    order[i] = static_cast<int>(i);
  }
  std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
    return boxes[lhs].score > boxes[rhs].score;
  });

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
      if (intersectionOverUnion(boxes[index], boxes[other]) > iou_threshold) {
        removed[other] = true;
      }
    }
  }
  return kept;
}

}  // namespace

class NnScrfd::CustomRuntime {
 public:
  ~CustomRuntime() { printProfile(); }

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    descriptor_ = descriptor;
    if (!session_.open(config, descriptor, error)) {
      return false;
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 127.5f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f / 128.0f);
    if (session_.nchwLayout() &&
        (session_.inputDtype() == BM_INT8 ||
         session_.inputDtype() == BM_UINT8)) {
      bmrt_runtime::VpssPreprocessor::Config vpss_config;
      vpss_config.width = session_.inputWidth();
      vpss_config.height = session_.inputHeight();
      vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
      vpss_config.keep_aspect_ratio = true;
      vpss_config.padding = {{127, 127, 127}};
      vpss_config.input_dtype = session_.inputDtype();
      vpss_config.input_scale = session_.inputScale();
      vpss_config.input_zero_point = session_.inputZeroPoint();
      for (int i = 0; i < 3; ++i) {
        vpss_config.mean[static_cast<size_t>(i)] = mean_[static_cast<size_t>(i)];
        vpss_config.scale[static_cast<size_t>(i)] = scale_[static_cast<size_t>(i)];
      }
      std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor(
          new bmrt_runtime::VpssPreprocessor());
      if (preprocessor->open(session_.handle(), vpss_config, &hardware_error_)) {
        hardware_preprocessor_ = std::move(preprocessor);
      }
    } else {
      hardware_error_ = "require NCHW int8/uint8 model input";
    }
    if (profileEnabled()) {
      std::cout << "[profile] scrfd_face input: size=" << session_.inputWidth()
                << "x" << session_.inputHeight()
                << ", layout=" << (session_.nchwLayout() ? "NCHW" : "NHWC")
                << ", dtype=" << dtypeName(session_.inputDtype())
                << ", input_scale=" << session_.inputScale()
                << ", input_zero_point=" << session_.inputZeroPoint()
                << std::endl;
    }
    return buildBranches(error);
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!result) {
      bmrt_runtime::setError(error, "result pointer is null");
      return false;
    }

    const bool profile_enabled = profileEnabled();
    const auto total_begin = std::chrono::steady_clock::now();

    double read_ms = 0.0;
    const cv::Mat *image = loadCachedImage(image_path, &read_ms, error);
    if (!image) {
      return false;
    }

    PreparedInput input;
    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    const auto preprocess_begin = std::chrono::steady_clock::now();
    if (!preprocess(*image, &input, &ratio, &top, &left, error)) {
      return false;
    }
    const auto preprocess_end = std::chrono::steady_clock::now();

    std::vector<bmrt_runtime::OutputTensor> outputs;
    bmrt_runtime::LaunchProfile launch_profile;
    const auto launch_begin = std::chrono::steady_clock::now();
    if (!session_.launchPrepared(input.data(), &outputs, error,
                                 profile_enabled ? &launch_profile : nullptr)) {
      return false;
    }
    const auto launch_end = std::chrono::steady_clock::now();

    *result = AlgorithmResult{};
    const auto decode_begin = std::chrono::steady_clock::now();
    result->boxes = decode(outputs, image->cols, image->rows, ratio, top, left,
                           options.threshold, options.iou_threshold);
    const auto decode_end = std::chrono::steady_clock::now();
    last_infer_profile_.source_prepare_ms = read_ms;
    last_infer_profile_.preprocess_ms = elapsedMs(preprocess_begin, preprocess_end);
    last_infer_profile_.launch_ms = elapsedMs(launch_begin, launch_end);
    last_infer_profile_.decode_ms = elapsedMs(decode_begin, decode_end);
    last_infer_profile_.total_ms = elapsedMs(total_begin, decode_end);
    last_infer_profile_.source_width = image->cols;
    last_infer_profile_.source_height = image->rows;
    last_infer_profile_.source_format = 0;
    last_infer_profile_.input_width = session_.inputWidth();
    last_infer_profile_.input_height = session_.inputHeight();
    last_infer_profile_.boxes = static_cast<int>(result->boxes.size());
    if (profile_enabled) {
      profile_.count++;
      profile_.read_ms += read_ms;
      profile_.preprocess_ms += last_infer_profile_.preprocess_ms;
      profile_.preprocess_resize_ms += preprocess_profile_.resize_ms;
      profile_.preprocess_fill_ms += preprocess_profile_.fill_ms;
      profile_.preprocess_write_ms += preprocess_profile_.write_ms;
      profile_.preprocess_total_measured_ms += preprocess_profile_.total_ms;
      profile_.launch_ms += last_infer_profile_.launch_ms;
      profile_.decode_ms += last_infer_profile_.decode_ms;
      profile_.total_ms += last_infer_profile_.total_ms;
      profile_.launch_input_ms += launch_profile.input_prepare_ms;
      profile_.launch_output_prepare_ms += launch_profile.output_prepare_ms;
      profile_.launch_bmrt_ms += launch_profile.bmrt_launch_ms;
      profile_.launch_output_convert_ms += launch_profile.output_convert_ms;
      profile_.launch_total_ms += launch_profile.total_ms;
    }
    return true;
  }

  bool inferFrame(const Frame &frame, const InferOptions &options,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!result) {
      bmrt_runtime::setError(error, "result pointer is null");
      return false;
    }
    if (!frame.native) {
      bmrt_runtime::setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
      return false;
    }

    const bool profile_enabled = profileEnabled();
    const auto total_begin = std::chrono::steady_clock::now();
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);

    if (!hardware_preprocessor_) {
      const std::string reason = hardware_error_.empty()
                                     ? "VPSS preprocessor is unavailable"
                                     : hardware_error_;
      bmrt_runtime::setError(error, "SCRFD VPSS input is unavailable: " + reason);
      return false;
    }
    const int source_width = static_cast<int>(video->stVFrame.u32Width);
    const int source_height = static_cast<int>(video->stVFrame.u32Height);
    if (source_width <= 0 || source_height <= 0) {
      bmrt_runtime::setError(error, "SCRFD source frame dimensions are invalid");
      return false;
    }
    const float ratio = std::min(
        static_cast<float>(session_.inputHeight()) / source_height,
        static_cast<float>(session_.inputWidth()) / source_width);
    const int resized_width = static_cast<int>(std::round(source_width * ratio));
    const int resized_height = static_cast<int>(std::round(source_height * ratio));
    const int top = (session_.inputHeight() - resized_height) / 2;
    const int left = (session_.inputWidth() - resized_width) / 2;
    const auto preprocess_begin = std::chrono::steady_clock::now();
    if (!hardware_preprocessor_->preprocess(video, error)) {
      return false;
    }
    const auto preprocess_end = std::chrono::steady_clock::now();

    std::vector<bmrt_runtime::OutputTensor> outputs;
    bmrt_runtime::LaunchProfile launch_profile;
    const auto launch_begin = std::chrono::steady_clock::now();
    if (!session_.launchDevice(hardware_preprocessor_->inputMemory(), &outputs,
                               error,
                               profile_enabled ? &launch_profile : nullptr)) {
      return false;
    }
    const auto launch_end = std::chrono::steady_clock::now();

    *result = AlgorithmResult{};
    const auto decode_begin = std::chrono::steady_clock::now();
    result->boxes = decode(outputs, source_width, source_height, ratio, top, left,
                           options.threshold, options.iou_threshold);
    const auto decode_end = std::chrono::steady_clock::now();

    last_infer_profile_.source_prepare_ms = 0.0;
    last_infer_profile_.preprocess_ms = elapsedMs(preprocess_begin, preprocess_end);
    last_infer_profile_.launch_ms = elapsedMs(launch_begin, launch_end);
    last_infer_profile_.decode_ms = elapsedMs(decode_begin, decode_end);
    last_infer_profile_.total_ms = elapsedMs(total_begin, decode_end);
    last_infer_profile_.source_width = source_width;
    last_infer_profile_.source_height = source_height;
    last_infer_profile_.source_format = static_cast<int>(video->stVFrame.enPixelFormat);
    last_infer_profile_.input_width = session_.inputWidth();
    last_infer_profile_.input_height = session_.inputHeight();
    last_infer_profile_.boxes = static_cast<int>(result->boxes.size());

    if (profile_enabled) {
      profile_.count++;
      profile_.read_ms += last_infer_profile_.source_prepare_ms;
      profile_.preprocess_ms += last_infer_profile_.preprocess_ms;
      profile_.preprocess_resize_ms += preprocess_profile_.resize_ms;
      profile_.preprocess_fill_ms += preprocess_profile_.fill_ms;
      profile_.preprocess_write_ms += preprocess_profile_.write_ms;
      profile_.preprocess_total_measured_ms += preprocess_profile_.total_ms;
      profile_.launch_ms += last_infer_profile_.launch_ms;
      profile_.decode_ms += last_infer_profile_.decode_ms;
      profile_.total_ms += last_infer_profile_.total_ms;
      profile_.launch_input_ms += launch_profile.input_prepare_ms;
      profile_.launch_output_prepare_ms += launch_profile.output_prepare_ms;
      profile_.launch_bmrt_ms += launch_profile.bmrt_launch_ms;
      profile_.launch_output_convert_ms += launch_profile.output_convert_ms;
      profile_.launch_total_ms += launch_profile.total_ms;
    }
    return true;
  }

  const NnScrfd::Profile &lastProfile() const { return last_infer_profile_; }

 private:
  struct PreparedInput {
    std::vector<float> float_data;
    std::vector<uint8_t> bytes;
    bool use_float = false;

    const void *data() const {
      return use_float ? static_cast<const void *>(float_data.data())
                       : static_cast<const void *>(bytes.data());
    }
  };

  struct ProfileStats {
    int count = 0;
    double read_ms = 0.0;
    double preprocess_ms = 0.0;
    double launch_ms = 0.0;
    double decode_ms = 0.0;
    double total_ms = 0.0;
    double launch_input_ms = 0.0;
    double launch_output_prepare_ms = 0.0;
    double launch_bmrt_ms = 0.0;
    double launch_output_convert_ms = 0.0;
    double launch_total_ms = 0.0;
    double preprocess_resize_ms = 0.0;
    double preprocess_fill_ms = 0.0;
    double preprocess_write_ms = 0.0;
    double preprocess_total_measured_ms = 0.0;
  };

  struct PreprocessProfile {
    double resize_ms = 0.0;
    double fill_ms = 0.0;
    double write_ms = 0.0;
    double total_ms = 0.0;
  };

  static bool profileEnabled() {
    const char *value = std::getenv("TDL_BENCH_PROFILE");
    return value && value[0] != '\0' && value[0] != '0';
  }

  static double elapsedMs(std::chrono::steady_clock::time_point begin,
                          std::chrono::steady_clock::time_point end) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::microseconds>(end -
                                                                      begin)
                   .count()) /
           1000.0;
  }

  static const char *dtypeName(bm_data_type_t dtype) {
    switch (dtype) {
      case BM_FLOAT32:
        return "BM_FLOAT32";
      case BM_FLOAT16:
        return "BM_FLOAT16";
      case BM_BFLOAT16:
        return "BM_BFLOAT16";
      case BM_INT8:
        return "BM_INT8";
      case BM_UINT8:
        return "BM_UINT8";
      case BM_INT16:
        return "BM_INT16";
      case BM_UINT16:
        return "BM_UINT16";
      case BM_INT32:
        return "BM_INT32";
      case BM_UINT32:
        return "BM_UINT32";
      default:
        return "UNKNOWN";
    }
  }

  void printProfile() const {
    if (!profileEnabled() || profile_.count <= 0) {
      return;
    }
    const double n = static_cast<double>(profile_.count);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "[profile] scrfd_face avg over " << profile_.count
        << " runs: total=" << profile_.total_ms / n
        << " ms, read_image=" << profile_.read_ms / n
        << " ms, preprocess=" << profile_.preprocess_ms / n
        << " ms, launch_total=" << profile_.launch_ms / n
        << " ms, decode_nms=" << profile_.decode_ms / n << " ms\n";
    oss << "[profile] scrfd_face launch avg: input_prepare="
        << profile_.launch_input_ms / n
        << " ms, output_prepare=" << profile_.launch_output_prepare_ms / n
        << " ms, bmrt_launch=" << profile_.launch_bmrt_ms / n
        << " ms, output_convert=" << profile_.launch_output_convert_ms / n
        << " ms, launch_measured=" << profile_.launch_total_ms / n << " ms\n";
    oss << "[profile] scrfd_face preprocess avg: resize="
        << profile_.preprocess_resize_ms / n
        << " ms, fill_padding=" << profile_.preprocess_fill_ms / n
        << " ms, write_quantize=" << profile_.preprocess_write_ms / n
        << " ms, measured=" << profile_.preprocess_total_measured_ms / n
        << " ms";
    std::cout << oss.str() << std::endl;
  }

  bool buildBranches(std::string *error) {
    branches_.clear();
    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];

    const int strides[] = {8, 16, 32};
    for (int stride : strides) {
      ScrfdBranch branch;
      branch.stride = stride;
      branch.feat_w = static_cast<int>(
          std::ceil(static_cast<float>(session_.inputWidth()) / stride));
      branch.feat_h = static_cast<int>(
          std::ceil(static_cast<float>(session_.inputHeight()) / stride));

      for (int i = 0; i < net_info->output_num; ++i) {
        const bm_shape_t &shape = stage.output_shapes[i];
        if (shape.num_dims != 4) {
          continue;
        }

        const bool nchw_match = shape.dims[2] == branch.feat_h &&
                                shape.dims[3] == branch.feat_w;
        const bool nhwc_match = shape.dims[1] == branch.feat_h &&
                                shape.dims[2] == branch.feat_w;
        if (!nchw_match && !nhwc_match) {
          continue;
        }

        const bool channel_last = nhwc_match && !nchw_match;
        const int channel_dim = channel_last ? shape.dims[3] : shape.dims[1];
        if (channel_dim == branch.num_anchors) {
          branch.score_index = i;
          branch.channel_last = channel_last;
        } else if (channel_dim == branch.num_anchors * 4) {
          branch.bbox_index = i;
          branch.channel_last = channel_last;
        } else if (channel_dim == branch.num_anchors * 10) {
          branch.landmark_index = i;
          branch.channel_last = channel_last;
        }
      }

      if (branch.score_index < 0 || branch.bbox_index < 0 ||
          branch.landmark_index < 0) {
        bmrt_runtime::setError(error, "incomplete SCRFD output branches");
        return false;
      }
      branches_.push_back(branch);
    }
    return true;
  }

  const cv::Mat *loadCachedImage(const std::string &image_path, double *read_ms,
                                 std::string *error) {
    if (cached_image_path_ == image_path && !cached_image_.empty()) {
      if (read_ms) {
        *read_ms = 0.0;
      }
      return &cached_image_;
    }

    const auto read_begin = std::chrono::steady_clock::now();
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    const auto read_end = std::chrono::steady_clock::now();
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return nullptr;
    }
    cached_image_path_ = image_path;
    cached_image_ = std::move(image);
    if (read_ms) {
      *read_ms = elapsedMs(read_begin, read_end);
    }
    return &cached_image_;
  }

  bool preprocess(const cv::Mat &image, PreparedInput *input, float *ratio,
                  int *top, int *left, std::string *error) const {
    const auto total_begin = std::chrono::steady_clock::now();
    preprocess_profile_ = PreprocessProfile{};
    if (!input) {
      bmrt_runtime::setError(error, "prepared input pointer is null");
      return false;
    }
    *ratio = std::min(static_cast<float>(session_.inputHeight()) / image.rows,
                      static_cast<float>(session_.inputWidth()) / image.cols);
    const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
    const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));
    *top = (session_.inputHeight() - resized_h) / 2;
    *left = (session_.inputWidth() - resized_w) / 2;

    cv::Mat resized;
    const auto resize_begin = std::chrono::steady_clock::now();
    cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0,
               cv::INTER_LINEAR);
    const auto resize_end = std::chrono::steady_clock::now();
    preprocess_profile_.resize_ms = elapsedMs(resize_begin, resize_end);

    if (session_.inputDtype() == BM_FLOAT32) {
      cv::Mat padded(session_.inputHeight(), session_.inputWidth(), CV_8UC3,
                     cv::Scalar(127, 127, 127));
      resized.copyTo(padded(cv::Rect(*left, *top, resized_w, resized_h)));
      input->use_float = true;
      bmrt_runtime::writeImageToTensor(
          padded, bmrt_runtime::wantsRgbInput(descriptor_, true),
          session_.nchwLayout(), mean_, scale_, &input->float_data);
      preprocess_profile_.total_ms =
          elapsedMs(total_begin, std::chrono::steady_clock::now());
      return true;
    }

    if (session_.inputDtype() != BM_INT8 && session_.inputDtype() != BM_UINT8) {
      bmrt_runtime::setError(error, "runtime does not support this input dtype");
      return false;
    }

    input->use_float = false;
    writeResizedToQuantizedInput(
        resized, session_.inputWidth(), session_.inputHeight(), *top, *left,
        bmrt_runtime::wantsRgbInput(descriptor_, true), session_.nchwLayout(),
        session_.inputDtype(), session_.inputScale(), session_.inputZeroPoint(),
        &input->bytes);
    preprocess_profile_.total_ms =
        elapsedMs(total_begin, std::chrono::steady_clock::now());
    return true;
  }

  uint8_t quantizedByte(float pixel_value, int channel, bm_data_type_t dtype,
                        float input_scale, int input_zero_point) const {
    const float normalized = (pixel_value - mean_[channel]) * scale_[channel];
    const float q = bmrt_runtime::quantizeInputValue(normalized, input_scale,
                                                     input_zero_point);
    if (dtype == BM_INT8) {
      return static_cast<uint8_t>(bmrt_runtime::clampCast<int8_t>(q));
    }
    return bmrt_runtime::clampCast<uint8_t>(q);
  }

  void writeResizedToQuantizedInput(const cv::Mat &resized, int canvas_w,
                                    int canvas_h, int top, int left,
                                    bool rgb_input, bool nchw_layout,
                                    bm_data_type_t dtype, float input_scale,
                                    int input_zero_point,
                                    std::vector<uint8_t> *bytes) const {
    const int channels = bmrt_runtime::kInputChannels;
    const size_t total =
        static_cast<size_t>(canvas_w) * canvas_h * channels;
    bytes->resize(total);

    const auto sourceChannel = [&](int c) {
      return rgb_input ? (channels - 1 - c) : c;
    };

    if (nchw_layout) {
      const auto fill_begin = std::chrono::steady_clock::now();
      const size_t plane = static_cast<size_t>(canvas_w) * canvas_h;
      for (int c = 0; c < channels; ++c) {
        const uint8_t pad =
            quantizedByte(127.0f, c, dtype, input_scale, input_zero_point);
        std::fill(bytes->begin() + static_cast<ptrdiff_t>(c * plane),
                  bytes->begin() + static_cast<ptrdiff_t>((c + 1) * plane),
                  pad);
      }
      preprocess_profile_.fill_ms =
          elapsedMs(fill_begin, std::chrono::steady_clock::now());

      const auto write_begin = std::chrono::steady_clock::now();
      for (int y = 0; y < resized.rows; ++y) {
        const cv::Vec3b *src = resized.ptr<cv::Vec3b>(y);
        const size_t row_base =
            static_cast<size_t>(top + y) * canvas_w + left;
        for (int x = 0; x < resized.cols; ++x) {
          const cv::Vec3b &pixel = src[x];
          for (int c = 0; c < channels; ++c) {
            const size_t dst = static_cast<size_t>(c) * plane + row_base + x;
            (*bytes)[dst] = quantizedByte(pixel[sourceChannel(c)], c, dtype,
                                          input_scale, input_zero_point);
          }
        }
      }
      preprocess_profile_.write_ms =
          elapsedMs(write_begin, std::chrono::steady_clock::now());
      return;
    }

    const auto fill_begin = std::chrono::steady_clock::now();
    for (int y = 0; y < canvas_h; ++y) {
      for (int x = 0; x < canvas_w; ++x) {
        const size_t base = (static_cast<size_t>(y) * canvas_w + x) * channels;
        for (int c = 0; c < channels; ++c) {
          (*bytes)[base + c] =
              quantizedByte(127.0f, c, dtype, input_scale, input_zero_point);
        }
      }
    }
    preprocess_profile_.fill_ms =
        elapsedMs(fill_begin, std::chrono::steady_clock::now());
    const auto write_begin = std::chrono::steady_clock::now();
    for (int y = 0; y < resized.rows; ++y) {
      const cv::Vec3b *src = resized.ptr<cv::Vec3b>(y);
      for (int x = 0; x < resized.cols; ++x) {
        const cv::Vec3b &pixel = src[x];
        const size_t base =
            (static_cast<size_t>(top + y) * canvas_w + (left + x)) * channels;
        for (int c = 0; c < channels; ++c) {
          (*bytes)[base + c] = quantizedByte(pixel[sourceChannel(c)], c, dtype,
                                             input_scale, input_zero_point);
        }
      }
    }
    preprocess_profile_.write_ms =
        elapsedMs(write_begin, std::chrono::steady_clock::now());
  }

  std::vector<Box> decode(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                          int image_width, int image_height, float ratio, int top,
                          int left, float threshold, float iou_threshold) const {
    constexpr int kLandmarkCount = 5;
    std::vector<Box> boxes;

    for (const ScrfdBranch &branch : branches_) {
      const auto &score = outputs[static_cast<size_t>(branch.score_index)].data;
      const auto &bbox = outputs[static_cast<size_t>(branch.bbox_index)].data;
      const auto &landmark =
          outputs[static_cast<size_t>(branch.landmark_index)].data;
      const int count = branch.feat_w * branch.feat_h;
      const int bbox_channels = branch.num_anchors * 4;
      const int landmark_channels = branch.num_anchors * 10;

      const auto scoreAt = [&](int index, int anchor) -> float {
        if (branch.channel_last) {
          return score[static_cast<size_t>(index * branch.num_anchors + anchor)];
        }
        return score[static_cast<size_t>(index + count * anchor)];
      };
      const auto bboxAt = [&](int index, int anchor, int component) -> float {
        if (branch.channel_last) {
          return bbox[static_cast<size_t>(index * bbox_channels +
                                          anchor * 4 + component)];
        }
        return bbox[static_cast<size_t>(index +
                                        count * (anchor * 4 + component))];
      };
      const auto landmarkAt = [&](int index, int anchor, int component) -> float {
        if (branch.channel_last) {
          return landmark[static_cast<size_t>(index * landmark_channels +
                                              anchor * 10 + component)];
        }
        return landmark[static_cast<size_t>(index +
                                            count * (anchor * 10 + component))];
      };

      for (int anchor = 0; anchor < branch.num_anchors; ++anchor) {
        for (int index = 0; index < count; ++index) {
          const float conf = scoreAt(index, anchor);
          if (conf < threshold) {
            continue;
          }

          const int grid_y = index / branch.feat_w;
          const int grid_x = index % branch.feat_w;
          // SCRFD's MMDetection anchors use center_offset=0. The exported
          // distance and keypoint heads are therefore relative to the integer
          // grid origin, not to the middle of the feature-map cell.
          const float center_x = static_cast<float>(grid_x * branch.stride);
          const float center_y = static_cast<float>(grid_y * branch.stride);

          const float x1 = center_x - bboxAt(index, anchor, 0) * branch.stride;
          const float y1 = center_y - bboxAt(index, anchor, 1) * branch.stride;
          const float x2 = center_x + bboxAt(index, anchor, 2) * branch.stride;
          const float y2 = center_y + bboxAt(index, anchor, 3) * branch.stride;
          if (x1 >= x2 || y1 >= y2) {
            continue;
          }

          Box box;
          box.class_id = 0;
          box.score = conf;
          box.x1 = modelToImageCoordinate(x1, left, ratio, image_width);
          box.y1 = modelToImageCoordinate(y1, top, ratio, image_height);
          box.x2 = modelToImageCoordinate(x2, left, ratio, image_width);
          box.y2 = modelToImageCoordinate(y2, top, ratio, image_height);

          for (int k = 0; k < kLandmarkCount; ++k) {
            Point point;
            const float model_x =
                landmarkAt(index, anchor, k * 2) * branch.stride + center_x;
            const float model_y = landmarkAt(index, anchor, k * 2 + 1) *
                                      branch.stride +
                                  center_y;
            point.x =
                modelToImageCoordinate(model_x, left, ratio, image_width);
            point.y =
                modelToImageCoordinate(model_y, top, ratio, image_height);
            box.landmarks.push_back(point);
          }
          boxes.push_back(box);
        }
      }
    }

    return nonMaxSuppression(boxes, iou_threshold > 0.0f ? iou_threshold : 0.5f);
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::string hardware_error_;
  std::vector<ScrfdBranch> branches_;
  std::mutex infer_mutex_;
  ProfileStats profile_;
  mutable PreprocessProfile preprocess_profile_;
  std::string cached_image_path_;
  cv::Mat cached_image_;
  NnScrfd::Profile last_infer_profile_;
};

NnScrfd::NnScrfd(std::string model_type) : model_type_(std::move(model_type)) {}

NnScrfd::~NnScrfd() = default;

TaskType NnScrfd::task() const { return TaskType::FaceDetection; }

std::string NnScrfd::modelType() const { return model_type_; }

bool NnScrfd::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    bmrt_runtime::setError(error,
                           "SCRFD runtime requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnScrfd::load(EngineConfig config, std::string *error) {
  initialized_ = false;
  // Releasing the old runtime here also tears down its private VPSS group and
  // persistent BMRT output buffers before a reload attempt starts.
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
  initialized_ = true;
  return true;
}

bool NnScrfd::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnScrfd::predict(const std::string &image_path, const InferOptions &options,
                      AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnScrfd::predictFrame(const Frame &frame, const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, options, result, error);
  }
  if (frame.native) {
    return custom_runtime_->inferFrame(frame, options, result, error);
  }
  bmrt_runtime::setError(error, "SCRFD runtime requires image_path or native frame");
  return false;
}

const NnScrfd::Profile &NnScrfd::lastProfile() const {
  static const Profile empty_profile;
  if (!custom_runtime_) {
    return empty_profile;
  }
  return custom_runtime_->lastProfile();
}

}  // namespace tdl_app
