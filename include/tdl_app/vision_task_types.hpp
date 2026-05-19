#pragma once

#include <cstdint>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

struct KeypointResult {
  int width = 0;
  int height = 0;
  std::vector<Point> points;

  void clear() {
    width = 0;
    height = 0;
    points.clear();
  }

  bool empty() const { return points.empty(); }
  int pointCount() const { return static_cast<int>(points.size()); }
};

struct SemanticSegmentationResult {
  int width = 0;
  int height = 0;
  int output_width = 0;
  int output_height = 0;
  std::vector<std::uint8_t> class_id;
  std::vector<std::uint8_t> class_conf;

  void clear() {
    width = 0;
    height = 0;
    output_width = 0;
    output_height = 0;
    class_id.clear();
    class_conf.clear();
  }

  bool empty() const { return class_id.empty(); }
  int pixelCount() const { return static_cast<int>(class_id.size()); }
};

struct InstanceSegment {
  Box box;
  std::vector<Point> outline;
  std::vector<std::uint8_t> mask;

  void clear() {
    box = Box{};
    outline.clear();
    mask.clear();
  }

  bool empty() const { return outline.empty() && mask.empty(); }
};

struct InstanceSegmentationResult {
  int width = 0;
  int height = 0;
  int mask_width = 0;
  int mask_height = 0;
  std::vector<InstanceSegment> instances;

  void clear() {
    width = 0;
    height = 0;
    mask_width = 0;
    mask_height = 0;
    instances.clear();
  }

  bool empty() const { return instances.empty(); }
  int instanceCount() const { return static_cast<int>(instances.size()); }
};

struct LaneSegment {
  Point start;
  Point end;
  float score = 0.0f;
};

struct LaneDetectionResult {
  int width = 0;
  int height = 0;
  int lane_state = 0;
  std::vector<LaneSegment> lanes;

  void clear() {
    width = 0;
    height = 0;
    lane_state = 0;
    lanes.clear();
  }

  bool empty() const { return lanes.empty(); }
  int laneCount() const { return static_cast<int>(lanes.size()); }
};

}  // namespace tdl_app
