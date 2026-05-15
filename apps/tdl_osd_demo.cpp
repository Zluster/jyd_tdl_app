#include <cstdlib>
#include <iostream>
#include <string>

#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  int handle = 100;
  int width = 300;
  int height = 200;
  int pixel_format = 4;
  int canvas_count = 2;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_osd_demo [--handle N] [--width N] [--height N]\n"
      << "               [--pixel-format N] [--canvas-count N]\n";
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

    if (arg == "--handle") {
      const char *v = value("--handle");
      if (!v) return false;
      opt->handle = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--pixel-format") {
      const char *v = value("--pixel-format");
      if (!v) return false;
      opt->pixel_format = std::atoi(v);
    } else if (arg == "--canvas-count") {
      const char *v = value("--canvas-count");
      if (!v) return false;
      opt->canvas_count = std::atoi(v);
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

  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "sys open failed: " << error << "\n";
    return 2;
  }

  tdl_app::OsdRegion::Config config;
  config.handle = opt.handle;
  config.size = {opt.width, opt.height};
  config.pixel_format = opt.pixel_format;
  config.canvas_count = opt.canvas_count;

  tdl_app::OsdRegion osd(config);
  if (!osd.create(&error)) {
    std::cerr << "osd create failed: " << error << "\n";
    return 3;
  }

  std::cout << "osd created: handle=" << opt.handle
            << " size=" << opt.width << "x" << opt.height
            << " fmt=" << opt.pixel_format
            << " canvas=" << opt.canvas_count << "\n";
  osd.destroy();
  return 0;
}
