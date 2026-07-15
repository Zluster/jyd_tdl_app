#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "tdl_app/keypoint_detector.hpp"

namespace tdl_app {

enum class HumanPoseClass {
  Unknown,
  Standing,
  Sitting,
  Lying,
  LeftHandUp,
  RightHandUp,
  BothHandsUp,
};

const char *humanPoseClassName(HumanPoseClass pose);

struct PoseClassificationProfile {
  double keypoint_ms = 0.0;
  double keypoint_smooth_ms = 0.0;
  double rule_ms = 0.0;
  double label_smooth_ms = 0.0;
  double total_ms = 0.0;
};

struct PoseClassificationResult {
  KeypointResult keypoints;
  HumanPoseClass raw_pose = HumanPoseClass::Unknown;
  HumanPoseClass pose = HumanPoseClass::Unknown;
  float confidence = 0.0f;
  int history_size = 0;
  PoseClassificationProfile profile;

  void clear() { *this = PoseClassificationResult{}; }
};

class PoseClassifier {
 public:
  struct Config {
    ModelSessionConfig keypoint;
    float keypoint_threshold = 0.05f;
    float coordinate_ema_alpha = 0.65f;
    int label_smooth_frames = 5;
  };

  PoseClassifier() = default;
  ~PoseClassifier() = default;
  PoseClassifier(const PoseClassifier &) = delete;
  PoseClassifier &operator=(const PoseClassifier &) = delete;
  PoseClassifier(PoseClassifier &&) noexcept = default;
  PoseClassifier &operator=(PoseClassifier &&) noexcept = default;

  bool load(const Config &config, std::string *error = nullptr);
  bool run(const std::string &image_path, PoseClassificationResult *result,
           std::string *error = nullptr);
  bool runFrame(const Frame &frame, PoseClassificationResult *result,
                std::string *error = nullptr);
  bool classify(const KeypointResult &keypoints,
                PoseClassificationResult *result,
                std::string *error = nullptr);

  bool initialized() const;
  void resetSmoothing();
  void reset();
  const Config &config() const { return config_; }

 private:
  bool finishClassification(const KeypointResult &keypoints,
                            double keypoint_ms,
                            const std::chrono::steady_clock::time_point &begin,
                            PoseClassificationResult *result,
                            std::string *error);
  KeypointResult smoothKeypoints(const KeypointResult &keypoints);
  HumanPoseClass applyRules(const KeypointResult &keypoints,
                            float *confidence) const;
  HumanPoseClass smoothLabel(HumanPoseClass raw_pose);

  Config config_;
  KeypointDetector detector_;
  std::vector<Point> previous_points_;
  std::deque<HumanPoseClass> pose_history_;
};

}  // namespace tdl_app
