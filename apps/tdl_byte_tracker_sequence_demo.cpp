#include <algorithm>
#include <chrono>
#include <cstdio>
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

#include "tdl_app/advanced.hpp"
#include "tdl_app/byte_tracker.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string frames_dir = "./assets/tracker_synthetic/frames_640";
  std::string model_spec = "./configs/model_specs/yolov8n_det_coco80.mud";
  std::string firmware;
  std::string output = "/tmp/jyd_results/bytetrack_sequence_overlay.jpg";
  std::string extension = ".jpg";
  int start = 0;
  int frames = 100;
  int filename_width = 4;
  int class_id = 0;
  int max_missed = 30;
  float threshold = 0.25f;
  float high_score = 0.45f;
  float low_score = 0.15f;
  float iou_threshold = 0.30f;
  float line_x = 1024.0f;
};

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_byte_tracker_sequence_demo [--frames-dir DIR] [--frames 100]\n"
      << "      [--start 0] [--filename-width 4] [--extension .jpg]\n"
      << "      [--model-spec FILE] [--class-id 0] [--line-x 1024]\n"
      << "      [--threshold 0.25] [--high-score 0.45] [--low-score 0.15]\n"
      << "      [--iou-threshold 0.30] [--max-missed 30] [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--frames-dir") {
      const char *v = value("--frames-dir"); if (!v) return false; opt->frames_dir = v;
    } else if (arg == "--model-spec") {
      const char *v = value("--model-spec"); if (!v) return false; opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware"); if (!v) return false; opt->firmware = v;
    } else if (arg == "--output") {
      const char *v = value("--output"); if (!v) return false; opt->output = v;
    } else if (arg == "--extension") {
      const char *v = value("--extension"); if (!v) return false; opt->extension = v;
    } else if (arg == "--start") {
      const char *v = value("--start"); if (!v) return false; opt->start = std::atoi(v);
    } else if (arg == "--frames") {
      const char *v = value("--frames"); if (!v) return false; opt->frames = std::atoi(v);
    } else if (arg == "--filename-width") {
      const char *v = value("--filename-width"); if (!v) return false; opt->filename_width = std::atoi(v);
    } else if (arg == "--class-id") {
      const char *v = value("--class-id"); if (!v) return false; opt->class_id = std::atoi(v);
    } else if (arg == "--max-missed") {
      const char *v = value("--max-missed"); if (!v) return false; opt->max_missed = std::atoi(v);
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
    } else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return !opt->frames_dir.empty() && !opt->model_spec.empty() &&
         !opt->output.empty() && opt->start >= 0 && opt->frames > 0 &&
         opt->filename_width > 0 && opt->high_score >= opt->low_score &&
         opt->max_missed >= 0;
}

