#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec = "./configs/model_specs/yolov8n_det_coco80.mud";
  std::string firmware;
  float threshold = 0.25f;
  int warmup = 5;
  int repeat = 1;
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
      << "  tdl_camera_yolov8_benchmark_demo [--model-spec FILE]\n"
      << "                                   [--firmware FILE]\n"
      << "                                   [--threshold 0.25]\n"
      << "                                   [--warmup N]\n"
      << "                                   [--repeat N]\n"
      << "                                   [--dump-boxes]\n"
      << "                                   [--dump-frame FILE] [--dump-overlay FILE]\n"
      << "                                   [--backend vi|vpss]\n"
      << "                                   [default: ai channel 640x640]\n"
      << "                                   [--use-mmf] [--attach-existing]\n"
      << "                                   [--attach-existing]\n"
      << "                                   [--sensor-ini FILE] [--frames N]\n"
      << "                                   [--device N] [--group N] [--pipe N]\n"
      << "                                   [--channel N] [--width N] [--height N]\n"
      << "                                   [--pixel-format N] [--timeout-ms N]\n";
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
    } else if (arg == "--threshold") {
      const char *v = value("--threshold");
      if (!v) return false;
      opt->threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--warmup") {
      const char *v = value("--warmup");
      if (!v) return false;
      opt->warmup = std::atoi(v);
    } else if (arg == "--repeat") {
      const char *v = value("--repeat");
      if (!v) return false;
      opt->repeat = std::atoi(v);
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
  return true;
}

void printBoxes(const tdl_app::AlgorithmResult &result) {
  for (size_t i = 0; i < result.boxes.size(); ++i) {
    const auto &box = result.boxes[i];
    const char *label =
        (box.class_id >= 0 && static_cast<size_t>(box.class_id) < result.labels.size())
            ? result.labels[static_cast<size_t>(box.class_id)].c_str()
            : "unknown";
    std::cout << "  box[" << i << "] class=" << box.class_id
              << " label=" << label << " score=" << std::fixed
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
                  cv::Scalar(0, 255, 0), 2);
    std::string text = "unknown";
    if (box.class_id >= 0 &&
        static_cast<size_t>(box.class_id) < result.labels.size()) {
      text = result.labels[static_cast<size_t>(box.class_id)];
    }
    text += " " + std::to_string(box.score);
    cv::putText(image, text, cv::Point(x1, std::max(16, y1 - 4)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 255, 0), 1);
  }
  if (!cv::imwrite(overlay_path, image)) {
    if (error) *error = "failed to write detection overlay: " + overlay_path;
    return false;
  }
  return true;
}

struct PassStats {
  double read_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double box_sum = 0.0;
  int frames = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t last_pts = 0;
  int last_width = 0;
  int last_height = 0;
  int last_format = 0;
  int last_boxes = 0;
  tdl_app::AlgorithmResult last_result;
  std::string last_frame_path;
  std::string last_overlay_path;
};

bool runPass(const Options &opt, int pass_index, PassStats *stats,
             std::string *error) {
  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, error)) {
    return false;
  }

  const tdl_app::Camera::Config &camera_config = runtime.camera.config();
  std::cerr << "pass[" << pass_index << "] camera config: backend="
            << camera_demo_support::backendName(camera_config.backend)
            << " device=" << camera_config.device
            << " pipe=" << camera_config.pipe
            << " group=" << camera_config.group
            << " channel=" << camera_config.channel
            << " width=" << camera_config.width
            << " height=" << camera_config.height
            << " pixel_format=" << camera_config.pixel_format
            << " timeout_ms=" << camera_config.timeout_ms << "\n";

  tdl_app::Detector::Config det_config;
  det_config.model_spec = opt.model_spec;
  det_config.firmware = opt.firmware;
  tdl_app::Detector detector(det_config, error);
  if (!detector.initialized()) {
    camera_demo_support::closeCameraRuntime(&runtime);
    if (error && error->empty()) {
      *error = "detector load failed";
    }
    return false;
  }
  if (error && !error->empty()) {
    std::cerr << "pass[" << pass_index << "] detector warning: "
              << *error << "\n";
    error->clear();
  }
  std::cerr << "pass[" << pass_index << "] detector model_type="
            << detector.modelType() << "\n";

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  const int measured_frames = opt.camera.frames <= 0 ? 1 : opt.camera.frames;
  const int total_frames = opt.warmup + measured_frames;

  *stats = PassStats{};
  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = SteadyClock::now();
    if (!runtime.camera.read(&frame, error)) {
      camera_demo_support::closeCameraRuntime(&runtime);
      return false;
    }
    const auto read_end = SteadyClock::now();

    tdl_app::AlgorithmResult result;
    const auto infer_begin = SteadyClock::now();
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    if (!video) {
      camera_demo_support::closeCameraRuntime(&runtime);
      if (error) {
        *error = "frame has no native VIDEO_FRAME_INFO_S";
      }
      return false;
    }
    if (error) {
      error->clear();
    }
    result = detector(*video, infer_options, error);
    const auto infer_end = SteadyClock::now();
    if (error && !error->empty()) {
      camera_demo_support::closeCameraRuntime(&runtime);
      return false;
    }

    if (index >= opt.warmup) {
      const double read_ms = elapsedMs(read_begin, read_end);
      const double infer_ms = elapsedMs(infer_begin, infer_end);
      const double total_ms = elapsedMs(read_begin, infer_end);
      stats->read_sum_ms += read_ms;
      stats->infer_sum_ms += infer_ms;
      stats->total_sum_ms += total_ms;
      stats->box_sum += static_cast<double>(result.boxes.size());
      stats->frames++;
      stats->last_sequence = frame.sequence;
      stats->last_pts = frame.timestamp_us;
      stats->last_width = frame.width;
      stats->last_height = frame.height;
      stats->last_format = frame.format;
      stats->last_boxes = static_cast<int>(result.boxes.size());
      stats->last_result = result;
      if (index == total_frames - 1 && !opt.dump_frame.empty()) {
        const std::string frame_path =
            camera_demo_support::frameOutputPath(opt.dump_frame, pass_index);
        if (!camera_demo_support::saveFrameAsImage(frame, frame_path, error)) {
          camera_demo_support::closeCameraRuntime(&runtime);
          return false;
        }
        stats->last_frame_path = frame_path;
        if (!opt.dump_overlay.empty()) {
          const std::string overlay_path =
              camera_demo_support::frameOutputPath(opt.dump_overlay, pass_index);
          if (!writeOverlay(frame_path, overlay_path, result, error)) {
            camera_demo_support::closeCameraRuntime(&runtime);
            return false;
          }
          stats->last_overlay_path = overlay_path;
        }
      }
    }
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  return true;
}

