#include <cstdlib>
#include <iostream>
#include <string>

#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string image;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_semantic_seg_demo --image FILE --model-spec FILE\n"
      << "                        [--firmware FILE] [--model-dir DIR]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto requireValue = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--image") {
      const char *value = requireValue("--image");
      if (!value) return false;
      opt->image = value;
    } else if (arg == "--model-spec") {
      const char *value = requireValue("--model-spec");
      if (!value) return false;
      opt->model_spec = value;
    } else if (arg == "--firmware") {
      const char *value = requireValue("--firmware");
      if (!value) return false;
      opt->firmware = value;
    } else if (arg == "--model-dir") {
      const char *value = requireValue("--model-dir");
      if (!value) return false;
      opt->model_dir = value;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (opt->image.empty()) {
    std::cerr << "image path is required\n";
    return false;
  }
  if (opt->model_spec.empty()) {
    std::cerr << "model-spec is required\n";
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

  tdl_app::SemanticSegmenter segmenter;
  std::string error;
  if (!segmenter.load(tdl_app::ModelSessionConfig::fromSpec(
                          opt.model_spec, opt.firmware, opt.model_dir),
                      &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }

  tdl_app::SemanticSegmentationResult result;
  if (!segmenter.run(opt.image, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  std::cout << "output_width: " << result.output_width << "\n";
  std::cout << "output_height: " << result.output_height << "\n";
  std::cout << "pixels: " << result.pixelCount() << "\n";
  return 0;
}
