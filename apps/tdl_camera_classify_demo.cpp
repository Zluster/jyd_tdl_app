#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  float threshold = 0.25f;
  int top_k = 5;
  int warmup = 0;
  std::string dump_frame;
  std::string dump_overlay;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_classify_demo --model-spec FILE\n"
      << "                           [--firmware FILE]\n"
      << "                           [--backend vi|vpss]\n"
      << "                           [default: dual-os existing MMF path]\n"
      << "                           [--use-mmf | --use-sensor-media]\n"
      << "                           [--attach-existing]\n"
      << "                           [--sensor-ini FILE] [--frames N]\n"
      << "                           [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                           [--width N] [--height N] [--pixel-format N]\n"
      << "                           [--timeout-ms N] [--hold-ms N]\n"
      << "                           [--threshold 0.25] [--top-k 5] [--warmup N]\n";
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
    } else if (arg == "--top-k") {
      const char *v = value("--top-k");
      if (!v) return false;
      opt->top_k = std::atoi(v);
    } else if (arg == "--warmup") {
      const char *v = value("--warmup");
      if (!v) return false;
      opt->warmup = std::atoi(v);
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
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  if (opt->warmup < 0) {
    std::cerr << "--warmup must be non-negative\n";
    return false;
  }
  return !opt->model_spec.empty();
}

bool writeOverlay(const std::string &input, const std::string &output,
                  const tdl_app::AlgorithmResult &result, std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read snapshot for overlay";
    return false;
  }
  int y = 28;
  for (const auto &item : result.classes) {
    std::string label = "class=" + std::to_string(item.class_id);
    if (item.class_id >= 0 && static_cast<size_t>(item.class_id) < result.labels.size()) {
      label = result.labels[static_cast<size_t>(item.class_id)];
    }
    label += cv::format(" %.3f", item.score);
    cv::putText(image, label, cv::Point(12, y), cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    y += 26;
  }
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write classification overlay";
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

  if (opt.camera.hold_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.camera.hold_ms));
  }

  tdl_app::Classifier::Config cls_config;
  cls_config.model_spec = opt.model_spec;
  cls_config.firmware = opt.firmware;
  tdl_app::Classifier classifier;
  if (!classifier.load(cls_config, &error)) {
    std::cerr << "classifier load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  infer_options.top_k = opt.top_k;
  if (opt.camera.frames <= 0) {
    std::cerr << "--frames must be positive when benchmarking\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }
  const int frame_limit = opt.warmup + opt.camera.frames;
  double read_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  int measured_frames = 0;
  for (int index = 0; index < frame_limit; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = std::chrono::steady_clock::now();
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }
    const auto read_end = std::chrono::steady_clock::now();

    tdl_app::AlgorithmResult result;
    const auto infer_begin = std::chrono::steady_clock::now();
    if (!classifier.runFrame(frame, infer_options, &result, &error)) {
      std::cerr << "classifier run failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }
    const auto infer_end = std::chrono::steady_clock::now();

    if (index == frame_limit - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !writeOverlay(opt.dump_frame, opt.dump_overlay, result, &error))) {
        std::cerr << "failed to save result: " << error << "\n";
        camera_demo_support::closeCameraRuntime(&runtime);
        return 7;
      }
    }
    runtime.camera.releaseFrame();
    if (index < opt.warmup) {
      continue;
    }
    read_sum_ms += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
    infer_sum_ms += std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
    ++measured_frames;

    if (index == frame_limit - 1) {
      demo_support::printLabels(result);
      demo_support::dumpResult(result);
    }
  }

  if (measured_frames > 0) {
    const double avg_read_ms = read_sum_ms / measured_frames;
    const double avg_infer_ms = infer_sum_ms / measured_frames;
    const double avg_total_ms = avg_read_ms + avg_infer_ms;
    std::cout << std::fixed << std::setprecision(3)
              << "summary: frames=" << measured_frames
              << " avg_read_ms=" << avg_read_ms
              << " avg_infer_ms=" << avg_infer_ms
              << " avg_total_ms=" << avg_total_ms
              << " fps=" << (avg_total_ms > 0.0 ? 1000.0 / avg_total_ms : 0.0)
              << "\n";
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
