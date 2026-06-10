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

#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_utils.h"
#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/tdl_sdk_utils.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace tdl_app {
namespace {

constexpr int kPoseKeypointDims = 3;

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
        error, "keypoint runtime only supports RGB/BGR/NV12/NV21/YUV400 frame input");
  }

  CVI_SYS_Munmap(mapped, map_size);
  if (!ok || image->empty()) {
    private_tdl_sdk::setError(error, "failed to convert frame to BGR image");
    return false;
  }
  return true;
}

struct KeypointRuntime {
  virtual ~KeypointRuntime() = default;
  virtual bool run(const std::string &image_path, KeypointResult *result,
                   std::string *error) = 0;
  virtual bool runFrame(const Frame &frame, KeypointResult *result,
                        std::string *error) = 0;
  virtual void reset() = 0;
  virtual bool initialized() const = 0;
};

class PoseYolov8Runtime : public KeypointRuntime {
 public:
  bool open(const KeypointDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type =
        private_tdl_sdk::resolveModelToken(config, requested_model_type,
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

    if (resolved_model_type) {
      *resolved_model_type = model_type_;
    }
    return true;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      private_tdl_sdk::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return inferMat(image, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    cv::Mat image;
    if (!frameToBgrMat(frame, &image, error)) {
      return false;
    }
    return inferMat(image, result, error);
  }

  void reset() override { session_.close(); }
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
        private_tdl_sdk::setError(
            error, "incomplete yolov8 pose output branches");
        return false;
      }
      const int branch_keypoint_count =
          branch.kpt_channels / kPoseKeypointDims;
      if (branch_keypoint_count <= 0) {
        private_tdl_sdk::setError(
            error, "invalid yolov8 pose keypoint channel count");
        return false;
      }
      if (keypoint_count_ == 0) {
        keypoint_count_ = branch_keypoint_count;
      } else if (keypoint_count_ != branch_keypoint_count) {
        private_tdl_sdk::setError(
            error, "inconsistent yolov8 pose keypoint branch shapes");
        return false;
      }
      branches_.push_back(branch);
    }
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor, float *ratio,
                  int *top, int *left) const {
    *ratio = std::min(static_cast<float>(session_.inputHeight()) / image.rows,
                      static_cast<float>(session_.inputWidth()) / image.cols);
    const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
    const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));
    *top = (session_.inputHeight() - resized_h) / 2;
    *left = (session_.inputWidth() - resized_w) / 2;

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

  bool inferMat(const cv::Mat &image, KeypointResult *result,
                std::string *error) {
    if (!result) {
      private_tdl_sdk::setError(error, "keypoint result pointer is null");
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
                                    static_cast<float>(image.cols)));
        candidate.box.y1 =
            std::max(0.0f, std::min((y1 - top) / ratio,
                                    static_cast<float>(image.rows)));
        candidate.box.x2 =
            std::max(0.0f, std::min((x2 - left) / ratio,
                                    static_cast<float>(image.cols)));
        candidate.box.y2 =
            std::max(0.0f, std::min((y2 - top) / ratio,
                                    static_cast<float>(image.rows)));

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
                                 static_cast<float>(image.cols)));
          point.y =
              std::max(0.0f, std::min(
                                 ((kpt[(k * 3 + 1) * anchor_count + anchor] *
                                       2.0f +
                                   static_cast<float>(anchor_y)) *
                                      branch.stride -
                                  top) /
                                     ratio,
                                 static_cast<float>(image.rows)));
          point.score = sigmoid(
              kpt[(k * 3 + 2) * anchor_count + anchor]);
          candidate.points.push_back(point);
        }
        candidates.push_back(std::move(candidate));
      }
    }

    if (candidates.empty()) {
      result->clear();
      result->width = image.cols;
      result->height = image.rows;
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
      result->width = image.cols;
      result->height = image.rows;
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
    result->width = image.cols;
    result->height = image.rows;
    if (best_candidate) {
      result->points = best_candidate->points;
    }
    return true;
  }

  std::string model_type_;
  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::vector<Branch> branches_;
  int keypoint_count_ = 0;
};

class LegacyKeypointRuntime : public KeypointRuntime {
 public:
  bool load(const KeypointDetector::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "KEYPOINT_HAND", error);
    if (model_type.empty()) {
      return false;
    }
    if (!session_.open(config, model_type, error)) {
      return false;
    }
    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.load(image_path, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.wrap(frame, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  void reset() override { session_.close(); }
  bool initialized() const override { return session_.initialized(); }

 private:
  bool infer(TDLImage image, KeypointResult *result, std::string *error) {
    if (!session_.initialized()) {
      private_tdl_sdk::setError(error, "keypoint detector is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(error, "keypoint result pointer is null");
      return false;
    }

    TDLKeypoint meta;
    std::memset(&meta, 0, sizeof(meta));
    const int ret =
        TDL_Keypoint(session_.handle(), session_.modelId(), image, &meta);
    if (ret != 0) {
      private_tdl_sdk::setError(
          error, "TDL_Keypoint failed, ret=" + std::to_string(ret));
      return false;
    }

    result->clear();
    result->width = static_cast<int>(meta.width);
    result->height = static_cast<int>(meta.height);
    result->points.reserve(meta.size);
    for (std::uint32_t i = 0; i < meta.size; ++i) {
      Point point;
      point.x = meta.info[i].x * meta.width;
      point.y = meta.info[i].y * meta.height;
      point.score = meta.info[i].score;
      result->points.push_back(point);
    }
    TDL_ReleaseKeypointMeta(&meta);
    return true;
  }

  private_tdl_sdk::Session session_;
};

}  // namespace

class KeypointDetector::Impl {
 public:
 bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
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
    private_tdl_sdk::setError(
        error, "unsupported keypoint model_type for custom BMRT runtime: " +
                   model_type);
    return false;
  }

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "keypoint detector is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "keypoint detector is not initialized");
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
    private_tdl_sdk::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool KeypointDetector::estimate(const std::string &image_path,
                                KeypointResult *result,
                                std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "keypoint detector is not initialized");
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
