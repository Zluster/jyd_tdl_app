#include "tdl_app/classifier.hpp"

#include <memory>
#include <utility>

#include "tdl_app/nn_base.hpp"
#include "algorithm/private/runtime_factory.hpp"

namespace tdl_app {
namespace {

}  // namespace

Classifier::Classifier() = default;

Classifier::Classifier(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

Classifier::~Classifier() = default;

Classifier::Classifier(Classifier &&) noexcept = default;

Classifier &Classifier::operator=(Classifier &&) noexcept = default;

bool Classifier::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = private_runtime_factory::resolveModelType(
      config_, requested_model_type_, "CLASSIFIER");
  const std::string runtime_name = private_runtime_factory::inferRuntime(
      TaskType::Classification, requested_model_type_, nullptr);
  model_ = private_runtime_factory::createRuntime(runtime_name,
                                                  requested_model_type_, error);
  if (!model_) {
    return false;
  }
  return model_->load(private_runtime_factory::toEngineConfig(config_), error);
}

bool Classifier::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool Classifier::load(const std::string &model_spec, const std::string &firmware,
                      std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool Classifier::load(const std::string &model_spec, const std::string &firmware,
                      const std::string &model_dir, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool Classifier::run(const std::string &image_path, const InferOptions &options,
                     AlgorithmResult *result, std::string *error) {
  return classify(image_path, options, result, error);
}

bool Classifier::run(const std::string &image_path, float threshold,
                     AlgorithmResult *result, std::string *error) {
  return classify(image_path, threshold, result, error);
}

bool Classifier::runFrame(const Frame &frame, const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return classifyFrame(frame, options, result, error);
}

bool Classifier::classify(const std::string &image_path,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "classifier is not initialized";
    }
    return false;
  }
  return model_->predict(image_path, options, result, error);
}

bool Classifier::classify(const std::string &image_path, float threshold,
                          AlgorithmResult *result, std::string *error) {
  InferOptions options;
  options.threshold = threshold;
  return classify(image_path, options, result, error);
}

bool Classifier::classifyFrame(const Frame &frame, const InferOptions &options,
                               AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "classifier is not initialized";
    }
    return false;
  }
  return model_->predictFrame(frame, options, result, error);
}

bool Classifier::initialized() const {
  return model_ && model_->initialized();
}

std::string Classifier::modelType() const {
  return requested_model_type_;
}

void Classifier::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
}

}  // namespace tdl_app
