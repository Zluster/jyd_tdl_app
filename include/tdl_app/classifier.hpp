#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnBase;

class Classifier {
 public:
  using Config = ModelSessionConfig;

  Classifier();
  explicit Classifier(std::string model_type);
  ~Classifier();

  static Classifier generic() { return Classifier("CLASSIFIER"); }

  Classifier(const Classifier &) = delete;
  Classifier &operator=(const Classifier &) = delete;
  Classifier(Classifier &&) noexcept;
  Classifier &operator=(Classifier &&) noexcept;

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
  bool classify(const std::string &image_path, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool classify(const std::string &image_path, float threshold,
                AlgorithmResult *result, std::string *error = nullptr);
  bool classifyFrame(const Frame &frame, const InferOptions &options,
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
