#pragma once

#include <cstdlib>
#include <string>

#include "demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace image_demo_support {

struct CommonOptions {
  std::string image;
  std::string model_spec;
  std::string firmware;
  std::string output;
  float threshold = 0.25f;
  int top_k = 5;
};

inline const char *valueForArg(int argc, char **argv, int *index,
                               const char *name, std::string *error) {
  if (!index || *index + 1 >= argc) {
    if (error) {
      *error = std::string("missing value for ") + name;
    }
    return nullptr;
  }
  return argv[++(*index)];
}

inline bool parseCommonArgs(int argc, char **argv, int *index,
                            CommonOptions *opt, bool *handled,
                            std::string *error = nullptr) {
  if (!index || !opt || !handled) {
    if (error) {
      *error = "image demo parse received null pointer";
    }
    return false;
  }

  *handled = true;
  const std::string arg = argv[*index];
  if (arg == "--image") {
    const char *v = valueForArg(argc, argv, index, "--image", error);
    if (!v) return false;
    opt->image = v;
  } else if (arg == "--model-spec") {
    const char *v = valueForArg(argc, argv, index, "--model-spec", error);
    if (!v) return false;
    opt->model_spec = v;
  } else if (arg == "--firmware") {
    const char *v = valueForArg(argc, argv, index, "--firmware", error);
    if (!v) return false;
    opt->firmware = v;
  } else if (arg == "--output") {
    const char *v = valueForArg(argc, argv, index, "--output", error);
    if (!v) return false;
    opt->output = v;
  } else if (arg == "--threshold") {
    const char *v = valueForArg(argc, argv, index, "--threshold", error);
    if (!v) return false;
    opt->threshold = static_cast<float>(std::atof(v));
  } else if (arg == "--top-k") {
    const char *v = valueForArg(argc, argv, index, "--top-k", error);
    if (!v) return false;
    opt->top_k = std::atoi(v);
  } else {
    *handled = false;
  }
  return true;
}

inline bool validateRequired(const CommonOptions &opt, bool require_model_spec,
                             std::string *error = nullptr) {
  if (opt.image.empty()) {
    if (error) {
      *error = "image path is required";
    }
    return false;
  }
  if (require_model_spec && opt.model_spec.empty()) {
    if (error) {
      *error = "model-spec is required";
    }
    return false;
  }
  return true;
}

inline tdl_app::Detector::Config detectorConfig(const CommonOptions &opt) {
  tdl_app::Detector::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline tdl_app::Classifier::Config classifierConfig(const CommonOptions &opt) {
  tdl_app::Classifier::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline tdl_app::FaceDetector::Config faceDetectorConfig(
    const CommonOptions &opt) {
  tdl_app::FaceDetector::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline tdl_app::FaceAttributeClassifier::Config faceAttributeConfig(
    const CommonOptions &opt) {
  tdl_app::FaceAttributeClassifier::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline tdl_app::PlateRecognizer::Config plateRecognizerConfig(
    const CommonOptions &opt) {
  tdl_app::PlateRecognizer::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline tdl_app::FeatureExtractor::Config featureConfig(
    const CommonOptions &opt) {
  tdl_app::FeatureExtractor::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  return config;
}

inline bool saveAnnotatedOutputIfRequested(
    const CommonOptions &opt, const tdl_app::AlgorithmResult &result,
    std::string *error = nullptr) {
  if (opt.output.empty()) {
    return true;
  }
  return demo_support::saveAnnotatedImage(opt.image, opt.output, result, error);
}

}  // namespace image_demo_support
