#include <cstdlib>
#include <iostream>
#include <string>

#include "demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string image;
  std::string detector_model_spec;
  std::string attribute_model_spec;
  std::string firmware;
  std::string output;
  float threshold = 0.25f;
};

const char *valueForArg(int argc, char **argv, int *index, const char *name) {
  if (!index || *index + 1 >= argc) {
    std::cerr << "missing value for " << name << "\n";
    return nullptr;
  }
  return argv[++(*index)];
}

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_face_attr_pipeline_demo --image FILE\n"
      << "      --detector-model-spec FILE --attribute-model-spec FILE\n"
      << "      [--firmware FILE] [--threshold 0.25] [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  if (!opt) {
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--image") {
      const char *value = valueForArg(argc, argv, &i, "--image");
      if (!value) return false;
      opt->image = value;
      continue;
    }
    if (arg == "--detector-model-spec") {
      const char *value =
          valueForArg(argc, argv, &i, "--detector-model-spec");
      if (!value) return false;
      opt->detector_model_spec = value;
      continue;
    }
    if (arg == "--attribute-model-spec") {
      const char *value =
          valueForArg(argc, argv, &i, "--attribute-model-spec");
      if (!value) return false;
      opt->attribute_model_spec = value;
      continue;
    }
    if (arg == "--firmware") {
      const char *value = valueForArg(argc, argv, &i, "--firmware");
      if (!value) return false;
      opt->firmware = value;
      continue;
    }
    if (arg == "--threshold") {
      const char *value = valueForArg(argc, argv, &i, "--threshold");
      if (!value) return false;
      opt->threshold = static_cast<float>(std::atof(value));
      continue;
    }
    if (arg == "--output") {
      const char *value = valueForArg(argc, argv, &i, "--output");
      if (!value) return false;
      opt->output = value;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    }

    std::cerr << "unknown argument: " << arg << "\n";
    return false;
  }

  if (opt->image.empty()) {
    std::cerr << "image path is required\n";
    return false;
  }
  if (opt->detector_model_spec.empty()) {
    std::cerr << "detector model spec is required\n";
    return false;
  }
  if (opt->attribute_model_spec.empty()) {
    std::cerr << "attribute model spec is required\n";
    return false;
  }
  return true;
}

void dumpStageResult(const tdl_app::MultiStagePipeline::StageResult &stage) {
  std::cout << "stage[" << stage.stage_index << "] " << stage.name;
  if (stage.source_stage >= 0) {
    std::cout << " source_stage=" << stage.source_stage
              << " source_result=" << stage.source_result_index
              << " source_box=" << stage.source_box_index;
  }
  std::cout << "\n";
  demo_support::dumpResult(stage.output);
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;

  tdl_app::FaceDetector detector;
  tdl_app::FaceDetector::Config detector_config;
  detector_config.model_spec = opt.detector_model_spec;
  detector_config.firmware = opt.firmware;
  if (!detector.load(detector_config, &error)) {
    std::cerr << "face detector initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::FaceAttributeClassifier classifier;
  tdl_app::FaceAttributeClassifier::Config attribute_config;
  attribute_config.model_spec = opt.attribute_model_spec;
  attribute_config.firmware = opt.firmware;
  if (!classifier.load(attribute_config, &error)) {
    std::cerr << "face attribute initialize failed: " << error << "\n";
    return 3;
  }

  tdl_app::MultiStagePipeline pipeline;
  pipeline.setImage(opt.image);
  const int detect_stage = pipeline.addFaceDetectorStage("face_detect", detector);
  pipeline.addFaceAttributeStage("face_attribute", classifier, detect_stage);

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;

  tdl_app::MultiStagePipeline::Result result;
  if (!pipeline.runOnce(infer_options, &result, &error)) {
    std::cerr << "pipeline run failed: " << error << "\n";
    return 4;
  }

  if (!opt.output.empty() &&
      !demo_support::saveAnnotatedImage(opt.image, opt.output, result.primary,
                                        &error)) {
    std::cerr << "save failed: " << error << "\n";
    return 5;
  }

  if (!opt.output.empty()) {
    std::cout << "saved: " << opt.output << "\n";
  }

  std::cout << "primary result\n";
  demo_support::dumpResult(result.primary);
  for (const auto &stage : result.stages) {
    dumpStageResult(stage);
  }
  return 0;
}
