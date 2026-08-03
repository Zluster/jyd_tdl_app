#include "tdl_app/keypoint_detector.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"
#include "cvi_comm_video.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kPoseKeypointDims = 3;
constexpr int kSimccKeypointCount = 17;

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return value;
}

namespace keypoint_runtime_utils {

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

std::string resolveModelToken(const KeypointDetector::Config &config,
                              const std::string &requested_model_type,
                              const std::string &fallback_model_type,
                              std::string *error) {
  if (!config.model_type.empty()) return toUpper(config.model_type);
  if (!config.model_spec.empty()) {
    ModelDescriptor descriptor;
    std::string ignored_error;
    if (loadModelDescriptor(config.model_spec, &descriptor, &ignored_error) &&
        !descriptor.model_type.empty()) {
      return toUpper(descriptor.model_type);
    }
  }
  if (!requested_model_type.empty()) return toUpper(requested_model_type);
  if (!fallback_model_type.empty()) return toUpper(fallback_model_type);
  setError(error, "model_type is empty and no default is available");
  return std::string();
}

}  // namespace keypoint_runtime_utils

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

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

struct KeypointRuntime {
  virtual ~KeypointRuntime() = default;
  virtual bool run(const std::string &image_path, KeypointResult *result,
                   std::string *error) = 0;
  virtual bool runFrame(const Frame &frame, KeypointResult *result,
                        std::string *error) = 0;
  virtual bool runMat(const cv::Mat &, KeypointResult *, std::string *error) {
    keypoint_runtime_utils::setError(error,
                                     "cropped inference is unavailable");
    return false;
  }
  virtual bool runFrameCrop(const Frame &, int, int, int, int,
                            KeypointResult *, std::string *error) {
    keypoint_runtime_utils::setError(error,
                                     "cropped inference is unavailable");
    return false;
  }
  virtual void reset() = 0;
  virtual bool initialized() const = 0;
};

class PoseYolov8Runtime : public KeypointRuntime {
 public:
  bool open(const KeypointDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type =
        keypoint_runtime_utils::resolveModelToken(config, requested_model_type,
                                           "KEYPOINT_YOLOV8POSE_PERSON17",
                                           error);
    if (model_type.empty()) {
      return false;
    }

    ModelDescriptor descriptor;
    if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
      return false;
    }

    if (descriptor.runtime.empty()) {
      descriptor.runtime = "keypoint";
    }
    if (descriptor.task_name.empty()) {
      descriptor.task_name = "keypoint";
    }
    if (descriptor.input_type.empty()) {
      descriptor.input_type = "rgb";
    }

