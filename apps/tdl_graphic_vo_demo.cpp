#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  std::string device = "/dev/fb0";
  int width = 720;
  int height = 1280;
  std::uint32_t color = 0xFF202040U;
  bool hide = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_graphic_vo_demo [--device /dev/fb0] [--width N] [--height N]\n"
      << "                      [--color 0xAARRGGBB] [--hide]\n";
}

bool parseColor(const std::string &text, std::uint32_t *value) {
  char *end = nullptr;
  unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--device") {
      const char *v = value("--device");
      if (!v) return false;
      opt->device = v;
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--color") {
      const char *v = value("--color");
      if (!v) return false;
      if (!parseColor(v, &opt->color)) {
        std::cerr << "invalid color: " << v << "\n";
        return false;
      }
    } else if (arg == "--hide") {
      opt->hide = true;
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
  tdl_app::GraphicVoLayer::Config config =
      tdl_app::GraphicVoLayer::argb8888(0, opt.width, opt.height, opt.device);
  config.show = !opt.hide;

  tdl_app::GraphicVoLayer layer(config);
  if (!layer.open(&error)) {
    std::cerr << "graphic vo open failed: " << error << "\n";
    return 2;
  }

  if (!layer.clear(opt.color, &error)) {
    std::cerr << "graphic vo clear failed: " << error << "\n";
    return 3;
  }
  if (!layer.present(&error)) {
    std::cerr << "graphic vo present failed: " << error << "\n";
    return 4;
  }

  const auto view = layer.buffer();
  std::cout << "graphic_vo: device=" << opt.device
            << " size=" << view.width << "x" << view.height
            << " stride=" << view.stride
            << " bpp=" << (view.bytes_per_pixel * 8)
            << " bytes=" << view.bytes << "\n";
  return 0;
}
