#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

struct SingleObjectTrackingResult {
  Box box;
  float confidence = 0.0f;
  int search_width = 0;
  int search_height = 0;

  void clear() {
    box = Box{};
    confidence = 0.0f;
    search_width = 0;
    search_height = 0;
  }

  bool valid() const { return box.valid(); }
};

class SingleObjectTracker {
 public:
  using Config = ModelSessionConfig;

  SingleObjectTracker();
  explicit SingleObjectTracker(std::string model_type);
  ~SingleObjectTracker();

  static SingleObjectTracker fearTrack() {
    return SingleObjectTracker("TRACKING_FEARTRACK");
  }

  SingleObjectTracker(const SingleObjectTracker &) = delete;
  SingleObjectTracker &operator=(const SingleObjectTracker &) = delete;
  SingleObjectTracker(SingleObjectTracker &&other) noexcept;
  SingleObjectTracker &operator=(SingleObjectTracker &&other) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool initialize(const std::string &image_path, const Box &target,
                  std::string *error = nullptr);
  bool initializeFrame(const Frame &frame, const Box &target,
                       std::string *error = nullptr);

  bool run(const std::string &image_path, SingleObjectTrackingResult *result,
           std::string *error = nullptr);
  bool runFrame(const Frame &frame, SingleObjectTrackingResult *result,
                std::string *error = nullptr);

  bool initialized() const;
  bool ready() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  const std::string &lastError() const { return last_error_; }
  Box currentBox() const;
  void reset();

 private:
  class Impl;

  std::string requested_model_type_;
  Config config_;
  std::string last_error_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
