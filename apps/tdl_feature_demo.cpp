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
      << "  tdl_feature_demo --image FILE --model-spec FILE\n"
      << "                   [--firmware FILE]\n";
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

    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
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
  tdl_app::FeatureExtractor extractor;
  if (!extractor.load(image_demo_support::featureConfig(opt.common), &error)) {
    std::cerr << "initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::InferOptions infer_options;
  tdl_app::AlgorithmResult result;
  if (!extractor.run(opt.common.image, infer_options, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  demo_support::dumpResult(result);
  return 0;
}
