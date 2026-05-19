#include "tdl_app/detector.hpp"

#include <memory>
#include <utility>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_yolov5.hpp"
#include "tdl_app/nn_yolov8.hpp"

namespace tdl_app {
namespace {

EngineConfig toEngineConfig(const Detector::Config &config) {
  EngineConfig out;
  out.model_descriptor_file = config.model_spec;
  out.model_dir = config.model_dir;
  out.bmrt_firmware = config.firmware;
  return out;
}

std::string resolveModelType(const Detector::Config &config) {
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
  return "YOLOV5";
}

}  // namespace

Detector::Detector() = default;

Detector::Detector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

Detector::Detector(const Config &config, std::string *error) {
  load(config, error);
}

Detector::Detector(std::string model_type, const Config &config,
                   std::string *error)
    : requested_model_type_(std::move(model_type)) {
  load(config, error);
}

Detector::~Detector() = default;

Detector::Detector(Detector &&) noexcept = default;

Detector &Detector::operator=(Detector &&) noexcept = default;

bool Detector::load(const Config &config, std::string *error) {
  config_ = config;
  if (config_.model_type.empty() && !requested_model_type_.empty()) {
    config_.model_type = requested_model_type_;
  }
  requested_model_type_ = resolveModelType(config_);
  if (requested_model_type_.rfind("YOLOV8", 0) == 0) {
    model_.reset(new NnYolov8(requested_model_type_));
  } else {
    model_.reset(new NnYolov5(requested_model_type_));
  }
  const bool ok = model_->load(toEngineConfig(config_), error);
  if (!ok) {
    last_error_ = error ? *error : "detector load failed";
    model_.reset();
    return false;
  }
  last_error_.clear();
  return true;
}

bool Detector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool Detector::load(const std::string &model_spec, const std::string &firmware,
                    std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool Detector::load(const std::string &model_spec, const std::string &firmware,
                    const std::string &model_dir, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool Detector::run(const std::string &image_path, const InferOptions &options,
                   AlgorithmResult *result, std::string *error) {
  return detect(image_path, options, result, error);
}

bool Detector::run(const std::string &image_path, float threshold,
                   AlgorithmResult *result, std::string *error) {
  return detect(image_path, threshold, result, error);
}

bool Detector::run(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
                   AlgorithmResult *result, std::string *error) {
  return detect(frame, options, result, error);
}

bool Detector::runFrame(const Frame &frame, const InferOptions &options,
                        AlgorithmResult *result, std::string *error) {
  return detectFrame(frame, options, result, error);
}

bool Detector::runFrame(const VIDEO_FRAME_INFO_S &frame,
                        const InferOptions &options, AlgorithmResult *result,
                        std::string *error) {
  return detectFrame(frame, options, result, error);
}

bool Detector::detect(const std::string &image_path, const InferOptions &options,
                      AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "detector is not initialized";
    }
    last_error_ = "detector is not initialized";
    return false;
  }
  const bool ok = model_->predict(image_path, options, result, error);
  if (!ok) {
    last_error_ = error ? *error : "detector predict failed";
    return false;
  }
  last_error_.clear();
  return true;
}

bool Detector::detect(const std::string &image_path, float threshold,
                      AlgorithmResult *result, std::string *error) {
  InferOptions options;
  options.threshold = threshold;
  return detect(image_path, options, result, error);
}

bool Detector::detect(const VIDEO_FRAME_INFO_S &frame,
                      const InferOptions &options, AlgorithmResult *result,
                      std::string *error) {
  if (!model_) {
    if (error) {
      *error = "detector is not initialized";
    }
    last_error_ = "detector is not initialized";
    return false;
  }

  Frame wrapped;
  wrapped.native = const_cast<VIDEO_FRAME_INFO_S *>(&frame);
  wrapped.width = static_cast<int>(frame.stVFrame.u32Width);
  wrapped.height = static_cast<int>(frame.stVFrame.u32Height);
  wrapped.format = static_cast<int>(frame.stVFrame.enPixelFormat);
  wrapped.sequence = frame.stVFrame.u32TimeRef;
  wrapped.timestamp_us = frame.stVFrame.u64PTS;
  return detectFrame(wrapped, options, result, error);
}

bool Detector::detectFrame(const Frame &frame, const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  if (!model_) {
    if (error) {
      *error = "detector is not initialized";
    }
    last_error_ = "detector is not initialized";
    return false;
  }
  const bool ok = model_->predictFrame(frame, options, result, error);
  if (!ok) {
    last_error_ = error ? *error : "detector frame predict failed";
    return false;
  }
  last_error_.clear();
  return true;
}

bool Detector::detectFrame(const VIDEO_FRAME_INFO_S &frame,
                           const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  return detect(frame, options, result, error);
}

AlgorithmResult Detector::detect(const std::string &image_path,
                                 const InferOptions &options,
                                 std::string *error) {
  AlgorithmResult result;
  detect(image_path, options, &result, error);
  return result;
}

AlgorithmResult Detector::detect(const VIDEO_FRAME_INFO_S &frame,
                                 const InferOptions &options,
                                 std::string *error) {
  AlgorithmResult result;
  detect(frame, options, &result, error);
  return result;
}

AlgorithmResult Detector::detectFrame(const Frame &frame,
                                      const InferOptions &options,
                                      std::string *error) {
  AlgorithmResult result;
  detectFrame(frame, options, &result, error);
  return result;
}

AlgorithmResult Detector::detectFrame(const VIDEO_FRAME_INFO_S &frame,
                                      const InferOptions &options,
                                      std::string *error) {
  AlgorithmResult result;
  detectFrame(frame, options, &result, error);
  return result;
}

bool Detector::operator()(const std::string &image_path,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return detect(image_path, options, result, error);
}

bool Detector::operator()(const Frame &frame, const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return detectFrame(frame, options, result, error);
}

bool Detector::operator()(const VIDEO_FRAME_INFO_S &frame,
                          const InferOptions &options,
                          AlgorithmResult *result, std::string *error) {
  return detect(frame, options, result, error);
}

AlgorithmResult Detector::operator()(const std::string &image_path,
                                     const InferOptions &options,
                                     std::string *error) {
  return detect(image_path, options, error);
}

AlgorithmResult Detector::operator()(const Frame &frame,
                                     const InferOptions &options,
                                     std::string *error) {
  return detectFrame(frame, options, error);
}

AlgorithmResult Detector::operator()(const VIDEO_FRAME_INFO_S &frame,
                                     const InferOptions &options,
                                     std::string *error) {
  return detect(frame, options, error);
}

bool Detector::initialized() const {
  return model_ && model_->initialized();
}

std::string Detector::modelType() const {
  return requested_model_type_;
}

void Detector::reset() {
  model_.reset();
  config_ = Config{};
  requested_model_type_.clear();
  last_error_.clear();
}

}  // namespace tdl_app
