#pragma once

#include <string>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/vision_task_types.hpp"

namespace tdl_app {

class KeypointDetector {
 public:
  using Config = ModelSessionConfig;

  KeypointDetector();
  explicit KeypointDetector(std::string model_type);
  ~KeypointDetector();

  static KeypointDetector hand() { return KeypointDetector("KEYPOINT_HAND"); }
  static KeypointDetector person17() {
    return KeypointDetector("KEYPOINT_YOLOV8POSE_PERSON17");
  }

  KeypointDetector(const KeypointDetector &) = delete;
  KeypointDetector &operator=(const KeypointDetector &) = delete;
  KeypointDetector(KeypointDetector &&) noexcept;
  KeypointDetector &operator=(KeypointDetector &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error = nullptr);
  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error = nullptr);
  bool estimate(const std::string &image_path, KeypointResult *result,
                std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  class Impl;

  std::string requested_model_type_;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
