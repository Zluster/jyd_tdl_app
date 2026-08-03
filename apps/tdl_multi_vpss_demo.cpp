#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"
#include "tdl_app/camera.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  int group0 = tdl_app::DualOsLayout::kCaptureVpssGroup;
  int channel0 = tdl_app::DualOsLayout::kAiChannel;
  int width0 = tdl_app::DualOsLayout::kAiWidth;
  int height0 = tdl_app::DualOsLayout::kAiHeight;
  int group1 = 1;
  int channel1 = 0;
  int width1 = 960;
  int height1 = 540;
  int pixel_format = 18;
  int timeout_ms = 1000;
  std::string output0 = "frame_g0.jpg";
  std::string output1 = "frame_g1.jpg";
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_multi_vpss_demo [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                      [--group0 N] [--channel0 N] [--width0 N] [--height0 N]\n"
      << "                      [--group1 N] [--channel1 N] [--width1 N] [--height1 N]\n"
      << "                      [--pixel-format N] [--timeout-ms N]\n"
      << "                      [--output0 FILE] [--output1 FILE]\n";
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

    if (arg == "--group0") {
      const char *v = value("--group0");
      if (!v) return false;
      opt->group0 = std::atoi(v);
    } else if (arg == "--channel0") {
      const char *v = value("--channel0");
      if (!v) return false;
      opt->channel0 = std::atoi(v);
    } else if (arg == "--width0") {
      const char *v = value("--width0");
      if (!v) return false;
      opt->width0 = std::atoi(v);
    } else if (arg == "--height0") {
      const char *v = value("--height0");
      if (!v) return false;
      opt->height0 = std::atoi(v);
    } else if (arg == "--group1") {
      const char *v = value("--group1");
      if (!v) return false;
      opt->group1 = std::atoi(v);
    } else if (arg == "--channel1") {
      const char *v = value("--channel1");
      if (!v) return false;
      opt->channel1 = std::atoi(v);
    } else if (arg == "--width1") {
      const char *v = value("--width1");
      if (!v) return false;
      opt->width1 = std::atoi(v);
    } else if (arg == "--height1") {
      const char *v = value("--height1");
      if (!v) return false;
      opt->height1 = std::atoi(v);
    } else if (arg == "--pixel-format") {
      const char *v = value("--pixel-format");
      if (!v) return false;
      opt->pixel_format = std::atoi(v);
    } else if (arg == "--timeout-ms") {
      const char *v = value("--timeout-ms");
      if (!v) return false;
      opt->timeout_ms = std::atoi(v);
    } else if (arg == "--output0") {
      const char *v = value("--output0");
      if (!v) return false;
      opt->output0 = v;
    } else if (arg == "--output1") {
      const char *v = value("--output1");
      if (!v) return false;
      opt->output1 = v;
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

bool captureOne(const tdl_app::Camera::Config &camera_config,
                const std::string &output_path, std::string *error) {
  tdl_app::Camera camera(camera_config);
  if (!camera.open(error)) {
    return false;
  }

  tdl_app::Frame frame;
  const bool ok = camera.read(&frame, error) &&
                  camera_demo_support::saveFrameAsImage(frame, output_path, error);
  camera.close();
  return ok;
}

tdl_app::Camera::Config makeVpssCameraConfig(int group, int channel, int width,
                                             int height, int pixel_format,
                                             int timeout_ms) {
  tdl_app::Camera::Config config;
  config.backend = tdl_app::Camera::Backend::Vpss;
  config.group = group;
  config.channel = channel;
  config.width = width;
  config.height = height;
  config.pixel_format = pixel_format;
  config.timeout_ms = timeout_ms;
  return config;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  const tdl_app::Camera::Config camera0 = makeVpssCameraConfig(
      opt.group0, opt.channel0, opt.width0, opt.height0, opt.pixel_format,
      opt.timeout_ms);
  const tdl_app::Camera::Config camera1 = makeVpssCameraConfig(
      opt.group1, opt.channel1, opt.width1, opt.height1, opt.pixel_format,
      opt.timeout_ms);

  if (!captureOne(camera0, opt.output0, &error)) {
    std::cerr << "capture group0 failed: " << error << "\n";
    camera_demo_support::dumpCameraDiagnostics();
    return 2;
  }
  if (!captureOne(camera1, opt.output1, &error)) {
    std::cerr << "capture group1 failed: " << error << "\n";
    camera_demo_support::dumpCameraDiagnostics();
    return 3;
  }

  std::cout << "saved: " << opt.output0 << "\n";
  std::cout << "saved: " << opt.output1 << "\n";
  return 0;
}
