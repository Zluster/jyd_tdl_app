#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "camera_demo_support.hpp"
#include "demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  std::string dump_frame;
  std::string dump_overlay;
  float threshold = 0.25f;
  int warmup = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_detect_demo --model-spec FILE\n"
      << "                         [--firmware FILE]\n"
      << "                         [--backend vi|vpss]\n"
      << "                         [default: dual-os existing MMF path]\n"
      << "                         [--use-mmf | --use-sensor-media]\n"
      << "                         [--attach-existing]\n"
      << "                         [--sensor-ini FILE] [--frames N]\n"
      << "                         [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                         [--width N] [--height N] [--pixel-format N]\n"
      << "                         [--timeout-ms N] [--hold-ms N]\n"
      << "                         [--threshold 0.25] [--warmup N]\n"
      << "                         [--dump-frame FILE] [--dump-overlay FILE]\n";
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
  if (opt->warmup < 0) {
    std::cerr << "--warmup must be non-negative\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  return !opt->model_spec.empty();
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

  tdl_app::Detector::Config det_config;
  det_config.model_spec = opt.model_spec;
  det_config.firmware = opt.firmware;
  std::cerr << "detector: load begin model_spec=" << det_config.model_spec
            << " firmware=" << det_config.firmware << "\n";
  tdl_app::Detector detector(det_config, &error);
  if (!detector.initialized()) {
    std::cerr << "detector load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }
  std::cerr << "detector: load done model_type=" << detector.modelType() << "\n";

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  if (opt.camera.frames <= 0) {
    std::cerr << "--frames must be positive when benchmarking\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }
  const int total_frames = opt.warmup + opt.camera.frames;
  double read_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double box_sum = 0.0;
  tdl_app::AlgorithmResult last_result;
  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto total_begin = std::chrono::steady_clock::now();
    const auto read_begin = total_begin;
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }
    const auto read_end = std::chrono::steady_clock::now();
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    if (!video) {
      std::cerr << "detector run failed: frame has no native VIDEO_FRAME_INFO_S\n";
      runtime.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }
    const auto infer_begin = std::chrono::steady_clock::now();
    error.clear();
    tdl_app::AlgorithmResult result = detector(*video, infer_options, &error);
    const auto infer_end = std::chrono::steady_clock::now();
    if (!error.empty()) {
      std::cerr << "detector run failed: " << error << "\n";
      runtime.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }
    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !demo_support::saveAnnotatedImage(opt.dump_frame, opt.dump_overlay,
                                             result, &error))) {
        std::cerr << "failed to save detection result: " << error << "\n";
        runtime.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&runtime);
        return 7;
      }
    }
    runtime.camera.releaseFrame();
    if (index < opt.warmup) continue;
    read_sum_ms += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
    infer_sum_ms += std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
    total_sum_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - total_begin).count();
    box_sum += result.boxes.size();
    last_result = std::move(result);
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  const double count = static_cast<double>(opt.camera.frames);
  const double avg_total_ms = total_sum_ms / count;
  std::cout << std::fixed << std::setprecision(3)
            << "camera_frames=" << opt.camera.frames
            << " avg_read_ms=" << read_sum_ms / count
            << " avg_infer_ms=" << infer_sum_ms / count
            << " avg_total_ms=" << avg_total_ms
            << " fps=" << (avg_total_ms > 0.0 ? 1000.0 / avg_total_ms : 0.0)
            << " avg_boxes=" << box_sum / count << "\n";
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  demo_support::dumpResult(last_result);
  return 0;
}
