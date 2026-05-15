#include "tdl_app/algorithm_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

#include "tdl_app/frame_source.hpp"
#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_classifier.hpp"
#include "tdl_app/nn_feature.hpp"
#include "tdl_app/nn_yolov5.hpp"
#include "tdl_app/nn_yolov8.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string normalizeToken(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::string defaultModelForTask(TaskType task) {
  switch (task) {
    case TaskType::Classification:
      return "CLASSIFIER";
    case TaskType::Detection:
    case TaskType::FaceDetection:
      return "YOLOV8";
    case TaskType::Feature:
      return "FEATURE";
    case TaskType::FaceAttribute:
    case TaskType::Landmark:
    case TaskType::Keypoint:
    case TaskType::Ocr:
      return "";
  }
  return "";
}

std::string inferRuntime(TaskType task, const std::string &model_name,
                         const ModelDescriptor *descriptor) {
  if (descriptor && !descriptor->runtime.empty()) {
    return normalizeToken(descriptor->runtime);
  }
  if (descriptor && !descriptor->task_name.empty()) {
    const std::string task_name = normalizeToken(descriptor->task_name);
    if (task_name == "DETECT" || task_name == "DETECTION") {
      return startsWith(normalizeToken(model_name), "YOLOV5") ? "YOLOV5" : "YOLOV8";
    }
    if (task_name == "CLASSIFY" || task_name == "CLASSIFICATION") {
      return "CLASSIFIER";
    }
    if (task_name == "FEATURE") {
      return "FEATURE";
    }
  }
  const std::string normalized = normalizeToken(model_name);
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
    case TaskType::FaceAttribute:
      return "CLASSIFIER";
    case TaskType::Detection:
    case TaskType::FaceDetection:
      return "YOLOV8";
    case TaskType::Feature:
      return "FEATURE";
    case TaskType::Landmark:
    case TaskType::Keypoint:
    case TaskType::Ocr:
      return "";
  }
  return "";
}

}  // namespace

class AlgorithmEngine::Impl {
 public:
  bool initialize(const EngineConfig &config, std::string *error) {
    config_ = config;
    if (!config.bmrt_firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.bmrt_firmware.c_str(), 0);
    }

    descriptor_loaded_ = false;
    descriptor_ = ModelDescriptor{};
    if (!config.model_descriptor_file.empty()) {
      if (!loadModelDescriptor(config.model_descriptor_file, &descriptor_, error)) {
        return false;
      }
      descriptor_loaded_ = true;
    }

