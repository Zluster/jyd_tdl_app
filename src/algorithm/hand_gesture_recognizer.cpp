#include "tdl_app/hand_gesture_recognizer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cvi_comm_video.h"
#include "algorithm/private/bmrt_utils.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/keypoint_detector.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kHandPointCount = 21;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

float clamp(float value, float lower, float upper) {
  return std::max(lower, std::min(value, upper));
}

struct Roi {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

bool makeRoi(const Box &box, int frame_width, int frame_height,
             float expand_ratio, Roi *roi) {
  if (!roi || !box.valid() || frame_width <= 1 || frame_height <= 1) {
    return false;
  }
  const float width = box.width();
  const float height = box.height();
  const float side = std::max(width, height) *
                     std::max(1.0f, 1.0f + clamp(expand_ratio, 0.0f, 1.0f));
  const float center_x = (box.x1 + box.x2) * 0.5f;
  const float center_y = (box.y1 + box.y2) * 0.5f;
  int left = static_cast<int>(std::floor(center_x - side * 0.5f));
  int top = static_cast<int>(std::floor(center_y - side * 0.5f));
  int right = static_cast<int>(std::ceil(center_x + side * 0.5f));
  int bottom = static_cast<int>(std::ceil(center_y + side * 0.5f));
  left = std::max(0, std::min(left, frame_width - 1));
  top = std::max(0, std::min(top, frame_height - 1));
  right = std::max(left + 1, std::min(right, frame_width));
  bottom = std::max(top + 1, std::min(bottom, frame_height));

  // NV12/NV21 sources require chroma-aligned crop coordinates and dimensions.
  left &= ~1;
  top &= ~1;
  right = std::min(frame_width, (right + 1) & ~1);
  bottom = std::min(frame_height, (bottom + 1) & ~1);
  if (right <= left || bottom <= top) return false;
  roi->x = left;
  roi->y = top;
  roi->width = right - left;
  roi->height = bottom - top;
  return true;
}

void mapRoi(const Roi &roi, KeypointResult *keypoints) {
  if (!keypoints) return;
  for (Point &point : keypoints->points) {
    point.x += roi.x;
    point.y += roi.y;
  }
  keypoints->width = std::max(keypoints->width, roi.x + roi.width);
  keypoints->height = std::max(keypoints->height, roi.y + roi.height);
}

std::string defaultGestureClassifierSpec(const std::string &keypoint_spec) {
  const std::string::size_type slash = keypoint_spec.find_last_of("/\\\\");
  if (slash == std::string::npos) {
    return "keypoint_hand_gesture.mud";
  }
  return keypoint_spec.substr(0, slash + 1) + "keypoint_hand_gesture.mud";
}

HandGesture gestureFromVendorLabel(const std::string &label) {
  if (label == "fist") return HandGesture::Fist;
  if (label == "one") return HandGesture::One;
  if (label == "two") return HandGesture::Two;
  if (label == "three" || label == "three2") return HandGesture::Three;
  if (label == "four") return HandGesture::Four;
  if (label == "five") return HandGesture::Five;
  if (label == "ok") return HandGesture::Ok;
  return HandGesture::Unknown;
}

class KeypointGestureClassifier {
 public:
  bool open(const std::string &model_spec, const std::string &firmware,
            std::string *error) {
    reset();
    if (!loadModelDescriptor(model_spec, &descriptor_, error)) return false;

    EngineConfig config;
    config.model_descriptor_file = model_spec;
    config.bmrt_firmware = firmware;
    if (!session_.open(config, descriptor_, error)) return false;
    if (session_.inputElementCount() != kHandPointCount * 2) {
      setError(error, "hand gesture classifier requires 42 input values");
      reset();
      return false;
    }
    if (descriptor_.labels.size() != 9 || session_.netInfo()->output_num != 1) {
      setError(error, "hand gesture classifier requires nine labeled outputs");
      reset();
      return false;
    }
    return true;
  }

  bool classify(const KeypointResult &keypoints, HandGesture *gesture,
                float *score, std::string *error) const {
    if (!gesture || !score || !session_.opened()) {
      setError(error, "hand gesture classifier is not initialized");
      return false;
    }
    if (keypoints.points.size() != kHandPointCount || keypoints.width <= 0 ||
        keypoints.height <= 0) {
      setError(error, "hand gesture classifier requires 21 crop-relative points");
      return false;
    }

    std::vector<float> input;
    input.reserve(kHandPointCount * 2);
    for (const Point &point : keypoints.points) {
      input.push_back(clamp(point.x / keypoints.width, 0.0f, 1.0f));
      input.push_back(clamp(point.y / keypoints.height, 0.0f, 1.0f));
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input, &outputs, error) || outputs.size() != 1 ||
        outputs[0].data.size() != descriptor_.labels.size()) {
      if (error && error->empty()) {
        setError(error, "unexpected hand gesture classifier output");
      }
      return false;
    }

