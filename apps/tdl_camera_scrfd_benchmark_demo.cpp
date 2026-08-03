#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/nn_scrfd.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
  float threshold = 0.25f;
  float iou_threshold = 0.45f;
  int warmup = 5;
  bool dump_boxes = false;
  std::string dump_frame;
  std::string dump_overlay;
};

double elapsedMs(const SteadyClock::time_point &begin,
                 const SteadyClock::time_point &end) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                 .count()) /
         1000.0;
}

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_scrfd_benchmark_demo --model-spec FILE\n"
      << "                                  [--firmware FILE]\n"
      << "                                  [--model-dir DIR]\n"
      << "                                  [--threshold 0.25]\n"
      << "                                  [--iou-threshold 0.45]\n"
      << "                                  [--warmup N]\n"
      << "                                  [--dump-boxes]\n"
      << "                                  [--dump-frame FILE] [--dump-overlay FILE]\n"
      << "                                  [--backend vi|vpss]\n"
      << "                                  [default: ai channel 640x640]\n"
      << "                                  [--use-mmf] [--attach-existing]\n"
      << "                                  [--attach-existing]\n"
      << "                                  [--sensor-ini FILE] [--frames N]\n"
      << "                                  [--device N] [--group N] [--pipe N]\n"
      << "                                  [--channel N] [--width N] [--height N]\n"
      << "                                  [--pixel-format N] [--timeout-ms N]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }

    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--model-spec") {
      const char *v = value("--model-spec");
      if (!v) return false;
      opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware");
      if (!v) return false;
      opt->firmware = v;
    } else if (arg == "--model-dir") {
      const char *v = value("--model-dir");
      if (!v) return false;
      opt->model_dir = v;
    } else if (arg == "--threshold") {
      const char *v = value("--threshold");
      if (!v) return false;
      opt->threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--iou-threshold") {
      const char *v = value("--iou-threshold");
      if (!v) return false;
      opt->iou_threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--warmup") {
      const char *v = value("--warmup");
      if (!v) return false;
      opt->warmup = std::atoi(v);
    } else if (arg == "--dump-boxes") {
      opt->dump_boxes = true;
    } else if (arg == "--dump-frame") {
      const char *v = value("--dump-frame");
      if (!v) return false;
      opt->dump_frame = v;
    } else if (arg == "--dump-overlay") {
      const char *v = value("--dump-overlay");
      if (!v) return false;
      opt->dump_overlay = v;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return !opt->model_spec.empty();
}

void printBoxes(const tdl_app::AlgorithmResult &result) {
  for (size_t i = 0; i < result.boxes.size(); ++i) {
    const auto &box = result.boxes[i];
    std::cout << "    box[" << i << "] score=" << std::fixed
              << std::setprecision(3) << box.score << " rect=(" << box.x1
              << "," << box.y1 << "," << box.x2 << "," << box.y2 << ")\n";
  }
}

