#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tdl_app {

struct Frame;
class FrameSource;

enum class TaskType {
  Classification,
  Detection,
  FaceDetection,
  FaceAttribute,
  Landmark,
  Keypoint,
  Ocr,
  Feature,
};

struct EngineConfig {
  std::string model_dir;
  std::string model_descriptor_file;
  std::string bmrt_firmware;
};

struct ModelSessionConfig {
  std::string model_spec;
  std::string model_type;
  std::string model_dir;
  std::string firmware;

  static ModelSessionConfig fromSpec(
      const std::string &spec, const std::string &firmware_path = std::string(),
      const std::string &models = std::string(),
      const std::string &type = std::string()) {
    ModelSessionConfig config;
    config.model_spec = spec;
    config.firmware = firmware_path;
    config.model_dir = models;
    config.model_type = type;
    return config;
  }
};

struct InferOptions {
  float threshold = 0.5f;
  float iou_threshold = 0.45f;
  int top_k = 5;
};

struct Point {
  float x = 0.0f;
  float y = 0.0f;
  float score = 0.0f;
};

struct Box {
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
  float score = 0.0f;
  int class_id = -1;
  std::vector<Point> landmarks;
};

struct ClassificationItem {
  int class_id = -1;
  float score = 0.0f;
};

struct Attribute {
  std::string name;
  float value = 0.0f;
};

struct AlgorithmResult {
  std::vector<ClassificationItem> classes;
  std::vector<Box> boxes;
  std::vector<Point> points;
  std::vector<Attribute> attributes;
  std::vector<float> feature;
  std::vector<std::string> labels;
  std::string text;
};

class AlgorithmEngine {
 public:
  AlgorithmEngine();
  ~AlgorithmEngine();

  AlgorithmEngine(const AlgorithmEngine &) = delete;
  AlgorithmEngine &operator=(const AlgorithmEngine &) = delete;

  bool initialize(const EngineConfig &config, std::string *error = nullptr);

  bool runImageFile(TaskType task, const std::string &model_type,
                    const std::string &image_path, float threshold,
                    AlgorithmResult *result, std::string *error = nullptr);

  bool runFrame(TaskType task, const std::string &model_type,
                const Frame &frame, float threshold,
                AlgorithmResult *result, std::string *error = nullptr);

  bool runFrameSource(TaskType task, const std::string &model_type,
                      FrameSource *source, float threshold,
                      AlgorithmResult *result,
                      std::string *error = nullptr);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

const char *taskTypeName(TaskType task);
TaskType taskTypeFromName(const std::string &name);

}  // namespace tdl_app
