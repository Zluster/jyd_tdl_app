#include "tdl_app/hand_gesture_recognizer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cvi_comm_video.h"
#include "tdl_app/detector.hpp"
#include "tdl_app/keypoint_detector.hpp"

namespace tdl_app {
namespace {

constexpr int kHandPointCount = 21;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

float distance(const Point &a, const Point &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
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

bool isFingerExtended(const KeypointResult &points, int mcp, int pip,
                      int tip) {
  const Point &wrist = points.points[0];
  const float tip_distance = distance(points.points[tip], wrist);
  const float pip_distance = distance(points.points[pip], wrist);
  const float base_distance = distance(points.points[mcp], wrist);
  return tip_distance > pip_distance * 1.12f &&
         tip_distance > base_distance * 1.35f;
}

HandGesture classify(const KeypointResult &points, float *score) {
  if (score) *score = 0.0f;
  if (points.points.size() != kHandPointCount) return HandGesture::Unknown;

  const float palm_width = distance(points.points[5], points.points[17]);
  if (palm_width < 2.0f) return HandGesture::Unknown;

  const bool index = isFingerExtended(points, 5, 6, 8);
  const bool middle = isFingerExtended(points, 9, 10, 12);
  const bool ring = isFingerExtended(points, 13, 14, 16);
  const bool little = isFingerExtended(points, 17, 18, 20);
  const float thumb_length = distance(points.points[4], points.points[2]);
  const float thumb_base = distance(points.points[3], points.points[2]);
  const bool thumb = thumb_length > thumb_base * 1.35f &&
                     thumb_length > palm_width * 0.45f;
  const float thumb_index_distance = distance(points.points[4], points.points[8]);
  const bool thumb_index_touch = thumb_index_distance < palm_width * 0.36f;
  const int extended = static_cast<int>(thumb) + static_cast<int>(index) +
                       static_cast<int>(middle) + static_cast<int>(ring) +
                       static_cast<int>(little);

  if (thumb_index_touch && middle && ring && little) {
    if (score) *score = clamp(1.0f - thumb_index_distance / (palm_width * 0.36f),
                              0.0f, 1.0f);
    return HandGesture::Ok;
  }
  if (thumb_index_touch && !middle && !ring && !little) {
    if (score) *score = clamp(1.0f - thumb_index_distance / (palm_width * 0.45f),
                              0.0f, 1.0f);
    return HandGesture::Pinch;
  }
  if (index && little && !middle && !ring) {
    if (score) *score = 0.85f;
    return HandGesture::Rock;
  }
  if (extended == 0) {
    if (score) *score = 0.80f;
    return HandGesture::Fist;
  }
  if (thumb && !index && !middle && !ring && !little) {
    if (score) *score = 0.80f;
    return HandGesture::ThumbUp;
  }
  if (!thumb && index && !middle && !ring && !little) {
    if (score) *score = 0.85f;
    return HandGesture::One;
  }
  if (!thumb && index && middle && !ring && !little) {
    if (score) *score = 0.85f;
    return HandGesture::Two;
  }
  if (!thumb && index && middle && ring && !little) {
    if (score) *score = 0.85f;
    return HandGesture::Three;
  }
  if (!thumb && index && middle && ring && little) {
    if (score) *score = 0.85f;
    return HandGesture::Four;
  }
  if (extended == 5) {
    if (score) *score = 0.90f;
    return HandGesture::Five;
  }
  return HandGesture::Unknown;
}

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

  Config config;
  Detector detector;
  KeypointDetector keypoint;
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
  if (!impl_->detector.load(ModelSessionConfig::fromSpec(
          config.detector_model_spec, config.firmware), error)) {
    return false;
  }
  impl_->config = config;
  impl_->loaded = true;
  return true;
}

bool HandGestureRecognizer::initialized() const {
  return impl_ && impl_->loaded && impl_->detector.initialized();
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
    mapRoi(roi, &result.keypoints);
    result.gesture = classify(result.keypoints, &result.score);
    results->push_back(std::move(result));
  }
  return true;
}

}  // namespace tdl_app
