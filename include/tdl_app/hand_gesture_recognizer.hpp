#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/vision_task_types.hpp"

namespace tdl_app {

class Detector;
class KeypointDetector;

// Static gestures recognized from the 21 MediaPipe-style hand keypoints.
enum class HandGesture {
  Unknown,
  Fist,
  One,
  Two,
  Three,
  Four,
  Five,
  ThumbUp,
  Ok,
  Rock,
  Pinch,
};

const char *handGestureName(HandGesture gesture);

struct HandGestureResult {
  Box box;
  KeypointResult keypoints;
  HandGesture gesture = HandGesture::Unknown;
  float score = 0.0f;
};

// Online hand gesture recognition: YOLOv8 hand detection, 21 keypoints, and
// the vendor 42-value keypoint gesture classifier.
class HandGestureRecognizer {
 public:
  struct Config {
    std::string detector_model_spec;
    std::string keypoint_model_spec;
    // Empty selects keypoint_hand_gesture.mud beside keypoint_model_spec.
    std::string gesture_classifier_model_spec;
    std::string firmware;
    float hand_threshold = 0.35f;
    float iou_threshold = 0.45f;
    float roi_expand_ratio = 0.25f;
    int max_hands = 2;
  };

  HandGestureRecognizer();
  ~HandGestureRecognizer();

  HandGestureRecognizer(const HandGestureRecognizer &) = delete;
  HandGestureRecognizer &operator=(const HandGestureRecognizer &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool initialized() const;

  // Returns an empty result list when no hand meets hand_threshold.
  bool recognizeFrame(const Frame &frame,
                      std::vector<HandGestureResult> *results,
                      std::string *error = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
