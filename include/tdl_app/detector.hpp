#pragma once

#include <memory>
#include <string>

#include "cvi_comm_video.h"
#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnBase;
class MultiStagePipeline;
class Pipeline;

class Detector {
 public:
  using Config = ModelSessionConfig;

  Detector();
  explicit Detector(std::string model_type);
  explicit Detector(const Config &config, std::string *error = nullptr);
  Detector(std::string model_type, const Config &config,
           std::string *error = nullptr);
  ~Detector();

  static Detector yolov5() { return Detector("YOLOV5"); }
  static Detector yolov8() { return Detector("YOLOV8"); }

  Detector(const Detector &) = delete;
  Detector &operator=(const Detector &) = delete;
  Detector(Detector &&) noexcept;
  Detector &operator=(Detector &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec,
            std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);
  bool run(const std::string &image_path, const InferOptions &options,
           AlgorithmResult *result, std::string *error = nullptr);
  bool run(const std::string &image_path, float threshold,
           AlgorithmResult *result, std::string *error = nullptr);
  bool run(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
           AlgorithmResult *result, std::string *error = nullptr);
  bool runFrame(const Frame &frame, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool runFrame(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool detect(const std::string &image_path, const InferOptions &options,
              AlgorithmResult *result, std::string *error = nullptr);
  bool detect(const std::string &image_path, float threshold,
              AlgorithmResult *result, std::string *error = nullptr);
  bool detect(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
              AlgorithmResult *result, std::string *error = nullptr);
  bool detectFrame(const Frame &frame, const InferOptions &options,
                   AlgorithmResult *result, std::string *error = nullptr);
  bool detectFrame(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
                   AlgorithmResult *result, std::string *error = nullptr);

  AlgorithmResult detect(const std::string &image_path,
                         const InferOptions &options,
                         std::string *error = nullptr);
  AlgorithmResult detect(const VIDEO_FRAME_INFO_S &frame,
                         const InferOptions &options,
                         std::string *error = nullptr);
  AlgorithmResult detectFrame(const Frame &frame, const InferOptions &options,
                              std::string *error = nullptr);
  AlgorithmResult detectFrame(const VIDEO_FRAME_INFO_S &frame,
                              const InferOptions &options,
                              std::string *error = nullptr);

  bool operator()(const std::string &image_path, const InferOptions &options,
                  AlgorithmResult *result, std::string *error = nullptr);
  bool operator()(const Frame &frame, const InferOptions &options,
                  AlgorithmResult *result, std::string *error = nullptr);
  bool operator()(const VIDEO_FRAME_INFO_S &frame, const InferOptions &options,
                  AlgorithmResult *result, std::string *error = nullptr);
  AlgorithmResult operator()(const std::string &image_path,
                             const InferOptions &options,
                             std::string *error = nullptr);
  AlgorithmResult operator()(const Frame &frame, const InferOptions &options,
                             std::string *error = nullptr);
  AlgorithmResult operator()(const VIDEO_FRAME_INFO_S &frame,
                             const InferOptions &options,
                             std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  const std::string &lastError() const { return last_error_; }
  void reset();

 private:
  friend class Pipeline;
  friend class MultiStagePipeline;

  std::string requested_model_type_;
  Config config_;
  std::string last_error_;
  std::shared_ptr<NnBase> model_;
};

}  // namespace tdl_app
