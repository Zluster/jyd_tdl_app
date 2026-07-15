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
  std::string dump_frame;
  std::string dump_overlay;
  int warmup = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_feature_demo --model-spec FILE\n"
      << "                         [--firmware FILE]\n"
      << "                         [--backend vi|vpss]\n"
      << "                         [default: dual-os existing MMF path]\n"
      << "                         [--use-mmf | --use-sensor-media]\n"
      << "                         [--attach-existing]\n"
      << "                         [--sensor-ini FILE] [--frames N]\n"
      << "                         [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                         [--width N] [--height N] [--pixel-format N]\n"
      << "                         [--timeout-ms N] [--hold-ms N] [--warmup N]\n"
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
  if (opt->warmup < 0 ||
      (!opt->dump_overlay.empty() && opt->dump_frame.empty())) {
    std::cerr << "invalid --warmup or dump options\n";
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

  tdl_app::FeatureExtractor::Config feature_config;
  feature_config.model_spec = opt.model_spec;
  feature_config.firmware = opt.firmware;
  tdl_app::FeatureExtractor extractor;
  if (!extractor.load(feature_config, &error)) {
    std::cerr << "feature extractor load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  tdl_app::InferOptions infer_options;
  if (opt.camera.frames <= 0) {
    std::cerr << "--frames must be positive when benchmarking\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }
  const int total_frames = opt.warmup + opt.camera.frames;
  double read_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  double total_sum_ms = 0.0;
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

    tdl_app::AlgorithmResult result;
    const auto infer_begin = read_end;
    if (!extractor.runFrame(frame, infer_options, &result, &error)) {
      std::cerr << "feature extractor run failed: " << error << "\n";
      runtime.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }
    const auto infer_end = std::chrono::steady_clock::now();
    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error)) {
        std::cerr << "failed to save feature frame: " << error << "\n";
        runtime.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&runtime);
        return 7;
      }
      if (!opt.dump_overlay.empty()) {
        cv::Mat image = cv::imread(opt.dump_frame, cv::IMREAD_COLOR);
        if (image.empty()) {
          error = "failed to read feature snapshot";
        } else {
          cv::putText(image, "feature_dim=" + std::to_string(result.feature.size()),
                      cv::Point(12, 28), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                      cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
          if (!cv::imwrite(opt.dump_overlay, image)) error = "failed to write feature overlay";
        }
        if (!error.empty()) {
          std::cerr << "failed to save feature overlay: " << error << "\n";
          runtime.camera.releaseFrame();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 7;
        }
      }
    }
    runtime.camera.releaseFrame();
    if (index < opt.warmup) continue;
    read_sum_ms += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
    infer_sum_ms += std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
    total_sum_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - total_begin).count();
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
            << " feature_dim=" << last_result.feature.size() << "\n";
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  demo_support::dumpResult(last_result);
  return 0;
}
