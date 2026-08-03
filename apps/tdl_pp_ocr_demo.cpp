#include <iostream>
#include <string>

#include "demo_support.hpp"
#include "image_demo_support.hpp"
#include "ocr_overlay_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  image_demo_support::CommonOptions common;
  std::string font = "./fonts/DroidSansFallbackFull.ttf";
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_pp_ocr_demo --image FILE --model-spec FILE\n"
      << "                  [--firmware FILE] [--font FILE] [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
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
    if (std::string(argv[i]) == "--font") {
      if (i + 1 >= argc) {
        std::cerr << "--font requires a value\n";
        return false;
      }
      opt->font = argv[++i];
      continue;
    } else if (std::string(argv[i]) == "-h" ||
               std::string(argv[i]) == "--help") {
      printUsage();
      std::exit(0);
    }
    std::cerr << "unknown argument: " << argv[i] << "\n";
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

  tdl_app::AlgorithmResult result;
  tdl_app::InferOptions infer_options;
  if (!recognizer.run(opt.common.image, infer_options, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  if (!opt.common.output.empty()) {
    if (!ocr_overlay_support::saveAnnotatedImage(
            opt.common.image, opt.common.output, result, opt.font, &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 4;
    }
    std::cout << "saved: " << opt.common.output << "\n";
  }

  demo_support::dumpResult(result);
  return 0;
}
