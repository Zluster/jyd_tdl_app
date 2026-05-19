#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnScrfd;
class MultiStagePipeline;
class Pipeline;

class FaceDetector {
 public:
  using Config = ModelSessionConfig;

  FaceDetector();
  explicit FaceDetector(std::string model_type);
  ~FaceDetector();

  static FaceDetector scrfd() { return FaceDetector("SCRFD"); }

  FaceDetector(const FaceDetector &) = delete;
  FaceDetector &operator=(const FaceDetector &) = delete;
  FaceDetector(FaceDetector &&) noexcept;
  FaceDetector &operator=(FaceDetector &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, const InferOptions &options,
           AlgorithmResult *result, std::string *error = nullptr);
  bool run(const std::string &image_path, float threshold,
           AlgorithmResult *result, std::string *error = nullptr);
  bool detect(const std::string &image_path, const InferOptions &options,
              AlgorithmResult *result, std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  friend class Pipeline;
  friend class MultiStagePipeline;

  std::string requested_model_type_;
  Config config_;
  std::shared_ptr<NnScrfd> model_;
};

}  // namespace tdl_app
