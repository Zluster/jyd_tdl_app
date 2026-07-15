#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"
#include "tdl_app/byte_tracker.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec = "./configs/model_specs/yolov8n_det_coco80.mud";
  std::string firmware;
  std::string dump_frame;
  std::string dump_overlay;
  float threshold = 0.25f;
  float high_score = 0.45f;
  float low_score = 0.15f;
  float iou_threshold = 0.30f;
  float line_x = 320.0f;
  int class_id = 0;
  int max_missed = 30;
  int warmup = 30;
};

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_byte_tracker_camera_demo [--model-spec FILE] [--class-id 0]\n"
      << "      [--threshold 0.25] [--high-score 0.45] [--low-score 0.15]\n"
      << "      [--iou-threshold 0.30] [--max-missed 30] [--line-x 320]\n"
      << "      [--group 0] [--channel 1] [--warmup 30] [--frames 300]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &parse_error)) {
      std::cerr << parse_error << "\n"; return false;
    }
    if (handled) continue;
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) { std::cerr << name << " requires a value\n"; return nullptr; }
      return argv[++i];
    };
    if (arg == "--camera") {
      continue;
    } else if (arg == "--model-spec") {
      const char *v = value("--model-spec"); if (!v) return false; opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware"); if (!v) return false; opt->firmware = v;
    } else if (arg == "--threshold") {
      const char *v = value("--threshold"); if (!v) return false; opt->threshold = std::atof(v);
    } else if (arg == "--high-score") {
      const char *v = value("--high-score"); if (!v) return false; opt->high_score = std::atof(v);
    } else if (arg == "--low-score") {
      const char *v = value("--low-score"); if (!v) return false; opt->low_score = std::atof(v);
    } else if (arg == "--iou-threshold") {
      const char *v = value("--iou-threshold"); if (!v) return false; opt->iou_threshold = std::atof(v);
    } else if (arg == "--line-x") {
      const char *v = value("--line-x"); if (!v) return false; opt->line_x = std::atof(v);
    } else if (arg == "--class-id") {
      const char *v = value("--class-id"); if (!v) return false; opt->class_id = std::atoi(v);
    } else if (arg == "--max-missed") {
      const char *v = value("--max-missed"); if (!v) return false; opt->max_missed = std::atoi(v);
    } else if (arg == "--warmup") {
      const char *v = value("--warmup"); if (!v) return false; opt->warmup = std::atoi(v);
    } else if (arg == "--dump-frame") {
      const char *v = value("--dump-frame"); if (!v) return false; opt->dump_frame = v;
    } else if (arg == "--dump-overlay") {
      const char *v = value("--dump-overlay"); if (!v) return false; opt->dump_overlay = v;
    } else if (arg == "-h" || arg == "--help") {
      usage(); std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n"; return false;
    }
  }
  return !opt->model_spec.empty() && opt->camera.frames > 0 && opt->warmup >= 0 &&
         opt->max_missed >= 0 && opt->high_score >= opt->low_score &&
         (opt->dump_overlay.empty() || !opt->dump_frame.empty());
}

cv::Scalar colorForId(std::uint64_t id) {
  return cv::Scalar((37 * id) % 200 + 40, (97 * id) % 200 + 40,
                    (151 * id) % 200 + 40);
}