    const std::vector<float> &logits = outputs[0].data;
    const size_t best = static_cast<size_t>(std::distance(
        logits.begin(), std::max_element(logits.begin(), logits.end())));
    const float max_logit = logits[best];
    float sum = 0.0f;
    for (float logit : logits) sum += std::exp(logit - max_logit);
    *gesture = gestureFromVendorLabel(descriptor_.labels[best]);
    *score = sum > 0.0f ? 1.0f / sum : 0.0f;
    return true;
  }

  bool initialized() const { return session_.opened(); }
  void reset() {
    session_.close();
    descriptor_ = ModelDescriptor{};
  }

 private:
  ModelDescriptor descriptor_;
  bmrt_runtime::Session session_;
};

}  // namespace

const char *handGestureName(HandGesture gesture) {
  switch (gesture) {
    case HandGesture::Fist: return "fist";
    case HandGesture::One: return "one";
    case HandGesture::Two: return "two";
    case HandGesture::Three: return "three";
    case HandGesture::Four: return "four";
    case HandGesture::Five: return "five";
    case HandGesture::ThumbUp: return "thumb_up";
    case HandGesture::Ok: return "ok";
    case HandGesture::Rock: return "rock";
    case HandGesture::Pinch: return "pinch";
    default: return "unknown";
  }
}

struct HandGestureRecognizer::Impl {
  Impl() : detector(Detector::yolov8()), keypoint(KeypointDetector::hand()) {}

  bool ensureKeypoint(std::string *error) {
    if (keypoint_loaded) return true;
    if (!keypoint.load(ModelSessionConfig::fromSpec(
            config.keypoint_model_spec, config.firmware), error)) {
      return false;
    }
    keypoint_loaded = true;
    return true;
  }

  bool openGestureClassifier(std::string *error) {
    const std::string model_spec = config.gesture_classifier_model_spec.empty()
        ? defaultGestureClassifierSpec(config.keypoint_model_spec)
        : config.gesture_classifier_model_spec;
    return gesture_classifier.open(model_spec, config.firmware, error);
  }

  Config config;
  Detector detector;
  KeypointDetector keypoint;
  KeypointGestureClassifier gesture_classifier;
  bool keypoint_loaded = false;
  bool loaded = false;
};

HandGestureRecognizer::HandGestureRecognizer() : impl_(new Impl()) {}
HandGestureRecognizer::~HandGestureRecognizer() = default;

bool HandGestureRecognizer::load(const Config &config, std::string *error) {
  if (!impl_) {
    setError(error, "hand gesture recognizer is unavailable");
    return false;
  }
  if (config.detector_model_spec.empty() || config.keypoint_model_spec.empty()) {
    setError(error, "detector_model_spec and keypoint_model_spec are required");
    return false;
  }
  if (config.max_hands <= 0) {
    setError(error, "max_hands must be positive");
    return false;
  }

  impl_->loaded = false;
  impl_->keypoint.reset();
  impl_->keypoint_loaded = false;
  impl_->gesture_classifier.reset();
  impl_->config = config;
  if (!impl_->detector.load(ModelSessionConfig::fromSpec(
          config.detector_model_spec, config.firmware), error)) {
    return false;
  }
  if (!impl_->openGestureClassifier(error)) return false;
  impl_->loaded = true;
  return true;
}

bool HandGestureRecognizer::initialized() const {
  return impl_ && impl_->loaded && impl_->detector.initialized() &&
         impl_->gesture_classifier.initialized();
}

bool HandGestureRecognizer::recognizeFrame(
    const Frame &frame, std::vector<HandGestureResult> *results,
    std::string *error) {
  if (!results) {
    setError(error, "hand gesture results pointer is null");
    return false;
  }
  results->clear();
  if (!initialized()) {
    setError(error, "hand gesture recognizer is not initialized");
    return false;
  }
  if (!frame.native) {
    setError(error, "hand gesture frame has no native video frame");
    return false;
  }

  AlgorithmResult detected;
  if (!impl_->detector.detectFrame(
          frame, InferOptions::detection(impl_->config.hand_threshold,
                                         impl_->config.iou_threshold),
          &detected, error)) {
    return false;
  }
  const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
  const int frame_width = static_cast<int>(video->stVFrame.u32Width);
  const int frame_height = static_cast<int>(video->stVFrame.u32Height);
  if (frame_width <= 1 || frame_height <= 1) {
    setError(error, "hand gesture frame dimensions are invalid");
    return false;
  }

  for (const Box &box : detected.boxes) {
    if (static_cast<int>(results->size()) >= impl_->config.max_hands) break;
    Roi roi;
    if (!makeRoi(box, frame_width, frame_height,
                 impl_->config.roi_expand_ratio, &roi)) {
      continue;
    }
    if (!impl_->ensureKeypoint(error)) {
      return false;
    }
    HandGestureResult result;
    result.box = box;
    if (!impl_->keypoint.runFrameCrop(frame, roi.x, roi.y, roi.width, roi.height,
                                      &result.keypoints, error)) {
      return false;
    }
    if (!impl_->gesture_classifier.classify(result.keypoints, &result.gesture,
                                            &result.score, error)) {
      return false;
    }
    mapRoi(roi, &result.keypoints);
    results->push_back(std::move(result));
  }
  return true;
}

}  // namespace tdl_app