    initialized_ = true;
    return true;
  }

  bool runImageFile(TaskType task, const std::string &model_type,
                    const std::string &image_path, float threshold,
                    AlgorithmResult *result, std::string *error) {
    Frame frame;
    frame.image_path = image_path;
    return runFrame(task, model_type, frame, threshold, result, error);
  }

  bool runFrame(TaskType task, const std::string &model_type, const Frame &frame,
                float threshold, AlgorithmResult *result, std::string *error) {
    if (!initialized_) {
      setError(error, "engine is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }

    std::string resolved_model_type = model_type;
    if (resolved_model_type.empty() && descriptor_loaded_) {
      resolved_model_type = descriptor_.model_type;
    }
    if (resolved_model_type.empty()) {
      resolved_model_type = defaultModelForTask(task);
    }
    resolved_model_type = normalizeToken(resolved_model_type);

    const std::string runtime =
        inferRuntime(task, resolved_model_type, descriptor_loaded_ ? &descriptor_ : nullptr);
    if (runtime.empty()) {
      setError(error, "unsupported task/model combination for custom runtime");
      return false;
    }

    if (!ensureRuntime(runtime, resolved_model_type, error)) {
      return false;
    }

    InferOptions options;
    options.threshold = threshold;

    *result = AlgorithmResult{};
    if (!runtime_model_->predictFrame(frame, options, result, error)) {
      return false;
    }
    if (result->labels.empty() && descriptor_loaded_) {
      result->labels = descriptor_.labels;
    }
    return true;
  }

  bool runFrameSource(TaskType task, const std::string &model_type,
                      FrameSource *source, float threshold,
                      AlgorithmResult *result, std::string *error) {
    if (!source) {
      setError(error, "frame source pointer is null");
      return false;
    }
    if (!source->open(error)) {
      return false;
    }

    Frame frame;
    const bool ok = source->read(&frame, error);
    source->close();
    if (!ok) {
      return false;
    }
    return runFrame(task, model_type, frame, threshold, result, error);
  }

 private:
  bool ensureRuntime(const std::string &runtime_name,
                     const std::string &model_type,
                     std::string *error) {
    const std::string runtime_key =
        runtime_name + "|" + model_type + "|" + config_.model_descriptor_file;
    if (runtime_model_ && runtime_key_ == runtime_key && runtime_model_->initialized()) {
      return true;
    }

    std::shared_ptr<NnBase> next_runtime;
    if (runtime_name == "YOLOV8") {
      next_runtime.reset(new NnYolov8(model_type));
    } else if (runtime_name == "YOLOV5") {
      next_runtime.reset(new NnYolov5(model_type));
    } else if (runtime_name == "CLASSIFIER") {
      next_runtime.reset(new NnClassifier(model_type));
    } else if (runtime_name == "FEATURE") {
      next_runtime.reset(new NnFeature(model_type));
    } else {
      setError(error, "unsupported runtime: " + runtime_name);
      return false;
    }

    if (!next_runtime->load(config_, error)) {
      return false;
    }
    runtime_model_ = std::move(next_runtime);
    runtime_key_ = runtime_key;
    return true;
  }

  EngineConfig config_;
  ModelDescriptor descriptor_;
  bool descriptor_loaded_ = false;
  bool initialized_ = false;
  std::string runtime_key_;
  std::shared_ptr<NnBase> runtime_model_;
};

AlgorithmEngine::AlgorithmEngine() : impl_(new Impl) {}

AlgorithmEngine::~AlgorithmEngine() = default;

bool AlgorithmEngine::initialize(const EngineConfig &config,
                                 std::string *error) {
  return impl_->initialize(config, error);
}

bool AlgorithmEngine::runImageFile(TaskType task, const std::string &model_type,
                                   const std::string &image_path,
                                   float threshold, AlgorithmResult *result,
                                   std::string *error) {
  return impl_->runImageFile(task, model_type, image_path, threshold, result,
                             error);
}

bool AlgorithmEngine::runFrame(TaskType task, const std::string &model_type,
                               const Frame &frame, float threshold,
                               AlgorithmResult *result,
                               std::string *error) {
  return impl_->runFrame(task, model_type, frame, threshold, result, error);
}

bool AlgorithmEngine::runFrameSource(TaskType task,
                                     const std::string &model_type,
                                     FrameSource *source, float threshold,
                                     AlgorithmResult *result,
                                     std::string *error) {
  return impl_->runFrameSource(task, model_type, source, threshold, result,
                               error);
}

const char *taskTypeName(TaskType task) {
  switch (task) {
    case TaskType::Classification:
      return "classify";
    case TaskType::Detection:
      return "detect";
    case TaskType::FaceDetection:
      return "face-detect";
    case TaskType::FaceAttribute:
      return "face-attr";
    case TaskType::Landmark:
      return "landmark";
    case TaskType::Keypoint:
      return "keypoint";
    case TaskType::Ocr:
      return "ocr";
    case TaskType::Feature:
      return "feature";
  }
  return "unknown";
}

TaskType taskTypeFromName(const std::string &name) {
  if (name == "classify") return TaskType::Classification;
  if (name == "detect") return TaskType::Detection;
  if (name == "face-detect") return TaskType::FaceDetection;
  if (name == "face-attr") return TaskType::FaceAttribute;
  if (name == "landmark") return TaskType::Landmark;
  if (name == "keypoint") return TaskType::Keypoint;
  if (name == "ocr") return TaskType::Ocr;
  if (name == "feature") return TaskType::Feature;
  return TaskType::Detection;
}

}  // namespace tdl_app