bool drawOverlay(const std::string &input, const std::string &output,
                 const std::vector<jyd_tracker::Track> &tracks,
                 const std::map<std::uint64_t, std::deque<cv::Point>> &trails,
                 float line_x, int left_to_right, int right_to_left,
                 std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) { if (error) *error = "failed to read tracker snapshot"; return false; }
  cv::line(image, cv::Point(static_cast<int>(line_x), 0),
           cv::Point(static_cast<int>(line_x), image.rows - 1),
           cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
  for (const auto &track : tracks) {
    if (track.missed != 0) continue;
    const cv::Scalar color = colorForId(track.id);
    cv::rectangle(image, cv::Point(static_cast<int>(track.box.x1), static_cast<int>(track.box.y1)),
                  cv::Point(static_cast<int>(track.box.x2), static_cast<int>(track.box.y2)),
                  color, 2, cv::LINE_AA);
    cv::putText(image, "ID=" + std::to_string(track.id),
                cv::Point(static_cast<int>(track.box.x1),
                          std::max(18, static_cast<int>(track.box.y1) - 5)),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv::LINE_AA);
    const auto trail = trails.find(track.id);
    if (trail != trails.end()) {
      for (size_t i = 1; i < trail->second.size(); ++i) {
        cv::line(image, trail->second[i - 1], trail->second[i], color, 2, cv::LINE_AA);
      }
    }
  }
  cv::putText(image, "L->R=" + std::to_string(left_to_right) +
                     " R->L=" + std::to_string(right_to_left),
              cv::Point(12, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7,
              cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
  if (!cv::imwrite(output, image)) { if (error) *error = "failed to write tracker overlay"; return false; }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) { usage(); return 1; }
  std::string error;
  tdl_app::Detector::Config detector_config;
  detector_config.model_spec = opt.model_spec;
  detector_config.firmware = opt.firmware;
  tdl_app::Detector detector(detector_config, &error);
  if (!detector.initialized()) { std::cerr << "detector load failed: " << error << "\n"; return 2; }
  jyd_tracker::ByteTracker::Config tracker_config;
  tracker_config.high_score = opt.high_score;
  tracker_config.low_score = opt.low_score;
  tracker_config.iou_threshold = opt.iou_threshold;
  tracker_config.max_missed = opt.max_missed;
  jyd_tracker::ByteTracker tracker(tracker_config);
  camera_demo_support::CameraRuntime camera;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &camera, &error)) {
    std::cerr << "camera open failed: " << error << "\n"; return 3;
  }
  std::map<std::uint64_t, std::deque<cv::Point>> trails;
  std::set<std::uint64_t> counted;
  int left_to_right = 0, right_to_left = 0;
  double read_sum = 0.0, detect_sum = 0.0, track_sum = 0.0, total_sum = 0.0;
  double detection_sum = 0.0, active_sum = 0.0;
  std::vector<jyd_tracker::Track> last_tracks;
  const int total_frames = opt.warmup + opt.camera.frames;
  tdl_app::InferOptions infer_options = tdl_app::InferOptions::detection(opt.threshold);
  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto total_begin = Clock::now();
    const auto read_begin = total_begin;
    if (!camera.camera.read(&frame, &error)) {
      camera_demo_support::closeCameraRuntime(&camera);
      std::cerr << "camera read failed: " << error << "\n"; return 4;
    }
    const auto read_end = Clock::now();
    const auto detect_begin = read_end;
    tdl_app::AlgorithmResult detections;
    if (!detector.runFrame(frame, infer_options, &detections, &error)) {
      camera.camera.releaseFrame(); camera_demo_support::closeCameraRuntime(&camera);
      std::cerr << "detect failed: " << error << "\n"; return 5;
    }
    const auto detect_end = Clock::now();
    std::vector<jyd_tracker::Detection> tracker_detections;
    for (const auto &box : detections.boxes) {
      if (opt.class_id >= 0 && box.class_id != opt.class_id) continue;
      tracker_detections.push_back({box.x1, box.y1, box.x2, box.y2,
                                    box.score, box.class_id});
    }
    const auto track_begin = Clock::now();
    std::vector<jyd_tracker::Track> tracks = tracker.update(tracker_detections);
    for (const auto &track : tracks) {
      if (track.missed != 0) continue;
      const float center_x = (track.box.x1 + track.box.x2) * 0.5f;
      const float center_y = (track.box.y1 + track.box.y2) * 0.5f;
      auto &trail = trails[track.id];
      trail.emplace_back(static_cast<int>(center_x), static_cast<int>(center_y));
      if (trail.size() > 64) trail.pop_front();
      if (!counted.count(track.id)) {
        if (track.previous_center_x < opt.line_x && center_x >= opt.line_x) {
          ++left_to_right; counted.insert(track.id);
        } else if (track.previous_center_x > opt.line_x && center_x <= opt.line_x) {
          ++right_to_left; counted.insert(track.id);
        }
      }
    }
    const auto track_end = Clock::now();
    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !drawOverlay(opt.dump_frame, opt.dump_overlay, tracks, trails,
                        opt.line_x, left_to_right, right_to_left, &error))) {
        camera.camera.releaseFrame(); camera_demo_support::closeCameraRuntime(&camera);
        std::cerr << "save failed: " << error << "\n"; return 6;
      }
    }
    camera.camera.releaseFrame();
    if (index < opt.warmup) continue;
    read_sum += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
    detect_sum += std::chrono::duration<double, std::milli>(detect_end - detect_begin).count();
    track_sum += std::chrono::duration<double, std::milli>(track_end - track_begin).count();
    total_sum += std::chrono::duration<double, std::milli>(Clock::now() - total_begin).count();
    detection_sum += tracker_detections.size();
    active_sum += std::count_if(tracks.begin(), tracks.end(),
                                [](const jyd_tracker::Track &track) { return track.missed == 0; });
    last_tracks = std::move(tracks);
  }
  camera_demo_support::closeCameraRuntime(&camera);
  const double count = static_cast<double>(opt.camera.frames);
  const double avg_total = total_sum / count;
  std::cout << std::fixed << std::setprecision(3)
            << "frames=" << opt.camera.frames << " class_id=" << opt.class_id
            << " left_to_right=" << left_to_right << " right_to_left=" << right_to_left << "\n"
            << "avg_read_ms=" << read_sum / count
            << " avg_detect_ms=" << detect_sum / count
            << " avg_tracker_ms=" << track_sum / count
            << " avg_total_ms=" << avg_total
            << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0) << "\n"
            << "avg_detections=" << detection_sum / count
            << " avg_active_tracks=" << active_sum / count << "\n";
  for (const auto &track : last_tracks) {
    if (track.missed == 0) std::cout << "track id=" << track.id << " score=" << track.box.score << "\n";
  }
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  return 0;
}
