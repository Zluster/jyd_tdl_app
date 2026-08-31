#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

struct FaceEmotionResult {
  Box box;
  std::string emotion = "unknown";
  int emotion_id = -1;
  float emotion_score = 0.0f;
  float detection_score = 0.0f;
  float gender = -1.0f;
  float age = -1.0f;
  float glasses = -1.0f;
  std::string gender_label = "unknown";
  int age_years = -1;
  bool has_glasses = false;
};

// Online face emotion pipeline: SCRFD detection followed by face-ROI
// attribute inference. Both stages use the NPU; the second stage crops and
// resizes the detected face with VPSS.
class FaceEmotionRecognizer {
 public:
  struct Config {
    std::string detector_model_spec;
    std::string attribute_model_spec;
    std::string firmware;
    float face_threshold = 0.35f;
    float iou_threshold = 0.45f;
    int max_faces = 3;
  };

  FaceEmotionRecognizer();
  ~FaceEmotionRecognizer();

  FaceEmotionRecognizer(const FaceEmotionRecognizer &) = delete;
  FaceEmotionRecognizer &operator=(const FaceEmotionRecognizer &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool initialized() const;
  bool recognizeFrame(const Frame &frame, std::vector<FaceEmotionResult> *results,
                      std::string *error = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
