#include "tdl_app/self_learning_classifier.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace tdl_app {
namespace {

constexpr const char *kFeatureBankMagic = "TDL_APP_FEATURE_BANK_V1";

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

float dotProduct(const std::vector<float> &lhs, const std::vector<float> &rhs) {
  const size_t count = std::min(lhs.size(), rhs.size());
  float out = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    out += lhs[i] * rhs[i];
  }
  return out;
}

float vectorNorm(const std::vector<float> &values) {
  float sum = 0.0f;
  for (float value : values) {
    sum += value * value;
  }
  return std::sqrt(sum);
}

void l2Normalize(std::vector<float> *values) {
  if (!values || values->empty()) {
    return;
  }
  const float norm = vectorNorm(*values);
  if (norm <= 0.0f) {
    return;
  }
  for (float &value : *values) {
    value /= norm;
  }
}

std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::stringstream ss(text);
  while (std::getline(ss, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

bool extractFeature(FeatureExtractor *extractor, const std::string &image_path,
                    std::vector<float> *feature, std::string *error) {
  if (!extractor) {
    setError(error, "feature extractor is null");
    return false;
  }
  AlgorithmResult result;
  InferOptions options;
  if (!extractor->run(image_path, options, &result, error)) {
    return false;
  }
  if (result.feature.empty()) {
    setError(error, "feature extractor returned empty embedding");
    return false;
  }
  *feature = result.feature;
  l2Normalize(feature);
  return true;
}

bool extractFeature(FeatureExtractor *extractor, const Frame &frame,
                    std::vector<float> *feature, std::string *error) {
  if (!extractor) {
    setError(error, "feature extractor is null");
    return false;
  }
  AlgorithmResult result;
  InferOptions options;
  if (!extractor->runFrame(frame, options, &result, error)) {
    return false;
  }
  if (result.feature.empty()) {
    setError(error, "feature extractor returned empty embedding");
    return false;
  }
  *feature = result.feature;
  l2Normalize(feature);
  return true;
}

bool extractFeature(FeatureExtractor *extractor, const Frame &frame,
                    const Box &roi, std::vector<float> *feature,
                    std::string *error) {
  if (!extractor) {
    setError(error, "feature extractor is null");
    return false;
  }
  if (!roi.valid()) {
    setError(error, "feature ROI is invalid");
    return false;
  }
  AlgorithmResult result;
  InferOptions options;
  if (!extractor->extractFrameCrop(frame, roi, options, &result, error)) {
    return false;
  }
  if (result.feature.empty()) {
    setError(error, "feature extractor returned empty ROI embedding");
    return false;
  }
  *feature = result.feature;
  l2Normalize(feature);
  return true;
}

}  // namespace

SelfLearningClassifier::SelfLearningClassifier() = default;
SelfLearningClassifier::~SelfLearningClassifier() = default;
SelfLearningClassifier::SelfLearningClassifier(
    SelfLearningClassifier &&other) noexcept = default;
SelfLearningClassifier &SelfLearningClassifier::operator=(
    SelfLearningClassifier &&other) noexcept = default;

bool SelfLearningClassifier::load(const Config &config, std::string *error) {
  config_ = config;
  return extractor_.load(config_, error);
}

bool SelfLearningClassifier::load(const std::string &model_spec,
                                  std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool SelfLearningClassifier::load(const std::string &model_spec,
                                  const std::string &firmware,
                                  std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool SelfLearningClassifier::load(const std::string &model_spec,
                                  const std::string &firmware,
                                  const std::string &model_dir,
                                  std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool SelfLearningClassifier::addSample(const std::string &label,
                                       const std::string &image_path,
                                       std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (label.empty()) {
    setError(error, "label is empty");
    return false;
  }

  std::vector<float> feature;
  if (!extractFeature(&extractor_, image_path, &feature, error)) {
    return false;
  }
  if (!samples_.empty() &&
      feature.size() != samples_.front().feature.size()) {
    setError(error, "feature dimension mismatch with existing bank");
    return false;
  }

  Sample sample;
  sample.label = label;
  sample.feature = std::move(feature);
  samples_.push_back(std::move(sample));
  return true;
}

bool SelfLearningClassifier::addFrame(const std::string &label,
                                      const Frame &frame,
                                      std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (label.empty()) {
    setError(error, "label is empty");
    return false;
  }

  std::vector<float> feature;
  if (!extractFeature(&extractor_, frame, &feature, error)) {
    return false;
  }
  if (!samples_.empty() &&
      feature.size() != samples_.front().feature.size()) {
    setError(error, "feature dimension mismatch with existing bank");
    return false;
  }

  Sample sample;
  sample.label = label;
  sample.feature = std::move(feature);
  samples_.push_back(std::move(sample));
  return true;
}

bool SelfLearningClassifier::addFrameCrop(const std::string &label,
                                          const Frame &frame, const Box &roi,
                                          std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (label.empty()) {
    setError(error, "label is empty");
    return false;
  }

  std::vector<float> feature;
  if (!extractFeature(&extractor_, frame, roi, &feature, error)) {
    return false;
  }
  if (!samples_.empty() &&
      feature.size() != samples_.front().feature.size()) {
    setError(error, "feature dimension mismatch with existing bank");
    return false;
  }

  Sample sample;
  sample.label = label;
  sample.feature = std::move(feature);
  samples_.push_back(std::move(sample));
  return true;
}

bool SelfLearningClassifier::classify(
    const std::string &image_path, int top_k,
    SelfLearningClassificationResult *result, std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (!result) {
    setError(error, "classification result pointer is null");
    return false;
  }
  if (samples_.empty()) {
    setError(error, "feature bank is empty");
    return false;
  }

  std::vector<float> feature;
  if (!extractFeature(&extractor_, image_path, &feature, error)) {
    return false;
  }
  if (feature.size() != samples_.front().feature.size()) {
    setError(error, "feature dimension mismatch with bank");
    return false;
  }

  return classifyFeature(std::move(feature), top_k, result, error);
}

bool SelfLearningClassifier::classifyFrame(
    const Frame &frame, int top_k, SelfLearningClassificationResult *result,
    SelfLearningClassificationProfile *profile, std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (!result) {
    setError(error, "classification result pointer is null");
    return false;
  }
  if (samples_.empty()) {
    setError(error, "feature bank is empty");
    return false;
  }

  const auto begin = std::chrono::steady_clock::now();
  std::vector<float> feature;
  if (!extractFeature(&extractor_, frame, &feature, error)) {
    return false;
  }
  const auto extracted = std::chrono::steady_clock::now();
  const bool ok = classifyFeature(std::move(feature), top_k, result, error);
  const auto finished = std::chrono::steady_clock::now();
  if (profile) {
    profile->feature_ms = std::chrono::duration<double, std::milli>(
        extracted - begin).count();
    profile->match_ms = std::chrono::duration<double, std::milli>(
        finished - extracted).count();
    profile->total_ms = std::chrono::duration<double, std::milli>(
        finished - begin).count();
  }
  return ok;
}

bool SelfLearningClassifier::classifyFrameCrop(
    const Frame &frame, const Box &roi, int top_k,
    SelfLearningClassificationResult *result,
    SelfLearningClassificationProfile *profile, std::string *error) {
  if (!initialized()) {
    setError(error, "self learning classifier is not initialized");
    return false;
  }
  if (!result) {
    setError(error, "classification result pointer is null");
    return false;
  }
  if (samples_.empty()) {
    setError(error, "feature bank is empty");
    return false;
  }

  const auto begin = std::chrono::steady_clock::now();
  std::vector<float> feature;
  if (!extractFeature(&extractor_, frame, roi, &feature, error)) {
    return false;
  }
  const auto extracted = std::chrono::steady_clock::now();
  const bool ok = classifyFeature(std::move(feature), top_k, result, error);
  const auto finished = std::chrono::steady_clock::now();
  if (profile) {
    profile->feature_ms = std::chrono::duration<double, std::milli>(
        extracted - begin).count();
    profile->match_ms = std::chrono::duration<double, std::milli>(
        finished - extracted).count();
    profile->total_ms = std::chrono::duration<double, std::milli>(
        finished - begin).count();
  }
  return ok;
}

bool SelfLearningClassifier::classifyFeature(
    std::vector<float> feature, int top_k,
    SelfLearningClassificationResult *result, std::string *error) const {
  if (!result) {
    setError(error, "classification result pointer is null");
    return false;
  }
  if (samples_.empty()) {
    setError(error, "feature bank is empty");
    return false;
  }
  if (feature.size() != samples_.front().feature.size()) {
    setError(error, "feature dimension mismatch with bank");
    return false;
  }

  std::map<std::string, std::vector<float>> prototype_sums;
  std::map<std::string, int> prototype_counts;
  for (const auto &sample : samples_) {
    std::vector<float> &sum = prototype_sums[sample.label];
    if (sum.empty()) {
      sum.resize(sample.feature.size(), 0.0f);
    }
    for (size_t i = 0; i < sample.feature.size(); ++i) {
      sum[i] += sample.feature[i];
    }
    ++prototype_counts[sample.label];
  }

  result->clear();
  result->feature_dim = static_cast<int>(feature.size());
  for (auto &entry : prototype_sums) {
    l2Normalize(&entry.second);
    SelfLearningClassResult item;
    item.label = entry.first;
    item.sample_count = prototype_counts[entry.first];
    item.score = dotProduct(feature, entry.second);
    result->classes.push_back(std::move(item));
  }

  std::sort(result->classes.begin(), result->classes.end(),
            [](const SelfLearningClassResult &lhs,
               const SelfLearningClassResult &rhs) {
              return lhs.score > rhs.score;
            });
  if (top_k > 0 && static_cast<int>(result->classes.size()) > top_k) {
    result->classes.resize(static_cast<size_t>(top_k));
  }
  return true;
}

bool SelfLearningClassifier::saveBank(const std::string &path,
                                      std::string *error) const {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    setError(error, "failed to open bank file for write: " + path);
    return false;
  }

  out << kFeatureBankMagic << "\n";
  out << "feature_dim=" << featureDim() << "\n";
  out << "sample_count=" << sampleCount() << "\n";
  for (const auto &sample : samples_) {
    out << sample.label << "\t";
    for (size_t i = 0; i < sample.feature.size(); ++i) {
      if (i > 0) {
        out << ",";
      }
      out << sample.feature[i];
    }
    out << "\n";
  }

  if (!out.good()) {
    setError(error, "failed while writing bank file: " + path);
    return false;
  }
  return true;
}

bool SelfLearningClassifier::loadBank(const std::string &path,
                                      std::string *error) {
  std::ifstream in(path);
  if (!in) {
    setError(error, "failed to open bank file: " + path);
    return false;
  }

  std::string line;
  if (!std::getline(in, line) || line != kFeatureBankMagic) {
    setError(error, "invalid feature bank header");
    return false;
  }

  int expected_dim = -1;
  int expected_samples = -1;
  if (std::getline(in, line)) {
    const std::vector<std::string> parts = split(line, '=');
    if (parts.size() == 2) {
      expected_dim = std::atoi(parts[1].c_str());
    }
  }
  if (std::getline(in, line)) {
    const std::vector<std::string> parts = split(line, '=');
    if (parts.size() == 2) {
      expected_samples = std::atoi(parts[1].c_str());
    }
  }

  std::vector<Sample> loaded;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      setError(error, "invalid bank sample line");
      return false;
    }

    Sample sample;
    sample.label = line.substr(0, tab);
    const std::vector<std::string> values = split(line.substr(tab + 1), ',');
    sample.feature.reserve(values.size());
    for (const std::string &value : values) {
      sample.feature.push_back(static_cast<float>(std::atof(value.c_str())));
    }
    l2Normalize(&sample.feature);

    if (expected_dim >= 0 &&
        static_cast<int>(sample.feature.size()) != expected_dim) {
      setError(error, "feature dimension mismatch inside bank file");
      return false;
    }
    loaded.push_back(std::move(sample));
  }

  if (expected_samples >= 0 &&
      static_cast<int>(loaded.size()) != expected_samples) {
    setError(error, "sample count mismatch inside bank file");
    return false;
  }

  if (!samples_.empty() && !loaded.empty() &&
      loaded.front().feature.size() != samples_.front().feature.size()) {
    setError(error, "loaded bank feature dimension mismatches current bank");
    return false;
  }
  samples_ = std::move(loaded);
  return true;
}

void SelfLearningClassifier::clearBank() { samples_.clear(); }

bool SelfLearningClassifier::initialized() const {
  return extractor_.initialized();
}

int SelfLearningClassifier::featureDim() const {
  if (samples_.empty()) {
    return 0;
  }
  return static_cast<int>(samples_.front().feature.size());
}

int SelfLearningClassifier::sampleCount() const {
  return static_cast<int>(samples_.size());
}

int SelfLearningClassifier::classCount() const {
  std::map<std::string, int> labels;
  for (const auto &sample : samples_) {
    labels[sample.label] = 1;
  }
  return static_cast<int>(labels.size());
}

}  // namespace tdl_app
