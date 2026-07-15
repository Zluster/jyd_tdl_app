#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace jyd_tracker {

struct Detection {
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
  float score = 0.0f;
  int class_id = -1;
};

struct Track {
  std::uint64_t id = 0;
  Detection box;
  int age = 0;
  int missed = 0;
  bool counted = false;
  float previous_center_x = 0.0f;
  std::array<float, 8> kalman_mean{{0.0f}};
  std::array<float, 64> kalman_covariance{{0.0f}};
  bool kalman_initialized = false;
};

class ByteTracker {
 public:
  struct Config {
    float high_score = 0.45f;
    float low_score = 0.15f;
    float iou_threshold = 0.30f;
    int max_missed = 30;
  };

  ByteTracker();
  explicit ByteTracker(Config config);

  std::vector<Track> update(const std::vector<Detection> &detections);
  int updateLineCount(float line_x);
  void reset();

 private:
  static float iou(const Detection &lhs, const Detection &rhs);
  static void initializeKalman(Track *track);
  static void predictKalman(Track *track);
  static void updateKalman(Track *track, const Detection &detection);
  void associate(const std::vector<Detection> &detections,
                 std::vector<bool> *matched_tracks,
                 std::vector<bool> *matched_detections);

  Config config_;
  std::vector<Track> tracks_;
  std::uint64_t next_id_ = 1;
};

}  // namespace jyd_tracker
