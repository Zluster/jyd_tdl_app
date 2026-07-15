#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"
#include "tdl_app/single_object_tracker.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera_options;
  bool camera = false;
  std::string template_image;
  std::string search_image;
  std::string model_spec = "./configs/model_specs/feartrack.mud";
  std::string firmware;
  std::string model_dir;
  std::string output;
  std::string dump_frame;
  std::string dump_overlay;
  tdl_app::Box init_box;
  bool has_init_box = false;
  int warmup = 5;
};

double elapsedMs(const Clock::time_point &begin, const Clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_single_object_tracker_demo --camera --init-box x1,y1,x2,y2\n"
      << "      [--model-spec FILE] [--warmup 5] [--frames 300]\n"
      << "      [--group 0] [--channel 1]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n"
      << "  tdl_single_object_tracker_demo --template-image FILE --search-image FILE\n"
      << "      --init-box x1,y1,x2,y2 [--model-spec FILE] [--output FILE]\n";
}

bool parseBox(const std::string &value, tdl_app::Box *box) {
  return box && std::sscanf(value.c_str(), "%f,%f,%f,%f", &box->x1, &box->y1,
                            &box->x2, &box->y2) == 4 && box->valid();
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string error;
    if (!camera_demo_support::parseCommonArgs(
            argc, argv, &i, &opt->camera_options, &handled, &error)) {
      std::cerr << error << "\n"; return false;
    }
    if (handled) { opt->camera = true; continue; }
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n"; return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--camera") {
      opt->camera = true;
    } else if (arg == "--template-image") {
      const char *v = value("--template-image"); if (!v) return false;
      opt->template_image = v;
    } else if (arg == "--search-image") {
      const char *v = value("--search-image"); if (!v) return false;
      opt->search_image = v;
    } else if (arg == "--model-spec") {
      const char *v = value("--model-spec"); if (!v) return false; opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware"); if (!v) return false; opt->firmware = v;
    } else if (arg == "--model-dir") {
      const char *v = value("--model-dir"); if (!v) return false; opt->model_dir = v;
    } else if (arg == "--output") {
      const char *v = value("--output"); if (!v) return false; opt->output = v;
    } else if (arg == "--dump-frame") {
      const char *v = value("--dump-frame"); if (!v) return false; opt->dump_frame = v;
    } else if (arg == "--dump-overlay") {
      const char *v = value("--dump-overlay"); if (!v) return false; opt->dump_overlay = v;
    } else if (arg == "--init-box") {
      const char *v = value("--init-box"); if (!v) return false;
      if (!parseBox(v, &opt->init_box)) {
        std::cerr << "invalid --init-box, expected x1,y1,x2,y2\n"; return false;
      }
      opt->has_init_box = true;
    } else if (arg == "--warmup") {
      const char *v = value("--warmup"); if (!v) return false; opt->warmup = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      usage(); std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n"; return false;
    }
  }
  if (!opt->has_init_box || opt->model_spec.empty()) return false;
  if (opt->camera) {
    if (opt->camera_options.frames <= 0 || opt->warmup < 0) return false;
    if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) return false;
    return true;
  }
  return !opt->template_image.empty() && !opt->search_image.empty();
}

cv::Point boxCenter(const tdl_app::Box &box) {
  return cv::Point(static_cast<int>(std::lround((box.x1 + box.x2) * 0.5f)),
                   static_cast<int>(std::lround((box.y1 + box.y2) * 0.5f)));
}

