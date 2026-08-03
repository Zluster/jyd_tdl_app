#include "tdl_app/pose_classifier.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

double elapsedMs(const std::chrono::steady_clock::time_point &begin,
                 const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

Point midpoint(const Point &lhs, const Point &rhs) {
  Point out;
  out.x = (lhs.x + rhs.x) * 0.5f;
  out.y = (lhs.y + rhs.y) * 0.5f;
  out.score = std::min(lhs.score, rhs.score);
  return out;
}

float distance(const Point &lhs, const Point &rhs) {
  const float dx = lhs.x - rhs.x;
  const float dy = lhs.y - rhs.y;
  return std::sqrt(dx * dx + dy * dy);
}

float jointAngle(const Point &a, const Point &joint, const Point &b) {
  const float ax = a.x - joint.x;
  const float ay = a.y - joint.y;
  const float bx = b.x - joint.x;
  const float by = b.y - joint.y;
  const float denominator =
      std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by));
  if (denominator <= 1e-5f) return 180.0f;
  const float cosine = std::max(-1.0f, std::min(1.0f,
      (ax * bx + ay * by) / denominator));
  return std::acos(cosine) * 180.0f / 3.14159265358979323846f;
}

float normalizedScore(float score) {
  return std::max(0.0f, std::min(1.0f, score));
}

}  // namespace

const char *humanPoseClassName(HumanPoseClass pose) {
  switch (pose) {
    case HumanPoseClass::Standing: return "standing";
    case HumanPoseClass::Sitting: return "sitting";
    case HumanPoseClass::Lying: return "lying";
    case HumanPoseClass::LeftHandUp: return "left_hand_up";
    case HumanPoseClass::RightHandUp: return "right_hand_up";
    case HumanPoseClass::BothHandsUp: return "both_hands_up";
    default: return "unknown";
  }
}

bool PoseClassifier::load(const Config &config, std::string *error) {
  reset();
  config_ = config;
  config_.coordinate_ema_alpha =
      std::max(0.0f, std::min(1.0f, config_.coordinate_ema_alpha));
  config_.label_smooth_frames = std::max(1, config_.label_smooth_frames);
  return detector_.load(config_.keypoint, error);
}

bool PoseClassifier::run(const std::string &image_path,
                         PoseClassificationResult *result,
                         std::string *error) {
  if (!initialized() || !result) {
    setError(error, "pose classifier/result is not initialized");
    return false;
  }
  const auto total_begin = std::chrono::steady_clock::now();
  KeypointResult keypoints;
  const auto keypoint_begin = std::chrono::steady_clock::now();
  if (!detector_.run(image_path, &keypoints, error)) return false;
  const double keypoint_ms =
      elapsedMs(keypoint_begin, std::chrono::steady_clock::now());
  return finishClassification(keypoints, keypoint_ms, total_begin, result, error);
}

bool PoseClassifier::runFrame(const Frame &frame,
                              PoseClassificationResult *result,
                              std::string *error) {
  if (!initialized() || !result) {
    setError(error, "pose classifier/result is not initialized");
    return false;
  }
  const auto total_begin = std::chrono::steady_clock::now();
  KeypointResult keypoints;
  const auto keypoint_begin = std::chrono::steady_clock::now();
  if (!detector_.runFrame(frame, &keypoints, error)) return false;
  const double keypoint_ms =
      elapsedMs(keypoint_begin, std::chrono::steady_clock::now());
  return finishClassification(keypoints, keypoint_ms, total_begin, result, error);
}

bool PoseClassifier::classify(const KeypointResult &keypoints,
                              PoseClassificationResult *result,
                              std::string *error) {
  if (!result) {
    setError(error, "pose classification result is null");
    return false;
  }
  const auto begin = std::chrono::steady_clock::now();
  return finishClassification(keypoints, 0.0, begin, result, error);
}

bool PoseClassifier::finishClassification(
    const KeypointResult &keypoints, double keypoint_ms,
    const std::chrono::steady_clock::time_point &begin,
    PoseClassificationResult *result, std::string *error) {
  if (keypoints.pointCount() != 17) {
    setError(error, "human pose classification requires exactly 17 COCO points");
    return false;
  }
  result->clear();
  result->profile.keypoint_ms = keypoint_ms;
  const auto coordinate_begin = std::chrono::steady_clock::now();
  result->keypoints = smoothKeypoints(keypoints);
  result->profile.keypoint_smooth_ms =
      elapsedMs(coordinate_begin, std::chrono::steady_clock::now());

  const auto rule_begin = std::chrono::steady_clock::now();
  result->raw_pose = applyRules(result->keypoints, &result->confidence);
  result->profile.rule_ms =
      elapsedMs(rule_begin, std::chrono::steady_clock::now());

  const auto label_begin = std::chrono::steady_clock::now();
  result->pose = smoothLabel(result->raw_pose);
  result->history_size = static_cast<int>(pose_history_.size());
  result->profile.label_smooth_ms =
      elapsedMs(label_begin, std::chrono::steady_clock::now());
  result->profile.total_ms =
      elapsedMs(begin, std::chrono::steady_clock::now());
  return true;
}

