#include <cstdlib>
#include <iostream>
#include <string>

#include "camera_demo_support.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string output = "sensor_probe.jpg";
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_sensor_profile_probe_demo [default: dual-os existing MMF path]\n"
      << "                                [--use-sensor-media] [--attach-existing]\n"
      << "                                [--sensor-model NAME]\n"
      << "                                [--sensor-profile FILE]\n"
      << "                                [--sensor-ini FILE]\n"
      << "                                [--backend vi|vpss]\n"
      << "                                [--group N] [--channel N]\n"
      << "                                [--width N] [--height N]\n"
      << "                                [--pixel-format N]\n"
      << "                                [--timeout-ms N]\n"
      << "                                [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  if (!opt) {
    return false;
  }
  opt->camera.group = tdl_app::DualOsLayout::kCaptureVpssGroup;
  opt->camera.channel = tdl_app::DualOsLayout::kLiveChannel;
  opt->camera.width = tdl_app::DualOsLayout::kLiveWidth;
  opt->camera.height = tdl_app::DualOsLayout::kLiveHeight;
  opt->camera.pixel_format = 18;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &error)) {
      std::cerr << error << "\n";
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

    if (arg == "--output") {
      const char *v = value("--output");
      if (!v) return false;
      opt->output = v;
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

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  if (opt.camera.use_sensor_media) {
    if (!camera_demo_support::resolveSensorIni(&opt.camera, &error)) {
      std::cerr << "resolve sensor ini failed: " << error << "\n";
      return 2;
    }
    std::cout << "sensor probe: "
              << camera_demo_support::describeSensorSelection(opt.camera) << "\n";
  } else {
    std::cout << "sensor probe: dual-os existing MMF path"
              << " group=" << opt.camera.group
              << " channel=" << opt.camera.channel
              << " size=" << opt.camera.width << "x" << opt.camera.height
              << "\n";
  }

  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 3;
  }

  tdl_app::Frame frame;
  if (!runtime.camera.read(&frame, &error)) {
    std::cerr << "camera read failed: " << error << "\n";
    camera_demo_support::dumpCameraDiagnostics();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  if (!camera_demo_support::saveFrameAsImage(frame, opt.output, &error)) {
    std::cerr << "save frame failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }

  std::cout << "sensor probe ok: " << opt.output << "\n";
  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
