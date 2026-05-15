#include <cstdlib>
#include <chrono>
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
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_feature_demo --model-spec FILE\n"
      << "                         [--firmware FILE]\n"
      << "                         [--backend vi|vpss]\n"
      << "                         [--use-mmf | --use-sensor-media]\n"
      << "                         [--attach-existing]\n"
      << "                         [--sensor-ini FILE] [--frames N]\n"
      << "                         [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                         [--width N] [--height N] [--pixel-format N]\n"
      << "                         [--timeout-ms N] [--hold-ms N]\n";
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
  const int frame_limit =
      opt.camera.frames <= 0 ? std::numeric_limits<int>::max() : opt.camera.frames;
  for (int index = 0; index < frame_limit; ++index) {
    tdl_app::Frame frame;
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }

    tdl_app::AlgorithmResult result;
    if (!extractor.runFrame(frame, infer_options, &result, &error)) {
      std::cerr << "feature extractor run failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }

    std::cout << "frame[" << index << "]\n";
    demo_support::dumpResult(result);
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
