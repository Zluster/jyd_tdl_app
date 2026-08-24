#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class FaceDetector;
class FeatureExtractor;

struct FaceRecognitionResult {
  Box box;
  std::string name;
  int class_id = 0;
  float similarity = -1.0f;
  bool matched = false;

  // Kept internal to the Python-facing recognition result so addFace() can
  // register the exact feature produced by recognizeFrame().
  std::vector<float> feature;
};

// Two-model online face recognition: SCRFD detection plus face embedding.
// Pixel alignment runs on the CPU, while both inference stages use the NPU.
class FaceRecognizer {
 public:
  struct Config {
    std::string detector_model_spec;
    std::string feature_model_spec;
    std::string firmware;
    float face_threshold = 0.25f;
    float match_threshold = 0.50f;
    int max_faces = 3;
  };

  FaceRecognizer();
  ~FaceRecognizer();

  FaceRecognizer(const FaceRecognizer &) = delete;
  FaceRecognizer &operator=(const FaceRecognizer &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool initialized() const;

  // Registers the largest SCRFD face from a live VPSS/VI frame.
  bool enrollFrame(const std::string &name, const Frame &frame,
                   std::string *error = nullptr);

  // Detects and recognizes up to Config::max_faces faces in a live frame.
  bool recognizeFrame(const Frame &frame,
                      std::vector<FaceRecognitionResult> *results,
                      std::string *error = nullptr);

  // MaixPy-compatible recognition controls. Detection and matching thresholds
  // are supplied per frame; max_faces remains part of Config.
  bool recognizeFrame(const Frame &frame, float face_threshold,
                      float iou_threshold, float match_threshold,
                      std::vector<FaceRecognitionResult> *results,
                      std::string *error = nullptr);

  // Registers a recognition result previously returned by recognizeFrame().
  bool addFace(const FaceRecognitionResult &face, const std::string &name,
               std::string *error = nullptr);
  bool saveFaces(const std::string &path, std::string *error = nullptr) const;
  bool loadFaces(const std::string &path, std::string *error = nullptr);

  bool remove(const std::string &name);
  void clear();
  std::vector<std::string> names() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
