#include "tdl_app/feature_extractor.hpp"

#include <memory>
#include <utility>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_feature.hpp"

namespace tdl_app {
namespace {

EngineConfig toEngineConfig(const FeatureExtractor::Config &config) {
  EngineConfig out;
  out.model_descriptor_file = config.model_spec;
  out.model_dir = config.model_dir;
  out.bmrt_firmware = config.firmware;
  return out;
}

std::string resolveModelType(const FeatureExtractor::Config &config) {
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
  return "FEATURE";
}

}  // namespace

FeatureExtractor::FeatureExtractor() = default;

FeatureExtractor::FeatureExtractor(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

FeatureExtractor::~FeatureExtractor() = default;

FeatureExtractor::FeatureExtractor(FeatureExtractor &&) noexcept = default;

FeatureExtractor &FeatureExtractor::operator=(FeatureExtractor &&) noexcept = default;

bool FeatureExtractor::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = resolveModelType(config_);
  model_.reset(new NnFeature(requested_model_type_));
  return model_->load(toEngineConfig(config_), error);
}

bool FeatureExtractor::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool FeatureExtractor::load(const std::string &model_spec,
                            const std::string &firmware,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool FeatureExtractor::load(const std::string &model_spec,
                            const std::string &firmware,
                            const std::string &model_dir,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool FeatureExtractor::run(const std::string &image_path,
                           const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  return extract(image_path, options, result, error);
}

bool FeatureExtractor::run(const std::string &image_path, float threshold,
                           AlgorithmResult *result, std::string *error) {
  return extract(image_path, threshold, result, error);
}

bool FeatureExtractor::runFrame(const Frame &frame, const InferOptions &options,
                                AlgorithmResult *result, std::string *error) {
  return extractFrame(frame, options, result, error);
}

bool FeatureExtractor::extract(const std::string &image_path,
                               const InferOptions &options,
                               AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "feature extractor is not initialized";
    }
    return false;
  }
  return model_->predict(image_path, options, result, error);
}

bool FeatureExtractor::extract(const std::string &image_path, float threshold,
                               AlgorithmResult *result, std::string *error) {
  InferOptions options;
  options.threshold = threshold;
  return extract(image_path, options, result, error);
}

bool FeatureExtractor::extractFrame(const Frame &frame, const InferOptions &options,
                                    AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "feature extractor is not initialized";
    }
    return false;
  }
  return model_->predictFrame(frame, options, result, error);
}

bool FeatureExtractor::initialized() const {
  return model_ && model_->initialized();
}

std::string FeatureExtractor::modelType() const {
  return requested_model_type_;
}

void FeatureExtractor::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
}

}  // namespace tdl_app