bool writeOverlay(const std::string &source_path, const std::string &overlay_path,
                  const tdl_app::AlgorithmResult &result, std::string *error) {
  cv::Mat image = cv::imread(source_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read snapshot for overlay: " + source_path;
    return false;
  }

  for (const auto &box : result.boxes) {
    const int x1 = std::max(0, std::min(static_cast<int>(std::floor(box.x1)),
                                        image.cols - 1));
    const int y1 = std::max(0, std::min(static_cast<int>(std::floor(box.y1)),
                                        image.rows - 1));
    const int x2 = std::max(0, std::min(static_cast<int>(std::ceil(box.x2)),
                                        image.cols - 1));
    const int y2 = std::max(0, std::min(static_cast<int>(std::ceil(box.y2)),
                                        image.rows - 1));
    cv::rectangle(image, cv::Point(x1, y1), cv::Point(x2, y2),
                  cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    cv::putText(image, cv::format("face %.3f", box.score),
                cv::Point(x1, std::max(16, y1 - 5)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 255, 0), 1,
                cv::LINE_AA);
    for (const auto &point : box.landmarks) {
      cv::circle(image,
                 cv::Point(static_cast<int>(std::lround(point.x)),
                           static_cast<int>(std::lround(point.y))),
                 2, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_AA);
    }
  }

  if (!cv::imwrite(overlay_path, image)) {
    if (error) *error = "failed to write face overlay: " + overlay_path;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }
  if (!opt.dump_overlay.empty() && opt.dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return 1;
  }

  camera_demo_support::CameraRuntime runtime;
  std::string error;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  const tdl_app::Camera::Config &camera_config = runtime.camera.config();
  std::cerr << "camera config: backend="
            << camera_demo_support::backendName(camera_config.backend)
            << " device=" << camera_config.device
            << " pipe=" << camera_config.pipe
            << " group=" << camera_config.group
            << " channel=" << camera_config.channel
            << " width=" << camera_config.width
            << " height=" << camera_config.height
            << " pixel_format=" << camera_config.pixel_format
            << " timeout_ms=" << camera_config.timeout_ms << "\n";

  tdl_app::EngineConfig engine_config;
  engine_config.model_descriptor_file = opt.model_spec;
  engine_config.model_dir = opt.model_dir;
  engine_config.bmrt_firmware = opt.firmware;

  tdl_app::NnScrfd model("SCRFD");
  if (!model.load(engine_config, &error)) {
    std::cerr << "scrfd load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }

  tdl_app::InferOptions infer_options =
      tdl_app::InferOptions::detection(opt.threshold, opt.iou_threshold);

  const int measured_frames = opt.camera.frames <= 0 ? 1 : opt.camera.frames;
  const int total_frames = opt.warmup + measured_frames;

  double read_sum_ms = 0.0;
  double source_sum_ms = 0.0;
  double preprocess_sum_ms = 0.0;
  double launch_sum_ms = 0.0;
  double decode_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double box_sum = 0.0;
  tdl_app::AlgorithmResult last_result;
  int last_width = 0;
  int last_height = 0;
  int last_format = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t last_pts = 0;
  std::string saved_frame;
  std::string saved_overlay;

  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = SteadyClock::now();
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 4;
    }
    const auto read_end = SteadyClock::now();
    const double read_ms = elapsedMs(read_begin, read_end);

    tdl_app::AlgorithmResult result;
    const auto infer_begin = SteadyClock::now();
    if (!model.predictFrame(frame, infer_options, &result, &error)) {
      std::cerr << "scrfd infer failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }
    const auto infer_end = SteadyClock::now();
    const double infer_ms = elapsedMs(infer_begin, infer_end);
    const tdl_app::NnScrfd::Profile &profile = model.lastProfile();
    const double total_ms = read_ms + infer_ms;

    const bool warmup = index < opt.warmup;
    if (!warmup) {
      read_sum_ms += read_ms;
      source_sum_ms += profile.source_prepare_ms;
      preprocess_sum_ms += profile.preprocess_ms;
      launch_sum_ms += profile.launch_ms;
      decode_sum_ms += profile.decode_ms;
      infer_sum_ms += infer_ms;
      total_sum_ms += total_ms;
      box_sum += static_cast<double>(result.boxes.size());
      last_result = result;
      last_width = frame.width;
      last_height = frame.height;
      last_format = frame.format;
      last_sequence = frame.sequence;
      last_pts = frame.timestamp_us;
      if (index == total_frames - 1 && !opt.dump_frame.empty()) {
        if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame,
                                                   &error)) {
          std::cerr << "failed to save frame: " << error << "\n";
          camera_demo_support::closeCameraRuntime(&runtime);
          return 6;
        }
        saved_frame = opt.dump_frame;
        if (!opt.dump_overlay.empty() &&
            !writeOverlay(saved_frame, opt.dump_overlay, result, &error)) {
          std::cerr << "failed to save overlay: " << error << "\n";
          camera_demo_support::closeCameraRuntime(&runtime);
          return 7;
        }
        saved_overlay = opt.dump_overlay;
      }
    }
  }

  const double denom =
      measured_frames > 0 ? static_cast<double>(measured_frames) : 1.0;
  std::cout << std::fixed << std::setprecision(3)
            << "summary: frames=" << measured_frames
            << " avg_read=" << (read_sum_ms / denom)
            << " ms, avg_source_prepare=" << (source_sum_ms / denom)
            << " ms, avg_preprocess=" << (preprocess_sum_ms / denom)
            << " ms, avg_launch=" << (launch_sum_ms / denom)
            << " ms, avg_decode=" << (decode_sum_ms / denom)
            << " ms, avg_infer=" << (infer_sum_ms / denom)
            << " ms, avg_total=" << (total_sum_ms / denom)
            << " ms, avg_boxes=" << (box_sum / denom)
            << ", avg_fps="
            << ((total_sum_ms / denom) > 0.0 ? 1000.0 / (total_sum_ms / denom)
                                             : 0.0)
            << "\n";
  std::cout << "last_frame: seq=" << last_sequence
            << " pts=" << last_pts
            << " src=" << last_width << "x" << last_height
            << " fmt=" << last_format
            << " boxes=" << last_result.boxes.size() << "\n";
  if (!saved_frame.empty()) {
    std::cout << "saved_frame=" << saved_frame << "\n";
  }
  if (!saved_overlay.empty()) {
    std::cout << "saved_overlay=" << saved_overlay << "\n";
  }
  if (opt.dump_boxes && !last_result.boxes.empty()) {
    printBoxes(last_result);
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
