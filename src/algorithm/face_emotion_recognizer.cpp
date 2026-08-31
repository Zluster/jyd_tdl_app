#include "tdl_app/face_emotion_recognizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tdl_app/face_attribute_classifier.hpp"
#include "tdl_app/face_detector.hpp"

namespace tdl_app {
namespace {

constexpr std::array<const char *, 7> kEmotionNames = {{
    "anger", "disgust", "fear", "happy", "neutral", "sad", "surprise"}};

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

float attributeValue(const AlgorithmResult &result, const char *name,
                     float fallback) {
  for (const Attribute &attribute : result.attributes) {
    if (attribute.name == name) return attribute.value;
  }
  return fallback;
}

}  // namespace

struct FaceEmotionRecognizer::Impl {
  // CV184X releases the attribute BMRT runtime before SCRFD, matching the
  // shutdown ordering already validated by FaceRecognizer.
  ~Impl() {
    attributes.reset();
    detector.reset();
  }

  Impl()
      : detector(FaceDetector::scrfd()),
        attributes(FaceAttributeClassifier::generic()) {}

  Config config;
  FaceDetector detector;
  FaceAttributeClassifier attributes;
  bool loaded = false;
};

FaceEmotionRecognizer::FaceEmotionRecognizer() : impl_(new Impl()) {}
FaceEmotionRecognizer::~FaceEmotionRecognizer() = default;

bool FaceEmotionRecognizer::load(const Config &config, std::string *error) {
  if (!impl_) {
    setError(error, "face emotion recognizer is unavailable");
    return false;
  }
  if (config.detector_model_spec.empty() || config.attribute_model_spec.empty()) {
    setError(error, "detector_model_spec and attribute_model_spec are required");
    return false;
  }
  if (config.max_faces <= 0) {
    setError(error, "max_faces must be positive");
    return false;
  }
  impl_->loaded = false;
  if (!impl_->detector.load(ModelSessionConfig::fromSpec(
          config.detector_model_spec, config.firmware), error) ||
      !impl_->attributes.load(ModelSessionConfig::fromSpec(
          config.attribute_model_spec, config.firmware), error)) {
    return false;
  }
  impl_->config = config;
  impl_->loaded = true;
  return true;
}

bool FaceEmotionRecognizer::initialized() const {
  return impl_ && impl_->loaded && impl_->detector.initialized() &&
         impl_->attributes.initialized();
}

bool FaceEmotionRecognizer::recognizeFrame(
    const Frame &frame, std::vector<FaceEmotionResult> *results,
    std::string *error) {
  if (!results) {
    setError(error, "face emotion results pointer is null");
    return false;
  }
  results->clear();
  if (!initialized()) {
    setError(error, "face emotion recognizer is not initialized");
    return false;
  }

  AlgorithmResult detected;
  if (!impl_->detector.detectFrame(
          frame, InferOptions::detection(impl_->config.face_threshold,
                                         impl_->config.iou_threshold),
          &detected, error)) {
    return false;
  }

  for (const Box &face : detected.boxes) {
    if (static_cast<int>(results->size()) >= impl_->config.max_faces) break;

    AlgorithmResult attributes;
    if (!impl_->attributes.classifyFrameCrop(frame, face, InferOptions{},
                                             &attributes, error)) {
      return false;
    }
    const int emotion_id = static_cast<int>(attributeValue(
        attributes, "emotion", -1.0f));
    if (emotion_id < 0 || emotion_id >= static_cast<int>(kEmotionNames.size())) {
      setError(error, "face attribute model did not return a valid emotion head");
      return false;
    }

    FaceEmotionResult result;
    result.box = face;
    result.emotion_id = emotion_id;
    result.emotion = kEmotionNames[static_cast<size_t>(emotion_id)];
    result.emotion_score = attributeValue(attributes, "emotion_score", 0.0f);
    result.detection_score = face.score;
    result.gender = attributeValue(attributes, "gender", -1.0f);
    result.age = attributeValue(attributes, "age", -1.0f);
    result.glasses = attributeValue(attributes, "glasses", -1.0f);
    if (result.gender >= 0.0f) {
      result.gender_label = result.gender > 0.5f ? "male" : "female";
    }
    if (result.age >= 0.0f) {
      result.age_years = static_cast<int>(std::lround(
          std::max(0.0f, std::min(1.0f, result.age)) * 100.0f));
    }
    result.has_glasses = result.glasses > 0.5f;
    results->push_back(std::move(result));
  }
  return true;
}

}  // namespace tdl_app
