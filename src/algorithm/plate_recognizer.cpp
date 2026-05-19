#include "tdl_app/plate_recognizer.hpp"

#include <memory>
#include <utility>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_plate_recognizer.hpp"

namespace tdl_app {
namespace {

EngineConfig toEngineConfig(const PlateRecognizer::Config &config) {
  EngineConfig out;
  out.model_descriptor_file = config.model_spec;
  out.model_dir = config.model_dir;
  out.bmrt_firmware = config.firmware;
  return out;
}

std::string resolveModelType(const PlateRecognizer::Config &config) {
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
  return "PLATE_RECOGNIZER";
}

}  // namespace

PlateRecognizer::PlateRecognizer() = default;

PlateRecognizer::PlateRecognizer(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

PlateRecognizer::~PlateRecognizer() = default;

PlateRecognizer::PlateRecognizer(PlateRecognizer &&) noexcept = default;

PlateRecognizer &PlateRecognizer::operator=(PlateRecognizer &&) noexcept =
    default;

bool PlateRecognizer::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = resolveModelType(config_);
  model_.reset(new NnPlateRecognizer(requested_model_type_));
  return model_->load(toEngineConfig(config_), error);
}

bool PlateRecognizer::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool PlateRecognizer::load(const std::string &model_spec,
                           const std::string &firmware, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool PlateRecognizer::load(const std::string &model_spec,
                           const std::string &firmware,
                           const std::string &model_dir, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool PlateRecognizer::run(const std::string &image_path,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return recognize(image_path, options, result, error);
}

bool PlateRecognizer::run(const std::string &image_path, const Box &roi,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return recognizeCrop(image_path, roi, options, result, error);
}

bool PlateRecognizer::recognize(const std::string &image_path,
                                const InferOptions &options,
                                AlgorithmResult *result,
                                std::string *error) {
  if (!model_) {
    if (error) {
      *error = "plate recognizer is not initialized";
    }
    return false;
  }
  return model_->predict(image_path, options, result, error);
}

bool PlateRecognizer::recognizeCrop(const std::string &image_path, const Box &roi,
                                    const InferOptions &options,
                                    AlgorithmResult *result,
                                    std::string *error) {
  if (!model_) {
    if (error) {
      *error = "plate recognizer is not initialized";
    }
    return false;
  }
  return model_->predictCrop(image_path, roi, options, result, error);
}

bool PlateRecognizer::initialized() const {
  return model_ && model_->initialized();
}

std::string PlateRecognizer::modelType() const { return requested_model_type_; }

void PlateRecognizer::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
}

}  // namespace tdl_app
