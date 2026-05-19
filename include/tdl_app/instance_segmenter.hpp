#pragma once

#include <string>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/vision_task_types.hpp"

namespace tdl_app {

class InstanceSegmenter {
 public:
  using Config = ModelSessionConfig;

  InstanceSegmenter();
  explicit InstanceSegmenter(std::string model_type);
  ~InstanceSegmenter();

  static InstanceSegmenter yolov8() {
    return InstanceSegmenter("YOLOV8_SEG_COCO80");
  }

  InstanceSegmenter(const InstanceSegmenter &) = delete;
  InstanceSegmenter &operator=(const InstanceSegmenter &) = delete;
  InstanceSegmenter(InstanceSegmenter &&) noexcept;
  InstanceSegmenter &operator=(InstanceSegmenter &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, InstanceSegmentationResult *result,
           std::string *error = nullptr);
  bool runFrame(const Frame &frame, InstanceSegmentationResult *result,
                std::string *error = nullptr);
  bool segment(const std::string &image_path,
               InstanceSegmentationResult *result,
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
