#include <cstdlib>
#include <iostream>
#include <string>

#include "demo_support.hpp"
#include "image_demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  image_demo_support::CommonOptions common;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_yolov5_demo --image FILE\n"
      << "                  [--model-spec FILE]\n"
      << "                  [--firmware FILE] [--threshold 0.25]\n"
      << "                  [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  opt->common.model_spec = "./configs/model_specs/yolov5s_det_coco80.ini";
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

    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  std::string error;
  if (!image_demo_support::validateRequired(opt->common, false, &error)) {
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
  tdl_app::Detector::Config config = image_demo_support::detectorConfig(opt.common);
  config.model_type = "YOLOV5";
  tdl_app::Detector detector(config, &error);
  if (!detector.initialized()) {
    std::cerr << "initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.common.threshold;

  tdl_app::AlgorithmResult result = detector(opt.common.image, infer_options, &error);
  if (!error.empty()) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  demo_support::printLabels(result);
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
