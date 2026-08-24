#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "tdl_app/camera.hpp"
#include "tdl_app/hand_gesture_recognizer.hpp"
#include "tdl_app/sys_context.hpp"

namespace {

struct TimingStats {
  int count = 0;
  double total_ms = 0.0;
  double min_ms = std::numeric_limits<double>::max();
  double max_ms = 0.0;

  void add(double milliseconds) {
    ++count;
    total_ms += milliseconds;
    min_ms = std::min(min_ms, milliseconds);
    max_ms = std::max(max_ms, milliseconds);
  }

  double average() const { return count ? total_ms / count : 0.0; }
};

double elapsedMs(const std::chrono::steady_clock::time_point &begin,
                 const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

}  // namespace

int main(int argc, char **argv) {
  const std::string detector_model = argc > 1
      ? argv[1] : "configs/model_specs/yolov8n_det_hand.mud";
  const std::string keypoint_model = argc > 2
      ? argv[2] : "configs/model_specs/keypoint_hand_128.mud";
  const int frame_count = argc > 3 ? std::max(1, std::atoi(argv[3])) : 120;
  if (argc > 4) {
    std::cerr << "usage: " << argv[0]
              << " [HAND_DETECTOR_MODEL] [HAND_KEYPOINT_MODEL] [frame_count]\n";
    return 2;
  }

  std::string error;
  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "CVI_SYS_Init failed: " << error << "\n";
    return 1;
  }

  tdl_app::HandGestureRecognizer recognizer;
  tdl_app::HandGestureRecognizer::Config config;
  config.detector_model_spec = detector_model;
  config.keypoint_model_spec = keypoint_model;
  config.max_hands = 2;
  if (!recognizer.load(config, &error)) {
    std::cerr << "model load failed: " << error << "\n";
    return 1;
  }

  tdl_app::Camera camera(tdl_app::Camera::ai());
  if (!camera.open(&error)) {
    std::cerr << "camera open failed: " << error << "\n";
    return 1;
  }

  TimingStats read_stats;
  TimingStats pipeline_stats;
  for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
    tdl_app::Frame frame;
    const auto read_begin = std::chrono::steady_clock::now();
    if (!camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera.close();
      return 1;
    }
    const auto read_end = std::chrono::steady_clock::now();
    read_stats.add(elapsedMs(read_begin, read_end));

    std::vector<tdl_app::HandGestureResult> results;
    const auto pipeline_begin = std::chrono::steady_clock::now();
    const bool ok = recognizer.recognizeFrame(frame, &results, &error);
    const auto pipeline_end = std::chrono::steady_clock::now();
    camera.releaseFrame();
    pipeline_stats.add(elapsedMs(pipeline_begin, pipeline_end));
    if (!ok) {
      std::cerr << "recognition failed at frame=" << frame_index << ": "
                << error << "\n";
      camera.close();
      return 1;
    }

    std::cout << "frame=" << frame_index << " hands=" << results.size();
    for (const auto &result : results) {
      std::cout << " gesture=" << tdl_app::handGestureName(result.gesture)
                << " score=" << std::fixed << std::setprecision(2)
                << result.score << " box=" << result.box.x1 << ','
                << result.box.y1 << ',' << result.box.x2 << ','
                << result.box.y2 << " points="
                << result.keypoints.pointCount();
    }
    std::cout << "\n";
  }
  camera.close();

  const double average_pipeline = pipeline_stats.average();
  std::cout << std::fixed << std::setprecision(3)
            << "performance: read_count=" << read_stats.count
            << " read_avg_ms=" << read_stats.average()
            << " pipeline_count=" << pipeline_stats.count
            << " pipeline_avg_ms=" << average_pipeline
            << " pipeline_min_ms=" << pipeline_stats.min_ms
            << " pipeline_max_ms=" << pipeline_stats.max_ms
            << " pipeline_fps="
            << (average_pipeline > 0.0 ? 1000.0 / average_pipeline : 0.0)
            << " end_to_end_fps="
            << ((read_stats.average() + average_pipeline) > 0.0
                    ? 1000.0 / (read_stats.average() + average_pipeline)
                    : 0.0)
            << "\n";
  return 0;
}
