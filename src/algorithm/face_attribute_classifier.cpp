#include "tdl_app/face_attribute_classifier.hpp"

#include <memory>
#include <utility>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_face_attribute.hpp"

namespace tdl_app {
namespace {

EngineConfig toEngineConfig(const FaceAttributeClassifier::Config &config) {
  EngineConfig out;
  out.model_descriptor_file = config.model_spec;
  out.model_dir = config.model_dir;
  out.bmrt_firmware = config.firmware;
  return out;
}

std::string resolveModelType(const FaceAttributeClassifier::Config &config) {
  if (!config.model_type.empty()) {
    return config.model_type;
  }
  if (!config.model_spec.empty()) {
    ModelDescriptor descriptor;
    std::string error;
    if (loadModelDescriptor(config.model_spec, &descriptor, &error)) {
      if (!descriptor.model_type.empty()) {
        return descriptor.model_type;
      }
      if (!descriptor.runtime.empty()) {
        return descriptor.runtime;
      }
    }
  }
  return "FACE_ATTRIBUTE";
}

}  // namespace

FaceAttributeClassifier::FaceAttributeClassifier() = default;

FaceAttributeClassifier::FaceAttributeClassifier(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

FaceAttributeClassifier::~FaceAttributeClassifier() = default;

FaceAttributeClassifier::FaceAttributeClassifier(
    FaceAttributeClassifier &&) noexcept = default;

FaceAttributeClassifier &FaceAttributeClassifier::operator=(
    FaceAttributeClassifier &&) noexcept = default;

bool FaceAttributeClassifier::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = resolveModelType(config_);
  model_.reset(new NnFaceAttribute(requested_model_type_));
  return model_->load(toEngineConfig(config_), error);
}

bool FaceAttributeClassifier::load(const std::string &model_spec,
                                   std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool FaceAttributeClassifier::load(const std::string &model_spec,
                                   const std::string &firmware,
                                   std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool FaceAttributeClassifier::load(const std::string &model_spec,
                                   const std::string &firmware,
                                   const std::string &model_dir,
                                   std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool FaceAttributeClassifier::run(const std::string &image_path,
                                  const InferOptions &options,
                                  AlgorithmResult *result,
                                  std::string *error) {
  return classify(image_path, options, result, error);
}

bool FaceAttributeClassifier::run(const std::string &image_path, const Box &roi,
                                  const InferOptions &options,
                                  AlgorithmResult *result,
                                  std::string *error) {
  return classifyCrop(image_path, roi, options, result, error);
}

bool FaceAttributeClassifier::classify(const std::string &image_path,
                                       const InferOptions &options,
                                       AlgorithmResult *result,
                                       std::string *error) {
  if (!model_) {
    if (error) {
      *error = "face attribute classifier is not initialized";
    }
    return false;
  }
  return model_->predict(image_path, options, result, error);
}

bool FaceAttributeClassifier::classifyCrop(const std::string &image_path,
                                           const Box &roi,
                                           const InferOptions &options,
                                           AlgorithmResult *result,
                                           std::string *error) {
  if (!model_) {
    if (error) {
      *error = "face attribute classifier is not initialized";
    }
    return false;
  }
  return model_->predictCrop(image_path, roi, options, result, error);
}

bool FaceAttributeClassifier::initialized() const {
  return model_ && model_->initialized();
}

std::string FaceAttributeClassifier::modelType() const {
  return requested_model_type_;
}

void FaceAttributeClassifier::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
}

}  // namespace tdl_app
