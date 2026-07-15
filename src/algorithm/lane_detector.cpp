#include "tdl_app/lane_detector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"

namespace tdl_app {
namespace {

constexpr int kLaneCandidateCount = 7;
constexpr int kLaneLogitDims = 2;
constexpr int kLaneCurveDims = 8;
constexpr int kLaneSlices = 100;
constexpr float kLaneCutRatio = 0.25f;
constexpr float kLaneUpperY = 0.6f;
constexpr float kLaneLowerY = 0.8f;

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

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
    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    const std::string model_type = !requested_model_type.empty()
                                       ? requested_model_type
                                       : (!config.model_type.empty()
                                              ? config.model_type
                                              : (!descriptor_.model_type.empty()
                                                     ? descriptor_.model_type
                                                     : "LSTR_DET_LANE"));
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
    if (!session_.nchwLayout() ||
        (session_.inputDtype() != BM_INT8 &&
         session_.inputDtype() != BM_UINT8)) {
      bmrt_runtime::setError(
          error, "LSTR VPSS path requires NCHW INT8/UINT8 model input");
      session_.close();
      return false;
    }

    bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = session_.inputWidth();
    vpss_config.height = session_.inputHeight();
    vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
    vpss_config.input_dtype = session_.inputDtype();
    vpss_config.input_scale = session_.inputScale();
    vpss_config.input_zero_point = session_.inputZeroPoint();
    vpss_config.mean = {{mean_[0], mean_[1], mean_[2]}};
    vpss_config.scale = {{scale_[0], scale_[1], scale_[2]}};
    std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor(
        new bmrt_runtime::VpssPreprocessor());
    if (!preprocessor->open(session_.handle(), vpss_config, error)) {
      session_.close();
      return false;
    }
    preprocessor_ = std::move(preprocessor);

    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return infer(image, result, error);
  }

  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error) override {
    if (!frame.image_path.empty()) {
      return run(frame.image_path, result, error);
    }
    if (!frame.native || !result) {
      bmrt_runtime::setError(error, "lane frame/result pointer is null");
      return false;
    }
    if (!preprocessor_->preprocess(frame.native, error)) {
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    if (width <= 0 || height <= 0) {
      bmrt_runtime::setError(error, "invalid native lane frame size");
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launchDevice(preprocessor_->inputMemory(), &outputs, error)) {
      return false;
    }
    return decodeOutputs(outputs, width, height, result, error);
  }

  void reset() override {
    preprocessor_.reset();
    session_.close();
  }
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
      bmrt_runtime::setError(
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

  float laneXAtY(const float *curve, float y) const {
    const float denominator = y - curve[3];
    if (std::fabs(denominator) < 1e-6f) {
      return curve[5] + curve[6] * y - curve[7];
    }
    return curve[2] / (denominator * denominator) + curve[4] / denominator +
           curve[5] + curve[6] * y - curve[7];
  }

  bool decodeOutputs(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                     int image_width, int image_height,
                     LaneDetectionResult *result, std::string *error) const {
    if (!result) {
      bmrt_runtime::setError(error, "lane result pointer is null");
      return false;
    }
    if (logits_index_ < 0 || curves_index_ < 0 ||
        logits_index_ >= static_cast<int>(outputs.size()) ||
        curves_index_ >= static_cast<int>(outputs.size())) {
      bmrt_runtime::setError(error, "invalid lane output index");
      return false;
    }

    const auto &logits = outputs[static_cast<size_t>(logits_index_)].data;
    const auto &curves = outputs[static_cast<size_t>(curves_index_)].data;
    if (logits.size() != static_cast<size_t>(kLaneCandidateCount * kLaneLogitDims) ||
        curves.size() != static_cast<size_t>(kLaneCandidateCount * kLaneCurveDims)) {
      bmrt_runtime::setError(error, "unexpected lane output tensor size");
      return false;
    }

    result->clear();
    result->width = image_width;
    result->height = image_height;

    std::vector<int> active_indices;
    std::vector<float> lane_distance;
    for (int i = 0; i < kLaneCandidateCount; ++i) {
      const float bg_logit = logits[static_cast<size_t>(i * 2 + 0)];
      const float lane_logit = logits[static_cast<size_t>(i * 2 + 1)];
      if (lane_logit <= bg_logit) {
        continue;
      }
      const float *curve = &curves[static_cast<size_t>(i * kLaneCurveDims)];
      active_indices.push_back(i);
      lane_distance.push_back((laneXAtY(curve, 1.0f) - 0.5f) * image_width);
    }

    std::vector<int> order(active_indices.size());
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
      return lane_distance[static_cast<size_t>(lhs)] <
             lane_distance[static_cast<size_t>(rhs)];
    });

    std::vector<int> selected;
    for (size_t i = 0; i < order.size(); ++i) {
      const int candidate = order[i];
      const float *curve = &curves[static_cast<size_t>(
          active_indices[static_cast<size_t>(candidate)] * kLaneCurveDims)];
      if (curve[1] - curve[0] <= 0.2f) {
        continue;
      }
      if (lane_distance[static_cast<size_t>(candidate)] < 0.0f) {
        if (i + 1 == order.size() ||
            lane_distance[static_cast<size_t>(order[i + 1])] > 0.0f) {
          selected.push_back(candidate);
        }
      } else {
        selected.push_back(candidate);
        break;
      }
    }

    for (int selected_index : selected) {
      const int lane_index = active_indices[static_cast<size_t>(selected_index)];
      const float *curve =
          &curves[static_cast<size_t>(lane_index * kLaneCurveDims)];
      const float lower = std::max(0.0f, curve[0]);
      const float upper = std::min(1.0f, curve[1]);
      const float slice = (upper - lower) / kLaneSlices;
      const float y1 = lower + kLaneSlices * kLaneCutRatio * slice;
      const float y2 = lower + kLaneSlices * (1.0f - kLaneCutRatio) * slice;
      const float denominator = y2 - y1;
      if (std::fabs(denominator) < 1e-6f) {
        continue;
      }
      const float x1 = laneXAtY(curve, y1);
      const float x2 = laneXAtY(curve, y2);
      const float score = sigmoid(
          logits[static_cast<size_t>(lane_index * 2 + 1)] -
          logits[static_cast<size_t>(lane_index * 2)]);

      LaneSegment lane;
      lane.score = score;
      lane.start.x = (x1 + (kLaneUpperY - y1) * (x2 - x1) / denominator) *
                     image_width;
      lane.start.y = kLaneUpperY * image_height;
      lane.start.score = score;
      lane.end.x = (x1 + (kLaneLowerY - y1) * (x2 - x1) / denominator) *
                   image_width;
      lane.end.y = kLaneLowerY * image_height;
      lane.end.score = score;
      result->lanes.push_back(lane);
    }
    result->lane_state = static_cast<int>(result->lanes.size());
    return true;
  }

  bool infer(const cv::Mat &image, LaneDetectionResult *result,
             std::string *error) {
    std::vector<float> input_tensor;
    preprocess(image, &input_tensor);
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    return decodeOutputs(outputs, image.cols, image.rows, result, error);
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor_;
  int logits_index_ = -1;
  int curves_index_ = -1;
};

}  // namespace

class LaneDetector::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = !requested_model_type.empty()
                                       ? requested_model_type
                                       : (!config.model_type.empty()
                                              ? config.model_type
                                              : "LSTR_DET_LANE");

    runtime_.reset();
    if (bmrt_runtime::startsWith(bmrt_runtime::toUpper(model_type), "LSTR_DET_LANE")) {
      std::unique_ptr<LstrRuntime> runtime(new LstrRuntime());
      if (!runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(runtime);
      return true;
    }

    bmrt_runtime::setError(
        error, "unsupported lane model_type for custom BMRT runtime: " + model_type);
    return false;
  }

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error) {
    if (!runtime_) {
      bmrt_runtime::setError(error, "lane detector is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error) {
    if (!runtime_) {
      bmrt_runtime::setError(error, "lane detector is not initialized");
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
    bmrt_runtime::setError(error, "lane detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool LaneDetector::detect(const std::string &image_path,
                          LaneDetectionResult *result,
                          std::string *error) {
  if (!impl_) {
    bmrt_runtime::setError(error, "lane detector is not initialized");
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
