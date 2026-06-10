#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_classifier.hpp"
#include "tdl_app/nn_face_attribute.hpp"
#include "tdl_app/nn_feature.hpp"
#include "tdl_app/nn_plate_recognizer.hpp"
#include "tdl_app/nn_scrfd.hpp"
#include "tdl_app/nn_yolov5.hpp"
#include "tdl_app/nn_yolov8.hpp"

namespace tdl_app {
namespace private_runtime_factory {

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

inline std::string normalizeToken(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

inline bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

inline EngineConfig toEngineConfig(const ModelSessionConfig &config) {
  EngineConfig out;
  out.model_descriptor_file = config.model_spec;
  out.model_dir = config.model_dir;
  out.bmrt_firmware = config.firmware;
  return out;
}

inline std::string resolveModelType(const ModelSessionConfig &config,
                                    const std::string &requested_model_type,
                                    const std::string &fallback_model_type) {
  if (!config.model_type.empty()) {
    return config.model_type;
  }
  if (!config.model_spec.empty()) {
    ModelDescriptor descriptor;
    std::string ignored_error;
    if (loadModelDescriptor(config.model_spec, &descriptor, &ignored_error)) {
      if (!descriptor.model_type.empty()) {
        return descriptor.model_type;
      }
      if (!descriptor.runtime.empty()) {
        return descriptor.runtime;
      }
    }
  }
  if (!requested_model_type.empty()) {
    return requested_model_type;
  }
  return fallback_model_type;
}

inline std::string inferRuntime(TaskType task, const std::string &model_name,
                                const ModelDescriptor *descriptor) {
  if (descriptor && !descriptor->runtime.empty()) {
    return normalizeToken(descriptor->runtime);
  }
  if (descriptor && !descriptor->task_name.empty()) {
    const std::string task_name = normalizeToken(descriptor->task_name);
    if (task_name == "DETECT" || task_name == "DETECTION") {
      return startsWith(normalizeToken(model_name), "YOLOV5") ? "YOLOV5"
                                                               : "YOLOV8";
    }
    if (task_name == "FACE_DETECT" || task_name == "FACE_DETECTION") {
      return "SCRFD";
    }
    if (task_name == "FACE_ATTR" || task_name == "FACE_ATTRIBUTE") {
      return "FACE_ATTRIBUTE";
    }
    if (task_name == "CLASSIFY" || task_name == "CLASSIFICATION") {
      return "CLASSIFIER";
    }
    if (task_name == "FEATURE") {
      return "FEATURE";
    }
    if (task_name == "OCR") {
      return "PLATE_RECOGNIZER";
    }
  }

  const std::string normalized = normalizeToken(model_name);
  if (startsWith(normalized, "SCRFD")) {
    return "SCRFD";
  }
  if (startsWith(normalized, "FACE_ATTRIBUTE")) {
    return "FACE_ATTRIBUTE";
  }
  if (startsWith(normalized, "PLATE_") || startsWith(normalized, "LPR")) {
    return "PLATE_RECOGNIZER";
  }
  if (startsWith(normalized, "YOLOV8")) {
    return "YOLOV8";
  }
  if (startsWith(normalized, "YOLOV5")) {
    return "YOLOV5";
  }
  if (startsWith(normalized, "FEATURE")) {
    return "FEATURE";
  }
  if (startsWith(normalized, "CLS") || startsWith(normalized, "CLASSIFIER")) {
    return "CLASSIFIER";
  }

  switch (task) {
    case TaskType::Classification:
      return "CLASSIFIER";
    case TaskType::Detection:
      return "YOLOV8";
    case TaskType::FaceDetection:
      return "SCRFD";
    case TaskType::FaceAttribute:
      return "FACE_ATTRIBUTE";
    case TaskType::Feature:
      return "FEATURE";
    case TaskType::Ocr:
      return "PLATE_RECOGNIZER";
    case TaskType::Landmark:
    case TaskType::Keypoint:
      return "";
  }
  return "";
}

inline std::shared_ptr<NnBase> createRuntime(const std::string &runtime_name,
                                             const std::string &model_type,
                                             std::string *error) {
  if (runtime_name == "YOLOV8") {
    return std::shared_ptr<NnBase>(new NnYolov8(model_type));
  }
  if (runtime_name == "YOLOV5") {
    return std::shared_ptr<NnBase>(new NnYolov5(model_type));
  }
  if (runtime_name == "SCRFD") {
    return std::shared_ptr<NnBase>(new NnScrfd(model_type));
  }
  if (runtime_name == "CLASSIFIER") {
    return std::shared_ptr<NnBase>(new NnClassifier(model_type));
  }
  if (runtime_name == "FACE_ATTRIBUTE") {
    return std::shared_ptr<NnBase>(new NnFaceAttribute(model_type));
  }
  if (runtime_name == "PLATE_RECOGNIZER") {
    return std::shared_ptr<NnBase>(new NnPlateRecognizer(model_type));
  }
  if (runtime_name == "FEATURE") {
    return std::shared_ptr<NnBase>(new NnFeature(model_type));
  }
  setError(error, "unsupported runtime: " + runtime_name);
  return std::shared_ptr<NnBase>();
}

}  // namespace private_runtime_factory
}  // namespace tdl_app