std::string framePath(const Options &opt, int frame_number) {
  char name[64]{};
  std::snprintf(name, sizeof(name), "%0*d%s", opt.filename_width,
                frame_number, opt.extension.c_str());
  return opt.frames_dir + "/" + name;
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
  if (image.empty()) {
    if (error) *error = "failed to read final sequence frame: " + input;
    return false;
  }
  cv::line(image, cv::Point(static_cast<int>(line_x), 0),
           cv::Point(static_cast<int>(line_x), image.rows - 1),
           cv::Scalar(0, 255, 255), 3, cv::LINE_AA);
  for (const auto &track : tracks) {
    if (track.missed != 0) continue;
    const cv::Scalar color = colorForId(track.id);
    cv::rectangle(image,
                  cv::Point(static_cast<int>(track.box.x1),
                            static_cast<int>(track.box.y1)),
                  cv::Point(static_cast<int>(track.box.x2),
                            static_cast<int>(track.box.y2)),
                  color, 3, cv::LINE_AA);
    cv::putText(image, "ID=" + std::to_string(track.id),
                cv::Point(static_cast<int>(track.box.x1),
                          std::max(24, static_cast<int>(track.box.y1) - 8)),
                cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2, cv::LINE_AA);
    const auto trail = trails.find(track.id);
    if (trail != trails.end()) {
      for (size_t index = 1; index < trail->second.size(); ++index) {
        cv::line(image, trail->second[index - 1], trail->second[index],
                 color, 3, cv::LINE_AA);
      }
    }
  }
  cv::putText(image, "L->R=" + std::to_string(left_to_right) +
                     "  R->L=" + std::to_string(right_to_left),
              cv::Point(20, 42), cv::FONT_HERSHEY_SIMPLEX, 1.0,
              cv::Scalar(0, 255, 255), 3, cv::LINE_AA);
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write sequence overlay: " + output;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    usage();
    return 1;
  }

  std::string error;
  tdl_app::Detector::Config detector_config;
  detector_config.model_spec = opt.model_spec;
  detector_config.firmware = opt.firmware;
  tdl_app::Detector detector(detector_config, &error);
  if (!detector.initialized()) {
    std::cerr << "detector load failed: " << error << "\n";
    return 2;
  }

  jyd_tracker::ByteTracker::Config tracker_config;
  tracker_config.high_score = opt.high_score;
  tracker_config.low_score = opt.low_score;
  tracker_config.iou_threshold = opt.iou_threshold;
  tracker_config.max_missed = opt.max_missed;
  jyd_tracker::ByteTracker tracker(tracker_config);
  std::map<std::uint64_t, std::deque<cv::Point>> trails;
  std::set<std::uint64_t> counted;
  int left_to_right = 0;
  int right_to_left = 0;
  double detect_sum = 0.0;
  double tracker_sum = 0.0;
  double total_sum = 0.0;
  double detections_sum = 0.0;
  double active_sum = 0.0;
  std::vector<jyd_tracker::Track> last_tracks;
  std::string last_path;
  const tdl_app::InferOptions infer_options =
      tdl_app::InferOptions::detection(opt.threshold);

  for (int index = 0; index < opt.frames; ++index) {
    const auto total_begin = Clock::now();
    const std::string path = framePath(opt, opt.start + index);
    const auto detect_begin = Clock::now();
    error.clear();
    tdl_app::AlgorithmResult result = detector(path, infer_options, &error);
    if (!error.empty()) {
      std::cerr << "detect failed at " << path << ": " << error << "\n";
      return 3;
    }
    const auto detect_end = Clock::now();
    std::vector<jyd_tracker::Detection> detections;
    for (const auto &box : result.boxes) {
      if (opt.class_id >= 0 && box.class_id != opt.class_id) continue;
      detections.push_back(
          {box.x1, box.y1, box.x2, box.y2, box.score, box.class_id});
    }
    const auto tracker_begin = Clock::now();
    std::vector<jyd_tracker::Track> tracks = tracker.update(detections);
    for (const auto &track : tracks) {
      if (track.missed != 0) continue;
      const float center_x = (track.box.x1 + track.box.x2) * 0.5f;
      const float center_y = (track.box.y1 + track.box.y2) * 0.5f;
      auto &trail = trails[track.id];
      trail.emplace_back(static_cast<int>(center_x), static_cast<int>(center_y));
      if (trail.size() > 100) trail.pop_front();
      if (!counted.count(track.id)) {
        if (track.previous_center_x < opt.line_x && center_x >= opt.line_x) {
          ++left_to_right;
          counted.insert(track.id);
        } else if (track.previous_center_x > opt.line_x && center_x <= opt.line_x) {
          ++right_to_left;
          counted.insert(track.id);
        }
      }
    }
    const auto tracker_end = Clock::now();
    detect_sum += std::chrono::duration<double, std::milli>(
        detect_end - detect_begin).count();
    tracker_sum += std::chrono::duration<double, std::milli>(
        tracker_end - tracker_begin).count();
    total_sum += std::chrono::duration<double, std::milli>(
        Clock::now() - total_begin).count();
    detections_sum += detections.size();
    active_sum += std::count_if(
        tracks.begin(), tracks.end(),
        [](const jyd_tracker::Track &track) { return track.missed == 0; });
    last_tracks = std::move(tracks);
    last_path = path;
  }

  if (!drawOverlay(last_path, opt.output, last_tracks, trails, opt.line_x,
                   left_to_right, right_to_left, &error)) {
    std::cerr << "overlay failed: " << error << "\n";
    return 4;
  }
  const double count = static_cast<double>(opt.frames);
  const double avg_total = total_sum / count;
  std::cout << std::fixed << std::setprecision(3)
            << "sequence_frames=" << opt.frames
            << " left_to_right=" << left_to_right
            << " right_to_left=" << right_to_left << "\n"
            << "avg_detect_ms=" << detect_sum / count
            << " avg_tracker_ms=" << tracker_sum / count
            << " avg_total_ms=" << avg_total
            << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0)
            << " avg_detections=" << detections_sum / count
            << " avg_active_tracks=" << active_sum / count << "\n"
            << "saved_overlay=" << opt.output << "\n";
  return 0;
}