    model_type_ = model_type;
    descriptor_ = descriptor;
    mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f / 255.0f);

    EngineConfig engine_config;
    engine_config.model_descriptor_file = config.model_spec;
    engine_config.model_dir = config.model_dir;
    engine_config.bmrt_firmware = config.firmware;

    if (!session_.open(engine_config, descriptor_, error)) {
      return false;
    }
    if (!buildBranches(error)) {
      session_.close();
      return false;
    }
    if (!session_.nchwLayout() ||
        (session_.inputDtype() != BM_INT8 &&
         session_.inputDtype() != BM_UINT8)) {
      keypoint_runtime_utils::setError(
          error, "YOLOv8 pose VPSS path requires NCHW INT8/UINT8 input");
      session_.close();
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = session_.inputWidth();
    vpss_config.height = session_.inputHeight();
    vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
    vpss_config.keep_aspect_ratio = true;
    vpss_config.padding = {{114, 114, 114}};
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
      *resolved_model_type = model_type_;
    }
    return true;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      keypoint_runtime_utils::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return inferMat(image, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    if (!result) {
      keypoint_runtime_utils::setError(error, "keypoint result pointer is null");
      return false;
    }
    if (!frame.native) {
      keypoint_runtime_utils::setError(error,
                                 "frame has no native VIDEO_FRAME_INFO_S buffer");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    if (width <= 0 || height <= 0) {
      keypoint_runtime_utils::setError(error, "invalid native frame size");
      return false;
    }

    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    letterboxTransform(width, height, &ratio, &top, &left);
    if (!preprocessor_->preprocess(frame.native, error)) {
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launchDevice(preprocessor_->inputMemory(), &outputs, error)) {
      return false;
    }
    return decodeOutputs(outputs, width, height, ratio, top, left, result);
  }

  void reset() override {
    preprocessor_.reset();
    session_.close();
  }
  bool initialized() const override { return session_.opened(); }

 private:
  struct Branch {
    int stride = 0;
    int feat_w = 0;
    int feat_h = 0;
    int bbox_index = -1;
    int score_index = -1;
    int kpt_index = -1;
    int kpt_channels = 0;
  };

  bool buildBranches(std::string *error) {
    branches_.clear();
    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];
    const int strides[] = {8, 16, 32};

    for (int stride : strides) {
      Branch branch;
      branch.stride = stride;
      branch.feat_w = session_.inputWidth() / stride;
      branch.feat_h = session_.inputHeight() / stride;

      for (int i = 0; i < net_info->output_num; ++i) {
        const bm_shape_t &shape = stage.output_shapes[i];
        if (shape.num_dims != 4) {
          continue;
        }
        if (shape.dims[2] != branch.feat_h || shape.dims[3] != branch.feat_w) {
          continue;
        }

        const int channel = shape.dims[1];
        if (channel == 64) {
          branch.bbox_index = i;
        } else if (channel == 1) {
          branch.score_index = i;
        } else if (channel > 0 && channel % kPoseKeypointDims == 0) {
          branch.kpt_index = i;
          branch.kpt_channels = channel;
        }
      }

      if (branch.bbox_index < 0 || branch.score_index < 0 ||
          branch.kpt_index < 0) {
        keypoint_runtime_utils::setError(
            error, "incomplete yolov8 pose output branches");
        return false;
      }
      const int branch_keypoint_count =
          branch.kpt_channels / kPoseKeypointDims;
      if (branch_keypoint_count <= 0) {
        keypoint_runtime_utils::setError(
            error, "invalid yolov8 pose keypoint channel count");
        return false;
      }
      if (keypoint_count_ == 0) {
        keypoint_count_ = branch_keypoint_count;
      } else if (keypoint_count_ != branch_keypoint_count) {
        keypoint_runtime_utils::setError(
            error, "inconsistent yolov8 pose keypoint branch shapes");
        return false;
      }
      branches_.push_back(branch);
    }
    return true;
  }

  void letterboxTransform(int source_width, int source_height, float *ratio,
                          int *top, int *left) const {
    *ratio = std::min(static_cast<float>(session_.inputHeight()) / source_height,
                      static_cast<float>(session_.inputWidth()) / source_width);
    const int resized_w =
        static_cast<int>(std::round(source_width * (*ratio)));
    const int resized_h =
        static_cast<int>(std::round(source_height * (*ratio)));
    *top = (session_.inputHeight() - resized_h) / 2;
    *left = (session_.inputWidth() - resized_w) / 2;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor, float *ratio,
                  int *top, int *left) const {
    letterboxTransform(image.cols, image.rows, ratio, top, left);
    const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
    const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0,
               cv::INTER_LINEAR);

    cv::Mat padded(session_.inputHeight(), session_.inputWidth(), CV_8UC3,
                   cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(*left, *top, resized_w, resized_h)));

    bmrt_runtime::writeImageToTensor(
        padded, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  std::vector<float> decodeDfl(const std::vector<float> &bbox, int anchor_index,
                               int anchor_count) const {
    constexpr int kRegMax = 16;
    std::vector<float> values(4, 0.0f);
    for (int side = 0; side < 4; ++side) {
      const int offset = side * kRegMax;
      float max_logit = -std::numeric_limits<float>::infinity();
      for (int i = 0; i < kRegMax; ++i) {
        const float logit = bbox[(offset + i) * anchor_count + anchor_index];
        max_logit = std::max(max_logit, logit);
      }
      float sum = 0.0f;
      float weighted = 0.0f;
      for (int i = 0; i < kRegMax; ++i) {
        const float expv =
            std::exp(bbox[(offset + i) * anchor_count + anchor_index] -
                     max_logit);
        sum += expv;
        weighted += expv * static_cast<float>(i);
      }
      values[side] = sum > 0.0f ? weighted / sum : 0.0f;
    }
    return values;
  }

  struct PoseCandidate {
    Box box;
    std::vector<Point> points;
  };

  bool decodeOutputs(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                     int image_width, int image_height, float ratio, int top,
                     int left, KeypointResult *result) const {
    std::vector<PoseCandidate> candidates;
    for (const Branch &branch : branches_) {
      const auto &bbox =
          outputs[static_cast<size_t>(branch.bbox_index)].data;
      const auto &score =
          outputs[static_cast<size_t>(branch.score_index)].data;
      const auto &kpt = outputs[static_cast<size_t>(branch.kpt_index)].data;
      const int anchor_count = branch.feat_w * branch.feat_h;

      for (int anchor = 0; anchor < anchor_count; ++anchor) {
        const float conf = sigmoid(score[static_cast<size_t>(anchor)]);
        if (conf < 0.25f) {
          continue;
        }

        const std::vector<float> distances =
            decodeDfl(bbox, anchor, anchor_count);
        const int anchor_y = anchor / branch.feat_w;
        const int anchor_x = anchor % branch.feat_w;
        const float grid_x = static_cast<float>(anchor_x) + 0.5f;
        const float grid_y = static_cast<float>(anchor_y) + 0.5f;

        const float x1 = (grid_x - distances[0]) * branch.stride;
        const float y1 = (grid_y - distances[1]) * branch.stride;
        const float x2 = (grid_x + distances[2]) * branch.stride;
        const float y2 = (grid_y + distances[3]) * branch.stride;
        if (x2 <= x1 || y2 <= y1) {
          continue;
        }

        PoseCandidate candidate;
        candidate.box.score = conf;
        candidate.box.class_id = 0;
        candidate.box.x1 =
            std::max(0.0f, std::min((x1 - left) / ratio,
                                    static_cast<float>(image_width)));
        candidate.box.y1 =
            std::max(0.0f, std::min((y1 - top) / ratio,
                                    static_cast<float>(image_height)));
        candidate.box.x2 =
            std::max(0.0f, std::min((x2 - left) / ratio,
                                    static_cast<float>(image_width)));
        candidate.box.y2 =
            std::max(0.0f, std::min((y2 - top) / ratio,
                                    static_cast<float>(image_height)));

        candidate.points.reserve(keypoint_count_);
        for (int k = 0; k < keypoint_count_; ++k) {
          Point point;
          point.x =
              std::max(0.0f, std::min(
                                 ((kpt[(k * 3 + 0) * anchor_count + anchor] *
                                       2.0f +
                                   static_cast<float>(anchor_x)) *
                                      branch.stride -
                                  left) /
                                     ratio,
                                 static_cast<float>(image_width)));
          point.y =
              std::max(0.0f, std::min(
                                 ((kpt[(k * 3 + 1) * anchor_count + anchor] *
                                       2.0f +
                                   static_cast<float>(anchor_y)) *
                                      branch.stride -
                                  top) /
                                     ratio,
                                 static_cast<float>(image_height)));
          point.score = sigmoid(
              kpt[(k * 3 + 2) * anchor_count + anchor]);
          candidate.points.push_back(point);
        }
        candidates.push_back(std::move(candidate));
      }
    }

    if (candidates.empty()) {
      result->clear();
      result->width = image_width;
      result->height = image_height;
      return true;
    }

    std::vector<Box> raw_boxes;
    raw_boxes.reserve(candidates.size());
    for (const auto &candidate : candidates) {
      raw_boxes.push_back(candidate.box);
    }

    const std::vector<Box> kept = nonMaxSuppression(raw_boxes, 0.45f);
    if (kept.empty()) {
      result->clear();
      result->width = image_width;
      result->height = image_height;
      return true;
    }

    auto best_it = std::max_element(
        kept.begin(), kept.end(),
        [](const Box &lhs, const Box &rhs) { return lhs.score < rhs.score; });

    const PoseCandidate *best_candidate = nullptr;
    float best_iou = -1.0f;
    for (const auto &candidate : candidates) {
      const float iou = intersectionOverUnion(candidate.box, *best_it);
      if (iou > best_iou) {
        best_iou = iou;
        best_candidate = &candidate;
      }
    }

    result->clear();
    result->width = image_width;
    result->height = image_height;
    if (best_candidate) {
      result->points = best_candidate->points;
    }
    return true;
  }

  bool inferMat(const cv::Mat &image, KeypointResult *result,
                std::string *error) {
    if (!result) {
      keypoint_runtime_utils::setError(error, "keypoint result pointer is null");
      return false;
    }
    std::vector<float> input_tensor;
    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    preprocess(image, &input_tensor, &ratio, &top, &left);
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    return decodeOutputs(outputs, image.cols, image.rows, ratio, top, left,
                         result);
  }

  std::string model_type_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor_;
  std::vector<Branch> branches_;
  int keypoint_count_ = 0;
};

