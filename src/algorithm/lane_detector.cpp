#include "tdl_app/lane_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/tdl_sdk_utils.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace tdl_app {
namespace {

constexpr int kLaneCandidateCount = 7;
constexpr int kLaneLogitDims = 2;
constexpr int kLaneCurveDims = 8;

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

bool frameToBgrMat(const Frame &frame, cv::Mat *image, std::string *error) {
  if (!frame.native) {
    private_tdl_sdk::setError(error,
                              "frame has no native VIDEO_FRAME_INFO_S buffer");
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
        error, "lane runtime only supports RGB/BGR/NV12/NV21/YUV400 frame input");
  }

  CVI_SYS_Munmap(mapped, map_size);
  if (!ok || image->empty()) {
    private_tdl_sdk::setError(error, "failed to convert frame to BGR image");
    return false;
  }
  return true;
}

struct LaneRuntime {
  virtual ~LaneRuntime() = default;
  virtual bool run(const std::string &image_path, LaneDetectionResult *result,
                   std::string *error) = 0;
  virtual bool runFrame(const Frame &frame, LaneDetectionResult *result,
                        std::string *error) = 0;
  virtual void reset() = 0;
  virtual bool initialized() const = 0;
};

class LstrRuntime : public LaneRuntime {
 public:
  bool open(const LaneDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "LSTR_DET_LANE", error);
    if (model_type.empty()) {
      return false;
    }

    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.runtime.empty()) {
      descriptor_.runtime = "lane_detection";
    }
    if (descriptor_.task_name.empty()) {
      descriptor_.task_name = "lane";
    }
    if (descriptor_.input_type.empty()) {
      descriptor_.input_type = "rgb";
    }

    if (descriptor_.mean.empty()) {
      descriptor_.mean = {0.485f * 255.0f, 0.456f * 255.0f, 0.406f * 255.0f};
    }
    if (descriptor_.scale.empty()) {
      descriptor_.scale = {1.0f / (255.0f * 0.229f),
                           1.0f / (255.0f * 0.224f),
                           1.0f / (255.0f * 0.225f)};
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f / 255.0f);

    EngineConfig engine_config;
    engine_config.model_descriptor_file = config.model_spec;
    engine_config.model_dir = config.model_dir;
    engine_config.bmrt_firmware = config.firmware;
    if (!session_.open(engine_config, descriptor_, error)) {
      return false;
    }

    if (!buildOutputs(error)) {
      session_.close();
      return false;
    }

    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      private_tdl_sdk::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return infer(image, result, error);
  }

  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error) override {
    if (!frame.image_path.empty()) {
      return run(frame.image_path, result, error);
    }
    cv::Mat image;
    if (!frameToBgrMat(frame, &image, error)) {
      return false;
    }
    return infer(image, result, error);
  }

  void reset() override { session_.close(); }
  bool initialized() const override { return session_.opened(); }

 private:
  bool buildOutputs(std::string *error) {
    logits_index_ = -1;
    curves_index_ = -1;

    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];
    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (shape.num_dims != 3 || shape.dims[0] != 1 ||
          shape.dims[1] != kLaneCandidateCount) {
        continue;
      }
      if (shape.dims[2] == kLaneLogitDims) {
        logits_index_ = i;
      } else if (shape.dims[2] == kLaneCurveDims) {
        curves_index_ = i;
      }
    }

    if (logits_index_ < 0 || curves_index_ < 0) {
      private_tdl_sdk::setError(
          error,
          "unable to locate LSTR lane outputs (expected [1,7,2] and [1,7,8])");
      return false;
    }
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor) const {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(session_.inputWidth(), session_.inputHeight()),
               0, 0, cv::INTER_LINEAR);
    bmrt_runtime::writeImageToTensor(
        resized, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  LaneSegment decodeLane(const float score, const float *curve,
                         int image_width, int image_height) const {
    LaneSegment lane;
    lane.score = score;

    const float y0 = std::max(0.0f, std::min(1.0f, curve[0]));
    const float y1 = std::max(0.0f, std::min(1.0f, curve[1]));
    const float k_2 = curve[2];
    const float f_2 = curve[3];
    const float m_2 = curve[4];
    const float n_1 = curve[5];
    const float b_2 = curve[6];
    const float b_3 = curve[7];

    auto eval_x = [&](float y_norm) {
      const float y = y_norm;
      const float denom = y - f_2;
      if (std::fabs(denom) < 1e-6f) {
        return n_1 + b_2 * y - b_3;
      }
      return k_2 / (denom * denom) + m_2 / denom + n_1 + b_2 * y - b_3;
    };

    const float x0 = eval_x(y0);
    const float x1 = eval_x(y1);

    lane.start.x = std::max(0.0f, std::min(x0, 1.0f)) * image_width;
    lane.start.y = y0 * image_height;
    lane.start.score = score;
    lane.end.x = std::max(0.0f, std::min(x1, 1.0f)) * image_width;
    lane.end.y = y1 * image_height;
    lane.end.score = score;
    return lane;
  }

  bool infer(const cv::Mat &image, LaneDetectionResult *result,
             std::string *error) {
    if (!result) {
      private_tdl_sdk::setError(error, "lane result pointer is null");
      return false;
    }

    std::vector<float> input_tensor;
    preprocess(image, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (logits_index_ < 0 || curves_index_ < 0 ||
        logits_index_ >= static_cast<int>(outputs.size()) ||
        curves_index_ >= static_cast<int>(outputs.size())) {
      private_tdl_sdk::setError(error, "invalid lane output index");
      return false;
    }

    const auto &logits = outputs[static_cast<size_t>(logits_index_)].data;
    const auto &curves = outputs[static_cast<size_t>(curves_index_)].data;
    if (logits.size() != static_cast<size_t>(kLaneCandidateCount * kLaneLogitDims) ||
        curves.size() != static_cast<size_t>(kLaneCandidateCount * kLaneCurveDims)) {
      private_tdl_sdk::setError(error, "unexpected lane output tensor size");
      return false;
    }

    result->clear();
    result->width = image.cols;
    result->height = image.rows;

    int active_count = 0;
    for (int i = 0; i < kLaneCandidateCount; ++i) {
      const float bg_logit = logits[static_cast<size_t>(i * 2 + 0)];
      const float lane_logit = logits[static_cast<size_t>(i * 2 + 1)];
      const float lane_prob = sigmoid(lane_logit - bg_logit);
      if (lane_prob < 0.5f) {
        continue;
      }

      const float *curve = &curves[static_cast<size_t>(i * kLaneCurveDims)];
      const LaneSegment lane = decodeLane(lane_prob, curve, image.cols, image.rows);
      if (lane.start.y == lane.end.y) {
        continue;
      }
      result->lanes.push_back(lane);
      ++active_count;
    }

    std::sort(result->lanes.begin(), result->lanes.end(),
              [](const LaneSegment &lhs, const LaneSegment &rhs) {
                const float lhs_center = (lhs.start.x + lhs.end.x) * 0.5f;
                const float rhs_center = (rhs.start.x + rhs.end.x) * 0.5f;
                return lhs_center < rhs_center;
              });
    result->lane_state = active_count;
    return true;
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  int logits_index_ = -1;
  int curves_index_ = -1;
};

}  // namespace

class LaneDetector::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "LSTR_DET_LANE", error);
    if (model_type.empty()) {
      return false;
    }

    runtime_.reset();
    if (bmrt_runtime::startsWith(bmrt_runtime::toUpper(model_type), "LSTR_DET_LANE")) {
      std::unique_ptr<LstrRuntime> runtime(new LstrRuntime());
      if (!runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(runtime);
      return true;
    }

    private_tdl_sdk::setError(
        error, "unsupported lane model_type for custom BMRT runtime: " + model_type);
    return false;
  }

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "lane detector is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "lane detector is not initialized");
      return false;
    }
    return runtime_->runFrame(frame, result, error);
  }

  void reset() {
    if (runtime_) {
      runtime_->reset();
    }
  }

  bool initialized() const {
    return runtime_ && runtime_->initialized();
  }

 private:
  std::unique_ptr<LaneRuntime> runtime_;
};

LaneDetector::LaneDetector() = default;

LaneDetector::LaneDetector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

LaneDetector::~LaneDetector() {
  reset();
  delete impl_;
}

LaneDetector::LaneDetector(LaneDetector &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

LaneDetector &LaneDetector::operator=(LaneDetector &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  delete impl_;
  requested_model_type_ = std::move(other.requested_model_type_);
  config_ = std::move(other.config_);
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool LaneDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
}

bool LaneDetector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool LaneDetector::load(const std::string &model_spec,
                        const std::string &firmware,
                        std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool LaneDetector::load(const std::string &model_spec,
                        const std::string &firmware,
                        const std::string &model_dir,
                        std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool LaneDetector::run(const std::string &image_path, LaneDetectionResult *result,
                       std::string *error) {
  return detect(image_path, result, error);
}

bool LaneDetector::runFrame(const Frame &frame, LaneDetectionResult *result,
                            std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "lane detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool LaneDetector::detect(const std::string &image_path,
                          LaneDetectionResult *result,
                          std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "lane detector is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool LaneDetector::initialized() const {
  return impl_ && impl_->initialized();
}

std::string LaneDetector::modelType() const { return requested_model_type_; }

void LaneDetector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