bool drawOverlay(const std::string &input, const std::string &output,
                 const tdl_app::Box &initial_box,
                 const tdl_app::SingleObjectTrackingResult &result,
                 const std::vector<cv::Point> &trajectory,
                 std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read tracker image: " + input;
    return false;
  }
  const auto clippedPoint = [&](float x, float y) {
    return cv::Point(std::max(0, std::min(image.cols - 1,
                                          static_cast<int>(std::lround(x)))),
                     std::max(0, std::min(image.rows - 1,
                                          static_cast<int>(std::lround(y)))));
  };
  const cv::Point initial_first = clippedPoint(initial_box.x1, initial_box.y1);
  const cv::Point initial_second = clippedPoint(initial_box.x2, initial_box.y2);
  cv::rectangle(image, initial_first, initial_second, cv::Scalar(0, 255, 255),
                2, cv::LINE_AA);
  cv::putText(image, "INIT", cv::Point(initial_first.x,
                                         std::max(18, initial_first.y - 5)),
              cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1,
              cv::LINE_AA);
  if (trajectory.size() >= 2) {
    cv::polylines(image, trajectory, false, cv::Scalar(255, 255, 0), 1,
                  cv::LINE_AA);
  }
  const cv::Point first = clippedPoint(result.box.x1, result.box.y1);
  const cv::Point second = clippedPoint(result.box.x2, result.box.y2);
  const cv::Scalar color = result.tracked ? cv::Scalar(0, 255, 0)
                                           : cv::Scalar(0, 0, 255);
  cv::rectangle(image, first, second, color, 2, cv::LINE_AA);
  const std::string status = result.tracked ? "TRACKED" : "LOST";
  cv::putText(image,
              cv::format("FearTrack %s %.3f grid=%d,%d", status.c_str(),
                         result.confidence, result.response_x,
                         result.response_y),
              cv::Point(first.x, std::max(18, first.y - 5)),
              cv::FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv::LINE_AA);
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write tracker overlay: " + output;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) { usage(); return 1; }
  tdl_app::SingleObjectTracker tracker = tdl_app::SingleObjectTracker::fearTrack();
  std::string error;
  if (!tracker.load(tdl_app::ModelSessionConfig::fromSpec(
                        opt.model_spec, opt.firmware, opt.model_dir), &error)) {
    std::cerr << "load failed: " << error << "\n"; return 2;
  }

  if (!opt.camera) {
    if (!tracker.initialize(opt.template_image, opt.init_box, &error)) {
      std::cerr << "initialize failed: " << error << "\n"; return 3;
    }
    tdl_app::SingleObjectTrackingResult result;
    if (!tracker.run(opt.search_image, &result, &error)) {
      std::cerr << "run failed: " << error << "\n"; return 4;
    }
    if (!opt.output.empty() &&
        !drawOverlay(opt.search_image, opt.output, opt.init_box, result, {},
                     &error)) {
      std::cerr << "overlay failed: " << error << "\n"; return 5;
    }
    std::cout << "score=" << result.confidence << " box=(" << result.box.x1
              << ',' << result.box.y1 << ',' << result.box.x2 << ',' << result.box.y2
              << ") preprocess_ms=" << result.preprocess_ms
              << " inference_ms=" << result.inference_ms
              << " postprocess_ms=" << result.postprocess_ms
              << " total_ms=" << result.total_ms << "\n";
    if (!opt.output.empty()) std::cout << "saved_overlay=" << opt.output << "\n";
    return 0;
  }

  camera_demo_support::CameraRuntime camera;
  if (!camera_demo_support::openCameraRuntime(opt.camera_options, &camera, &error)) {
    std::cerr << "camera open failed: " << error << "\n"; return 3;
  }
  tdl_app::Frame initial_frame;
  const auto init_begin = Clock::now();
  if (!camera.camera.read(&initial_frame, &error) ||
      !tracker.initializeFrame(initial_frame, opt.init_box, &error)) {
    std::cerr << "tracker initialize failed: " << error << "\n";
    camera.camera.releaseFrame();
    camera_demo_support::closeCameraRuntime(&camera); return 4;
  }
  camera.camera.releaseFrame();
  const double initialize_ms = elapsedMs(init_begin, Clock::now());
  const tdl_app::Box initial_box = tracker.currentBox();

  double read_sum = 0.0, preprocess_sum = 0.0, inference_sum = 0.0;
  double copy_sum = 0.0, postprocess_sum = 0.0, algorithm_sum = 0.0;
  double total_sum = 0.0, score_sum = 0.0, area_sum = 0.0;
  int tracked_frames = 0, lost_frames = 0;
  tdl_app::SingleObjectTrackingResult last;
  std::vector<cv::Point> trajectory;
  const int total_frames = opt.warmup + opt.camera_options.frames;
  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = Clock::now();
    if (!camera.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&camera); return 5;
    }
    const auto read_end = Clock::now();
    tdl_app::SingleObjectTrackingResult result;
    if (!tracker.runFrame(frame, &result, &error)) {
      std::cerr << "track failed: " << error << "\n";
      camera.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&camera); return 6;
    }
    trajectory.push_back(boxCenter(result.box));
    last = result;
    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !drawOverlay(opt.dump_frame, opt.dump_overlay, initial_box, result,
                        trajectory, &error))) {
        std::cerr << "effect image failed: " << error << "\n";
        camera.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&camera); return 7;
      }
    }
    camera.camera.releaseFrame();
    if (index < opt.warmup) continue;
    const double read_ms = elapsedMs(read_begin, read_end);
    read_sum += read_ms;
    preprocess_sum += result.preprocess_ms;
    inference_sum += result.inference_ms;
    copy_sum += result.output_copy_ms;
    postprocess_sum += result.postprocess_ms;
    algorithm_sum += result.total_ms;
    total_sum += read_ms + result.total_ms;
    score_sum += result.confidence;
    area_sum += std::max(0.0f, result.box.width()) *
                std::max(0.0f, result.box.height());
    if (result.tracked) {
      ++tracked_frames;
    } else {
      ++lost_frames;
    }
  }
  camera_demo_support::closeCameraRuntime(&camera);
  const double count = static_cast<double>(opt.camera_options.frames);
  const double avg_total = total_sum / count;
  const cv::Point initial_center = boxCenter(initial_box);
  const cv::Point final_center = boxCenter(last.box);
  const double center_shift = std::hypot(
      static_cast<double>(final_center.x - initial_center.x),
      static_cast<double>(final_center.y - initial_center.y));
  std::cout << std::fixed << std::setprecision(3)
            << "frames=" << opt.camera_options.frames
            << " initialize_ms=" << initialize_ms
            << " status=" << (last.tracked ? "TRACKED" : "LOST")
            << " score=" << last.confidence << " box=(" << last.box.x1 << ','
            << last.box.y1 << ',' << last.box.x2 << ',' << last.box.y2 << ")\n"
            << "tracked_frames=" << tracked_frames
            << " lost_frames=" << lost_frames
            << " avg_score=" << score_sum / count
            << " avg_box_area=" << area_sum / count
            << " center_shift_px=" << center_shift << "\n"
            << "avg_read_ms=" << read_sum / count
            << " avg_vpss_roi_ms=" << preprocess_sum / count
            << " avg_bmrt_ms=" << inference_sum / count
            << " avg_output_copy_ms=" << copy_sum / count
            << " avg_postprocess_ms=" << postprocess_sum / count
            << " avg_algorithm_ms=" << algorithm_sum / count
            << " avg_total_ms=" << avg_total
            << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0) << "\n";
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  return 0;
}
