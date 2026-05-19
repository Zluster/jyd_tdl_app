#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnFaceAttribute;
class MultiStagePipeline;
class Pipeline;

class FaceAttributeClassifier {
 public:
  using Config = ModelSessionConfig;

  FaceAttributeClassifier();
  explicit FaceAttributeClassifier(std::string model_type);
  ~FaceAttributeClassifier();

  static FaceAttributeClassifier generic() {
    return FaceAttributeClassifier("FACE_ATTRIBUTE");
  }

  FaceAttributeClassifier(const FaceAttributeClassifier &) = delete;
  FaceAttributeClassifier &operator=(const FaceAttributeClassifier &) = delete;
  FaceAttributeClassifier(FaceAttributeClassifier &&) noexcept;
  FaceAttributeClassifier &operator=(FaceAttributeClassifier &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, const InferOptions &options,
           AlgorithmResult *result, std::string *error = nullptr);
  bool run(const std::string &image_path, const Box &roi,
           const InferOptions &options, AlgorithmResult *result,
           std::string *error = nullptr);
  bool classify(const std::string &image_path, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool classifyCrop(const std::string &image_path, const Box &roi,
                    const InferOptions &options, AlgorithmResult *result,
                    std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  friend class Pipeline;
  friend class MultiStagePipeline;

  std::string requested_model_type_;
  Config config_;
  std::shared_ptr<NnFaceAttribute> model_;
};

}  // namespace tdl_app