void printPassSummary(int pass_index, const PassStats &stats) {
  const double denom = stats.frames > 0 ? static_cast<double>(stats.frames) : 1.0;
  std::cout << std::fixed << std::setprecision(3)
            << "pass[" << pass_index << "] summary: frames=" << stats.frames
            << " avg_read=" << (stats.read_sum_ms / denom)
            << " ms, avg_infer=" << (stats.infer_sum_ms / denom)
            << " ms, avg_total=" << (stats.total_sum_ms / denom)
            << " ms, avg_boxes=" << (stats.box_sum / denom)
            << ", avg_fps="
            << ((stats.total_sum_ms / denom) > 0.0
                    ? 1000.0 / (stats.total_sum_ms / denom)
                    : 0.0)
            << "\n";
  std::cout << "pass[" << pass_index << "] last_frame: seq="
            << stats.last_sequence << " pts=" << stats.last_pts
            << " src=" << stats.last_width << "x" << stats.last_height
            << " fmt=" << stats.last_format
            << " boxes=" << stats.last_boxes << "\n";
  if (!stats.last_frame_path.empty()) {
    std::cout << "pass[" << pass_index << "] saved_frame="
              << stats.last_frame_path << "\n";
  }
  if (!stats.last_overlay_path.empty()) {
    std::cout << "pass[" << pass_index << "] saved_overlay="
              << stats.last_overlay_path << "\n";
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  if (opt.repeat <= 0) {
    opt.repeat = 1;
  }
  if (!opt.dump_overlay.empty() && opt.dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return 1;
  }

  PassStats total;
  std::string error;
  for (int pass = 0; pass < opt.repeat; ++pass) {
    error.clear();
    PassStats pass_stats;
    if (!runPass(opt, pass, &pass_stats, &error)) {
      std::cerr << "pass[" << pass << "] failed: " << error << "\n";
      return 2;
    }
    printPassSummary(pass, pass_stats);
    total.read_sum_ms += pass_stats.read_sum_ms;
    total.infer_sum_ms += pass_stats.infer_sum_ms;
    total.total_sum_ms += pass_stats.total_sum_ms;
    total.box_sum += pass_stats.box_sum;
    total.frames += pass_stats.frames;
    total.last_sequence = pass_stats.last_sequence;
    total.last_pts = pass_stats.last_pts;
    total.last_width = pass_stats.last_width;
    total.last_height = pass_stats.last_height;
    total.last_format = pass_stats.last_format;
    total.last_boxes = pass_stats.last_boxes;
    total.last_result = pass_stats.last_result;
    total.last_frame_path = pass_stats.last_frame_path;
    total.last_overlay_path = pass_stats.last_overlay_path;
  }

  const double denom = total.frames > 0 ? static_cast<double>(total.frames) : 1.0;
  std::cout << std::fixed << std::setprecision(3)
            << "overall: repeat=" << opt.repeat
            << " frames=" << total.frames
            << " avg_read=" << (total.read_sum_ms / denom)
            << " ms, avg_infer=" << (total.infer_sum_ms / denom)
            << " ms, avg_total=" << (total.total_sum_ms / denom)
            << " ms, avg_boxes=" << (total.box_sum / denom)
            << ", avg_fps="
            << ((total.total_sum_ms / denom) > 0.0
                    ? 1000.0 / (total.total_sum_ms / denom)
                    : 0.0)
            << "\n";
  std::cout << "overall last_frame: seq=" << total.last_sequence
            << " pts=" << total.last_pts
            << " src=" << total.last_width << "x" << total.last_height
            << " fmt=" << total.last_format
            << " boxes=" << total.last_boxes << "\n";
  if (opt.dump_boxes) {
    printBoxes(total.last_result);
  }
  return 0;
}
