#pragma once

#include <string>
#include <vector>

#include "tdl_app/feature_extractor.hpp"

namespace tdl_app {

struct SelfLearningClassResult {
  std::string label;
  float score = 0.0f;
  int sample_count = 0;
};

struct SelfLearningClassificationResult {
  std::vector<SelfLearningClassResult> classes;
  int feature_dim = 0;

  void clear() {
    classes.clear();
    feature_dim = 0;
  }

  bool empty() const { return classes.empty(); }
};

struct SelfLearningClassificationProfile {
  double feature_ms = 0.0;
  double match_ms = 0.0;
  double total_ms = 0.0;
};

class SelfLearningClassifier {
 public:
  using Config = FeatureExtractor::Config;

  SelfLearningClassifier();
  ~SelfLearningClassifier();

  SelfLearningClassifier(const SelfLearningClassifier &) = delete;
  SelfLearningClassifier &operator=(const SelfLearningClassifier &) = delete;
  SelfLearningClassifier(SelfLearningClassifier &&other) noexcept;
  SelfLearningClassifier &operator=(SelfLearningClassifier &&other) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool addSample(const std::string &label, const std::string &image_path,
                 std::string *error = nullptr);
  bool addFrame(const std::string &label, const Frame &frame,
                std::string *error = nullptr);
  bool classify(const std::string &image_path, int top_k,
                SelfLearningClassificationResult *result,
                std::string *error = nullptr);
  bool classifyFrame(const Frame &frame, int top_k,
                     SelfLearningClassificationResult *result,
                     SelfLearningClassificationProfile *profile = nullptr,
                     std::string *error = nullptr);

  bool saveBank(const std::string &path, std::string *error = nullptr) const;
  bool loadBank(const std::string &path, std::string *error = nullptr);
  void clearBank();

  bool initialized() const;
  int featureDim() const;
  int sampleCount() const;
  int classCount() const;
  const Config &config() const { return config_; }

 private:
  struct Sample {
    std::string label;
    std::vector<float> feature;
  };

  bool classifyFeature(std::vector<float> feature, int top_k,
                       SelfLearningClassificationResult *result,
                       std::string *error) const;

  Config config_;
  FeatureExtractor extractor_;
  std::vector<Sample> samples_;
};

}  // namespace tdl_app
