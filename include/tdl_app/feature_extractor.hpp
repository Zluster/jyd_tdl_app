#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnBase;

class FeatureExtractor {
 public:
  using Config = ModelSessionConfig;

  FeatureExtractor();
  explicit FeatureExtractor(std::string model_type);
  ~FeatureExtractor();

  static FeatureExtractor generic() { return FeatureExtractor("FEATURE"); }

  FeatureExtractor(const FeatureExtractor &) = delete;
  FeatureExtractor &operator=(const FeatureExtractor &) = delete;
  FeatureExtractor(FeatureExtractor &&) noexcept;
  FeatureExtractor &operator=(FeatureExtractor &&) noexcept;

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
  bool runFrame(const Frame &frame, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool extract(const std::string &image_path, const InferOptions &options,
               AlgorithmResult *result, std::string *error = nullptr);
  bool extract(const std::string &image_path, float threshold,
               AlgorithmResult *result, std::string *error = nullptr);
  bool extractFrame(const Frame &frame, const InferOptions &options,
                    AlgorithmResult *result, std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  friend class Pipeline;

  std::string requested_model_type_;
  Config config_;
  std::shared_ptr<NnBase> model_;
};

}  // namespace tdl_app
