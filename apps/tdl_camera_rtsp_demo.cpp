#include <cstdlib>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  int venc_channel = 0;
  float threshold = 0.25f;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_rtsp_demo --model-spec FILE\n"
      << "                       [--firmware FILE]\n"
      << "                       [--backend vi|vpss]\n"
      << "                       [--use-mmf | --use-sensor-media]\n"
      << "                       [--attach-existing]\n"
      << "                       [--sensor-ini FILE] [--frames N]\n"
      << "                       [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                       [--width N] [--height N] [--pixel-format N]\n"
      << "                       [--timeout-ms N] [--hold-ms N]\n"
      << "                       [--venc-channel N] [--threshold 0.25]\n";
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
    } else if (arg == "--venc-channel") {
      const char *v = value("--venc-channel");
      if (!v) return false;
      opt->venc_channel = std::atoi(v);
    } else if (arg == "--threshold") {
      const char *v = value("--threshold");
      if (!v) return false;
      opt->threshold = static_cast<float>(std::atof(v));
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

  tdl_app::Detector::Config det_config;
  det_config.model_spec = opt.model_spec;
  det_config.firmware = opt.firmware;
  tdl_app::Detector detector;
  if (!detector.load(det_config, &error)) {
    std::cerr << "detector load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  bool rtsp_opened = false;
  std::unique_ptr<tdl_app::RtspStreamer> streamer;

  const int frame_limit =
      opt.camera.frames <= 0 ? std::numeric_limits<int>::max() : opt.camera.frames;
  for (int index = 0; index < frame_limit; ++index) {
    tdl_app::Frame frame;
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      if (rtsp_opened) {
        streamer->close();
      }
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }

    tdl_app::InferOptions infer_options;
    infer_options.threshold = opt.threshold;
    tdl_app::AlgorithmResult result;
    if (!detector.detectFrame(frame, infer_options, &result, &error)) {
      std::cerr << "detector run failed: " << error << "\n";
      if (rtsp_opened) {
        streamer->close();
      }
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }

    if (!rtsp_opened) {
      tdl_app::RtspStreamer::Config rtsp_config;
      rtsp_config.venc_channel = opt.venc_channel;
      rtsp_config.width = frame.width;
      rtsp_config.height = frame.height;
      rtsp_config.codec = tdl_app::RtspStreamer::Codec::H264;
      streamer.reset(new tdl_app::RtspStreamer(rtsp_config));
      if (!streamer->open(&error)) {
        std::cerr << "rtsp open failed: " << error << "\n";
        camera_demo_support::closeCameraRuntime(&runtime);
        return 7;
      }
      rtsp_opened = true;
      std::cout << "rtsp: started\n";
    }

    if (!streamer->write(frame, result, &error)) {
      std::cerr << "rtsp write failed: " << error << "\n";
      streamer->close();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 8;
    }
    std::cout << "rtsp: frame sent\n";
  }

  if (rtsp_opened) {
    streamer->close();
  }
  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
