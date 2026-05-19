#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "demo_support.hpp"
#include "image_demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  image_demo_support::CommonOptions common;
  bool has_roi = false;
  tdl_app::Box roi;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_plate_recognize_demo --image FILE --model-spec FILE\n"
      << "                           [--firmware FILE] [--roi x,y,w,h]\n"
      << "                           [--output FILE]\n";
}

bool parseRoi(const std::string &value, tdl_app::Box *roi) {
  if (!roi) {
    return false;
  }
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  if (std::sscanf(value.c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) != 4) {
    return false;
  }
  roi->x1 = x;
  roi->y1 = y;
  roi->x2 = x + w;
  roi->y2 = y + h;
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!image_demo_support::parseCommonArgs(argc, argv, &i, &opt->common,
                                             &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }

    if (arg == "--roi") {
      const char *value =
          image_demo_support::valueForArg(argc, argv, &i, "--roi", &parse_error);
      if (!value || !parseRoi(value, &opt->roi)) {
        std::cerr << "invalid --roi value, expected x,y,w,h\n";
        return false;
      }
      opt->has_roi = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    }

    std::cerr << "unknown argument: " << arg << "\n";
    return false;
  }

  std::string error;
  if (!image_demo_support::validateRequired(opt->common, true, &error)) {
    std::cerr << error << "\n";
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

  std::string error;
  tdl_app::PlateRecognizer recognizer;
  if (!recognizer.load(image_demo_support::plateRecognizerConfig(opt.common),
                       &error)) {
    std::cerr << "initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::InferOptions infer_options;
  tdl_app::AlgorithmResult result;
  const bool ok = opt.has_roi
                      ? recognizer.run(opt.common.image, opt.roi, infer_options,
                                       &result, &error)
                      : recognizer.run(opt.common.image, infer_options, &result,
                                       &error);
  if (!ok) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  if (!opt.common.output.empty()) {
    if (!image_demo_support::saveAnnotatedOutputIfRequested(opt.common, result,
                                                            &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 4;
    }
    std::cout << "saved: " << opt.common.output << "\n";
  }

  demo_support::dumpResult(result);
  return 0;
}