KeypointResult PoseClassifier::smoothKeypoints(
    const KeypointResult &keypoints) {
  KeypointResult smoothed = keypoints;
  if (previous_points_.size() != keypoints.points.size()) {
    previous_points_ = keypoints.points;
    return smoothed;
  }
  const float alpha = config_.coordinate_ema_alpha;
  for (size_t i = 0; i < smoothed.points.size(); ++i) {
    Point &current = smoothed.points[i];
    Point &previous = previous_points_[i];
    if (current.score >= config_.keypoint_threshold &&
        previous.score >= config_.keypoint_threshold) {
      current.x = alpha * current.x + (1.0f - alpha) * previous.x;
      current.y = alpha * current.y + (1.0f - alpha) * previous.y;
    }
    previous = current;
  }
  return smoothed;
}

HumanPoseClass PoseClassifier::applyRules(const KeypointResult &keypoints,
                                          float *confidence) const {
  const auto &p = keypoints.points;
  auto valid = [&](int index) {
    return p[static_cast<size_t>(index)].score >= config_.keypoint_threshold;
  };
  const bool torso_valid = valid(5) && valid(6) && valid(11) && valid(12);
  if (!torso_valid) {
    if (confidence) *confidence = 0.0f;
    return HumanPoseClass::Unknown;
  }
  const Point shoulder = midpoint(p[5], p[6]);
  const Point hip = midpoint(p[11], p[12]);
  const float torso = std::max(1.0f, distance(shoulder, hip));
  const float torso_dx = std::fabs(hip.x - shoulder.x);
  const float torso_dy = std::fabs(hip.y - shoulder.y);
  float score = std::min(
      std::min(normalizedScore(p[5].score), normalizedScore(p[6].score)),
      std::min(normalizedScore(p[11].score), normalizedScore(p[12].score)));

  if (torso_dx > torso_dy * 1.10f) {
    if (confidence) *confidence = score;
    return HumanPoseClass::Lying;
  }

  const bool left_up = valid(9) && p[9].y < p[5].y - torso * 0.10f;
  const bool right_up = valid(10) && p[10].y < p[6].y - torso * 0.10f;
  if (left_up || right_up) {
    if (left_up) score = std::min(score, normalizedScore(p[9].score));
    if (right_up) score = std::min(score, normalizedScore(p[10].score));
    if (confidence) *confidence = score;
    if (left_up && right_up) return HumanPoseClass::BothHandsUp;
    return left_up ? HumanPoseClass::LeftHandUp
                   : HumanPoseClass::RightHandUp;
  }

  std::vector<float> knee_angles;
  if (valid(11) && valid(13) && valid(15)) {
    knee_angles.push_back(jointAngle(p[11], p[13], p[15]));
  }
  if (valid(12) && valid(14) && valid(16)) {
    knee_angles.push_back(jointAngle(p[12], p[14], p[16]));
  }
  if (!knee_angles.empty() &&
      *std::min_element(knee_angles.begin(), knee_angles.end()) < 140.0f) {
    if (confidence) *confidence = score;
    return HumanPoseClass::Sitting;
  }

  const bool left_leg = valid(13) && valid(15) && p[15].y > hip.y + torso * 0.7f;
  const bool right_leg = valid(14) && valid(16) && p[16].y > hip.y + torso * 0.7f;
  if (torso_dy >= torso_dx && (left_leg || right_leg)) {
    if (confidence) *confidence = score;
    return HumanPoseClass::Standing;
  }
  if (confidence) *confidence = score * 0.5f;
  return HumanPoseClass::Unknown;
}

HumanPoseClass PoseClassifier::smoothLabel(HumanPoseClass raw_pose) {
  pose_history_.push_back(raw_pose);
  while (static_cast<int>(pose_history_.size()) > config_.label_smooth_frames) {
    pose_history_.pop_front();
  }
  std::map<HumanPoseClass, int> counts;
  for (HumanPoseClass pose : pose_history_) ++counts[pose];
  HumanPoseClass best = raw_pose;
  int best_count = -1;
  for (auto it = pose_history_.rbegin(); it != pose_history_.rend(); ++it) {
    const int count = counts[*it];
    if (count > best_count) {
      best = *it;
      best_count = count;
    }
  }
  return best;
}

bool PoseClassifier::initialized() const { return detector_.initialized(); }

void PoseClassifier::resetSmoothing() {
  previous_points_.clear();
  pose_history_.clear();
}

void PoseClassifier::reset() {
  detector_.reset();
  resetSmoothing();
  config_ = Config{};
}

}  // namespace tdl_app
