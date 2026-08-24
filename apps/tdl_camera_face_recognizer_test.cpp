#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct TimingStats {
  int count = 0;
  double sum_ms = 0.0;
  double min_ms = std::numeric_limits<double>::max();
  double max_ms = 0.0;

  void add(double ms) {
    ++count;
    sum_ms += ms;
    min_ms = std::min(min_ms, ms);
    max_ms = std::max(max_ms, ms);
  }

  double average() const { return count > 0 ? sum_ms / count : 0.0; }
};

double elapsedMs(const std::chrono::steady_clock::time_point &begin,
                 const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

}  // namespace

#include "tdl_app/camera.hpp"
#include "tdl_app/face_recognizer.hpp"
#include "tdl_app/sys_context.hpp"

int main(int argc, char **argv) {
  if (argc < 4 || argc > 5) {
    std::cerr << "usage: " << argv[0]
              << " SCRFD_MODEL FEATURE_MODEL NAME [frame_count]\n";
    return 2;
  }

  const int frame_count = argc == 5 ? std::max(1, std::atoi(argv[4])) : 120;
  std::string error;
  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "CVI_SYS_Init failed: " << error << "\n";
    return 1;
  }

  tdl_app::FaceRecognizer recognizer;
  tdl_app::FaceRecognizer::Config config;
  config.detector_model_spec = argv[1];
  config.feature_model_spec = argv[2];
  // This benchmark targets one enrolled subject. Avoid running the feature
  // model on a second low-confidence SCRFD candidate every frame.
  config.max_faces = 1;
  if (!recognizer.load(config, &error)) {
    std::cerr << "model load failed: " << error << "\n";
    return 1;
  }

  tdl_app::Camera camera(tdl_app::Camera::ai());
  if (!camera.open(&error)) {
    std::cerr << "camera open failed: " << error << "\n";
    return 1;
  }

  bool enrolled = false;
  int pending_errors = 0;
  TimingStats read_stats;
  TimingStats pipeline_stats;
  int recognize_ok = 0;
  int recognize_failed = 0;
  for (int i = 0; i < frame_count; ++i) {
    tdl_app::Frame frame;
    const auto read_begin = std::chrono::steady_clock::now();
    if (!camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      return 1;
    }
    const auto read_end = std::chrono::steady_clock::now();
    read_stats.add(elapsedMs(read_begin, read_end));

    bool ok = true;
    const auto pipeline_begin = std::chrono::steady_clock::now();
    if (!enrolled) {
      ok = recognizer.enrollFrame(argv[3], frame, &error);
      if (ok) {
        enrolled = true;
        std::cout << "enrolled " << argv[3] << "\n";
      } else {
        ++pending_errors;
        if ((i % 10) == 0) {
          std::cerr << "enroll pending frame=" << i << ": "
                    << (error.empty() ? "unspecified failure" : error)
                    << "\n";
        }
      }
    } else {
      std::vector<tdl_app::FaceRecognitionResult> results;
      ok = recognizer.recognizeFrame(frame, &results, &error);
      if (ok) {
        ++recognize_ok;
        for (const auto &result : results) {
          std::cout << "name=" << result.name
                    << " score=" << result.similarity
                    << " matched=" << result.matched
                    << " box=" << result.box.x1 << "," << result.box.y1
                    << "," << result.box.x2 << "," << result.box.y2 << "\n";
        }
      } else {
        ++recognize_failed;
      }
    }
    const auto pipeline_end = std::chrono::steady_clock::now();
    pipeline_stats.add(elapsedMs(pipeline_begin, pipeline_end));
    camera.releaseFrame();

    if (!ok && enrolled) {
      std::cerr << "recognition failed: " << error << "\n";
      return 1;
    }
  }

  if (!enrolled) {
    std::cerr << "no face enrolled within " << frame_count
              << " frames; pending_failures=" << pending_errors << "\n";
    return 1;
  }

  std::cout << std::fixed << std::setprecision(3)
            << "performance: read_count=" << read_stats.count
            << " read_avg_ms=" << read_stats.average()
            << " read_min_ms=" << read_stats.min_ms
            << " read_max_ms=" << read_stats.max_ms
            << " pipeline_count=" << pipeline_stats.count
            << " pipeline_avg_ms=" << pipeline_stats.average()
            << " pipeline_min_ms=" << pipeline_stats.min_ms
            << " pipeline_max_ms=" << pipeline_stats.max_ms
            << " pipeline_fps="
            << (pipeline_stats.average() > 0.0
                    ? 1000.0 / pipeline_stats.average()
                    : 0.0)
            << " end_to_end_fps="
            << ((read_stats.average() + pipeline_stats.average()) > 0.0
                    ? 1000.0 /
                          (read_stats.average() + pipeline_stats.average())
                    : 0.0)
            << " recognize_ok=" << recognize_ok
            << " recognize_failed=" << recognize_failed << "\n";
  return 0;
}
