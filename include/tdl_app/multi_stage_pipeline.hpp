#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/camera.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/mmf.hpp"
#include "tdl_app/sensor_media.hpp"

namespace tdl_app {

class Classifier;
class Detector;
class FaceAttributeClassifier;
class FaceDetector;
class FeatureExtractor;
class FrameSource;
class NnBase;
class PlateRecognizer;

class MultiStagePipeline {
 public:
  struct StageResult {
    int stage_index = -1;
    std::string name;
    int source_stage = -1;
    int source_result_index = -1;
    int source_box_index = -1;
    AlgorithmResult output;
  };

  struct Result {
    AlgorithmResult primary;
    std::vector<StageResult> stages;
  };

  MultiStagePipeline();
  ~MultiStagePipeline();

  MultiStagePipeline(const MultiStagePipeline &) = delete;
  MultiStagePipeline &operator=(const MultiStagePipeline &) = delete;

  void setSource(std::unique_ptr<FrameSource> source);
  void setImage(const std::string &image_path);
  void setCamera(const Camera::Config &config);
  void setMmf(const Mmf::Config &config);
  void setSensorMedia(const SensorMedia::Config &config);
  void clearBootstrap();
  void setSink(std::unique_ptr<FrameSink> sink);
  void setNullSink();
  void setRtspSink(const RtspFrameSink::Config &config);

  int addDetectorStage(const std::string &name, const Detector &detector);
  int addDetectorStage(const Detector &detector) {
    return addDetectorStage("detector", detector);
  }
  int addClassifierStage(const std::string &name, const Classifier &classifier);
  int addClassifierStage(const Classifier &classifier) {
    return addClassifierStage("classifier", classifier);
  }
  int addFeatureStage(const std::string &name,
                      const FeatureExtractor &feature_extractor);
  int addFeatureStage(const FeatureExtractor &feature_extractor) {
    return addFeatureStage("feature", feature_extractor);
  }
  int addFaceDetectorStage(const std::string &name,
                           const FaceDetector &detector);
  int addFaceDetectorStage(const FaceDetector &detector) {
    return addFaceDetectorStage("face_detector", detector);
  }
  int addFaceAttributeStage(const std::string &name,
                            const FaceAttributeClassifier &classifier,
                            int source_stage);
  int addFaceAttributeStage(const FaceAttributeClassifier &classifier,
                            int source_stage) {
    return addFaceAttributeStage("face_attribute", classifier, source_stage);
  }
  int addPlateRecognizerStage(const std::string &name,
                              const PlateRecognizer &recognizer,
                              int source_stage);
  int addPlateRecognizerStage(const PlateRecognizer &recognizer,
                              int source_stage) {
    return addPlateRecognizerStage("plate_recognizer", recognizer, source_stage);
  }
  void clearStages();

  bool open(std::string *error = nullptr);
  bool runOnce(const InferOptions &options, Result *result,
               std::string *error = nullptr);
  bool runOnce(float threshold, Result *result, std::string *error = nullptr);
  void close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