class SimccRuntime : public KeypointRuntime {
 public:
  bool open(const KeypointDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = keypoint_runtime_utils::resolveModelToken(
        config, requested_model_type, "KEYPOINT_SIMCC_PERSON17", error);
    if (model_type.empty()) {
      return false;
    }
    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.input_type.empty()) {
      descriptor_.input_type = "rgb";
    }
    if (descriptor_.mean.empty()) {
      descriptor_.mean = {0.485f * 255.0f, 0.456f * 255.0f,
                          0.406f * 255.0f};
    }
    if (descriptor_.scale.empty()) {
      descriptor_.scale = {1.0f / (255.0f * 0.229f),
                           1.0f / (255.0f * 0.224f),
                           1.0f / (255.0f * 0.225f)};
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f);

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
      keypoint_runtime_utils::setError(
          error, "SimCC VPSS path requires NCHW INT8/UINT8 input");
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

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      keypoint_runtime_utils::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return inferMat(image, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) override {
    if (!result || !frame.native) {
      keypoint_runtime_utils::setError(error, "SimCC frame/result pointer is null");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    if (width <= 0 || height <= 0) {
      keypoint_runtime_utils::setError(error, "invalid SimCC native frame size");
      return false;
    }
    if (!preprocessor_->preprocess(frame.native, error)) {
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
    x_index_ = -1;
    y_index_ = -1;
    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];
    const int expected_x_bins = session_.inputWidth() * 2;
    const int expected_y_bins = session_.inputHeight() * 2;
    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (shape.num_dims != 3 || shape.dims[0] != 1 ||
          shape.dims[1] != kSimccKeypointCount) {
        continue;
      }
      const int bins = shape.dims[2];
      if (bins == expected_x_bins) {
        x_index_ = i;
      } else if (bins == expected_y_bins) {
        y_index_ = i;
      }
    }
    if (x_index_ < 0 || y_index_ < 0) {
      keypoint_runtime_utils::setError(
          error, "unable to locate SimCC [1,17,2*width/height] outputs");
      return false;
    }
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor) const {
    cv::Mat resized;
    cv::resize(image, resized,
               cv::Size(session_.inputWidth(), session_.inputHeight()), 0, 0,
               cv::INTER_LINEAR);
    bmrt_runtime::writeImageToTensor(
        resized, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  bool decodeOutputs(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                     int image_width, int image_height,
                     KeypointResult *result, std::string *error) const {
    if (x_index_ < 0 || y_index_ < 0 ||
        x_index_ >= static_cast<int>(outputs.size()) ||
        y_index_ >= static_cast<int>(outputs.size())) {
      keypoint_runtime_utils::setError(error, "invalid SimCC output index");
      return false;
    }
    const auto &x = outputs[static_cast<size_t>(x_index_)];
    const auto &y = outputs[static_cast<size_t>(y_index_)];
    const int x_bins = x.shape.dims[2];
    const int y_bins = y.shape.dims[2];
    if (x_bins <= 0 || y_bins <= 0 ||
        x.data.size() < static_cast<size_t>(kSimccKeypointCount * x_bins) ||
        y.data.size() < static_cast<size_t>(kSimccKeypointCount * y_bins)) {
      keypoint_runtime_utils::setError(error, "unexpected SimCC output size");
      return false;
    }

    result->clear();
    result->width = image_width;
    result->height = image_height;
    result->points.reserve(kSimccKeypointCount);
    const float x_scale = static_cast<float>(image_width) / session_.inputWidth();
    const float y_scale = static_cast<float>(image_height) / session_.inputHeight();
    for (int keypoint = 0; keypoint < kSimccKeypointCount; ++keypoint) {
      const auto x_begin = x.data.begin() + keypoint * x_bins;
      const auto y_begin = y.data.begin() + keypoint * y_bins;
      const auto x_max = std::max_element(x_begin, x_begin + x_bins);
      const auto y_max = std::max_element(y_begin, y_begin + y_bins);
      Point point;
      point.x = std::max(0.0f, std::min(
          (static_cast<float>(std::distance(x_begin, x_max)) / 2.0f) * x_scale,
          static_cast<float>(image_width)));
      point.y = std::max(0.0f, std::min(
          (static_cast<float>(std::distance(y_begin, y_max)) / 2.0f) * y_scale,
          static_cast<float>(image_height)));
      point.score = std::min(*x_max, *y_max);
      result->points.push_back(point);
    }
    return true;
  }

  bool inferMat(const cv::Mat &image, KeypointResult *result,
                std::string *error) {
    if (!result) {
      keypoint_runtime_utils::setError(error, "keypoint result pointer is null");
      return false;
    }
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
  int x_index_ = -1;
  int y_index_ = -1;
};

class HandRuntime : public KeypointRuntime {
 public:
  bool open(const KeypointDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = keypoint_runtime_utils::resolveModelToken(
        config, requested_model_type, "KEYPOINT_HAND", error);
    if (model_type.empty() || !loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.input_type.empty()) descriptor_.input_type = "rgb";
    if (descriptor_.mean.empty()) {
      descriptor_.mean = {0.485f * 255.0f, 0.456f * 255.0f, 0.406f * 255.0f};
    }
    if (descriptor_.scale.empty()) {
      descriptor_.scale = {1.0f / (255.0f * 0.229f),
                           1.0f / (255.0f * 0.224f),
                           1.0f / (255.0f * 0.225f)};
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f);
    EngineConfig engine_config;
    engine_config.model_descriptor_file = config.model_spec;
    engine_config.model_dir = config.model_dir;
    engine_config.bmrt_firmware = config.firmware;
    if (!session_.open(engine_config, descriptor_, error)) return false;
    const bm_net_info_t *net_info = session_.netInfo();
    const bm_shape_t &output = net_info->stages[0].output_shapes[0];
    if (!session_.nchwLayout() ||
        (session_.inputDtype() != BM_INT8 && session_.inputDtype() != BM_UINT8) ||
        net_info->output_num != 1 || output.num_dims != 3 ||
        output.dims[0] != 1 || output.dims[1] != 21 || output.dims[2] != 2) {
      keypoint_runtime_utils::setError(error,
          "hand keypoint requires NCHW int8/uint8 input and [1,21,2] output");
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
    if (resolved_model_type) *resolved_model_type = model_type;
    return true;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      keypoint_runtime_utils::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return runMat(image, result, error);
  }

  bool runMat(const cv::Mat &image, KeypointResult *result,
              std::string *error) override {
    if (image.empty()) {
      keypoint_runtime_utils::setError(error, "hand keypoint input is empty");
      return false;
    }
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(session_.inputWidth(), session_.inputHeight()));
    std::vector<float> input;
    bmrt_runtime::writeImageToTensor(resized,
        bmrt_runtime::wantsRgbInput(descriptor_, true), session_.nchwLayout(),
        mean_, scale_, &input);
    std::vector<bmrt_runtime::OutputTensor> outputs;
    return session_.launch(input, &outputs, error) &&
           decode(outputs, image.cols, image.rows, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) override {
    if (!frame.native || !result) {
      keypoint_runtime_utils::setError(error, "hand keypoint frame/result pointer is null");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int width = static_cast<int>(video->stVFrame.u32Width);
    const int height = static_cast<int>(video->stVFrame.u32Height);
    return runFrameCrop(frame, 0, 0, width, height, result, error);
  }

  bool runFrameCrop(const Frame &frame, int x, int y, int width, int height,
                    KeypointResult *result, std::string *error) override {
    if (!frame.native || !result || x < 0 || y < 0 || width <= 0 || height <= 0) {
      keypoint_runtime_utils::setError(error, "invalid hand keypoint crop");
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    const int frame_width = static_cast<int>(video->stVFrame.u32Width);
    const int frame_height = static_cast<int>(video->stVFrame.u32Height);
    if (x + width > frame_width || y + height > frame_height) {
      keypoint_runtime_utils::setError(error, "hand keypoint crop is outside frame");
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Roi roi;
    roi.x = x;
    roi.y = y;
    roi.width = width;
    roi.height = height;
    if (!preprocessor_->preprocess(frame.native, &roi, error)) {
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    return session_.launchDevice(preprocessor_->inputMemory(), &outputs, error) &&
           decode(outputs, width, height, result, error);
  }

  void reset() override { preprocessor_.reset(); session_.close(); }
  bool initialized() const override { return session_.opened(); }

 private:
  bool decode(const std::vector<bmrt_runtime::OutputTensor> &outputs, int width,
              int height, KeypointResult *result, std::string *error) const {
    if (!result || outputs.size() != 1 || outputs[0].data.size() < 42) {
      keypoint_runtime_utils::setError(error, "unexpected hand keypoint output");
      return false;
    }
    result->clear();
    result->width = width;
    result->height = height;
    result->points.reserve(21);
    for (int index = 0; index < 21; ++index) {
      Point point;
      point.x = std::max(0.0f, std::min(outputs[0].data[index * 2] * width,
                                         static_cast<float>(width)));
      point.y = std::max(0.0f, std::min(outputs[0].data[index * 2 + 1] * height,
                                         static_cast<float>(height)));
      point.score = 1.0f;
      result->points.push_back(point);
    }
    return true;
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor_;
};

}  // namespace

class KeypointDetector::Impl {
 public:
 bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = keypoint_runtime_utils::resolveModelToken(
        config, requested_model_type, "KEYPOINT_HAND", error);
    if (model_type.empty()) {
      return false;
    }

    runtime_.reset();
    if (startsWith(toUpper(model_type), "KEYPOINT_YOLOV8POSE")) {
      std::unique_ptr<PoseYolov8Runtime> pose_runtime(new PoseYolov8Runtime());
      if (!pose_runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(pose_runtime);
      return true;
    }
    if (startsWith(toUpper(model_type), "KEYPOINT_SIMCC")) {
      std::unique_ptr<SimccRuntime> simcc_runtime(new SimccRuntime());
      if (!simcc_runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(simcc_runtime);
      return true;
    }
    if (startsWith(toUpper(model_type), "KEYPOINT_HAND")) {
      std::unique_ptr<HandRuntime> hand_runtime(new HandRuntime());
      if (!hand_runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(hand_runtime);
      return true;
    }
    keypoint_runtime_utils::setError(
        error, "unsupported keypoint model_type for custom BMRT runtime: " +
                   model_type);
    return false;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    if (!runtime_) {
      keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    if (!runtime_) {
      keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
      return false;
    }
    return runtime_->runFrame(frame, result, error);
  }

  bool runMat(const cv::Mat &image, KeypointResult *result,
              std::string *error) {
    if (!runtime_) {
      keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
      return false;
    }
    return runtime_->runMat(image, result, error);
  }

  bool runFrameCrop(const Frame &frame, int x, int y, int width, int height,
                    KeypointResult *result, std::string *error) {
    if (!runtime_) {
      keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
      return false;
    }
    return runtime_->runFrameCrop(frame, x, y, width, height, result, error);
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
  std::unique_ptr<KeypointRuntime> runtime_;
};

KeypointDetector::KeypointDetector() = default;

KeypointDetector::KeypointDetector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

KeypointDetector::~KeypointDetector() {
  reset();
  delete impl_;
}

KeypointDetector::KeypointDetector(KeypointDetector &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

KeypointDetector &KeypointDetector::operator=(KeypointDetector &&other) noexcept {
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

bool KeypointDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
}

bool KeypointDetector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool KeypointDetector::load(const std::string &model_spec,
                            const std::string &firmware,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool KeypointDetector::load(const std::string &model_spec,
                            const std::string &firmware,
                            const std::string &model_dir,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool KeypointDetector::run(const std::string &image_path, KeypointResult *result,
                           std::string *error) {
  return estimate(image_path, result, error);
}

bool KeypointDetector::runFrame(const Frame &frame, KeypointResult *result,
                                std::string *error) {
  if (!impl_) {
    keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool KeypointDetector::runMat(const cv::Mat &image, KeypointResult *result,
                              std::string *error) {
  if (!impl_) {
    keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->runMat(image, result, error);
}

bool KeypointDetector::runFrameCrop(const Frame &frame, int x, int y,
                                    int width, int height,
                                    KeypointResult *result,
                                    std::string *error) {
  if (!impl_) {
    keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->runFrameCrop(frame, x, y, width, height, result, error);
}

bool KeypointDetector::estimate(const std::string &image_path,
                                KeypointResult *result,
                                std::string *error) {
  if (!impl_) {
    keypoint_runtime_utils::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool KeypointDetector::initialized() const {
  return impl_ && impl_->initialized();
}

std::string KeypointDetector::modelType() const { return requested_model_type_; }

void KeypointDetector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
