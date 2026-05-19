#pragma once

#include <string>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/vision_task_types.hpp"

namespace tdl_app {

class LaneDetector {
 public:
  using Config = ModelSessionConfig;

  LaneDetector();
  explicit LaneDetector(std::string model_type);
  ~LaneDetector();

  static LaneDetector lstr() { return LaneDetector("LSTR_DET_LANE"); }

  LaneDetector(const LaneDetector &) = delete;
  LaneDetector &operator=(const LaneDetector &) = delete;
  LaneDetector(LaneDetector &&) noexcept;
  LaneDetector &operator=(LaneDetector &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error = nullptr);
  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error = nullptr);
  bool detect(const std::string &image_path, LaneDetectionResult *result,
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
