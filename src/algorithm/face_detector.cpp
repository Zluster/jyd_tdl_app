#include "tdl_app/face_detector.hpp"

#include <utility>

#include "tdl_app/nn_base.hpp"
#include "algorithm/private/runtime_factory.hpp"

namespace tdl_app {
FaceDetector::FaceDetector() = default;

FaceDetector::FaceDetector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

FaceDetector::~FaceDetector() = default;

FaceDetector::FaceDetector(FaceDetector &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      model_(std::move(other.model_)) {}

FaceDetector &FaceDetector::operator=(FaceDetector &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  requested_model_type_ = std::move(other.requested_model_type_);
  config_ = std::move(other.config_);
  model_ = std::move(other.model_);
  return *this;
}

bool FaceDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = private_runtime_factory::resolveModelType(
      config_, requested_model_type_, "SCRFD");
  const std::string runtime_name = private_runtime_factory::inferRuntime(
      TaskType::FaceDetection, requested_model_type_, nullptr);
  model_ = private_runtime_factory::createRuntime(runtime_name,
                                                  requested_model_type_, error);
  if (!model_) {
    return false;
  }
  if (!model_->load(private_runtime_factory::toEngineConfig(config_), error)) {
    model_.reset();
    return false;
  }
  requested_model_type_ = model_->modelType();
  return true;
}

bool FaceDetector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool FaceDetector::load(const std::string &model_spec,
                        const std::string &firmware, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool FaceDetector::load(const std::string &model_spec,
                        const std::string &firmware,
                        const std::string &model_dir, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool FaceDetector::run(const std::string &image_path,
                       const InferOptions &options, AlgorithmResult *result,
                       std::string *error) {
  return detect(image_path, options, result, error);
}

bool FaceDetector::run(const std::string &image_path, float threshold,
                       AlgorithmResult *result, std::string *error) {
  InferOptions options;
  options.threshold = threshold;
  return detect(image_path, options, result, error);
}

bool FaceDetector::detect(const std::string &image_path,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "face detector is not initialized";
    }
    return false;
  }
  return model_->predict(image_path, options, result, error);
}

bool FaceDetector::initialized() const {
  return model_ && model_->initialized();
}

std::string FaceDetector::modelType() const { return requested_model_type_; }

void FaceDetector::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
}

}  // namespace tdl_app
