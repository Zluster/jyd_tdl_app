#pragma once

#include <memory>
#include <string>

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

class Pipeline {
 public:
  Pipeline();
  ~Pipeline();

  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;

  void setSource(std::unique_ptr<FrameSource> source);
  void setImage(const std::string &image_path);
  void setCamera(const Camera::Config &config);
  void setModel(std::shared_ptr<NnBase> model);
  void setDetector(const Detector &detector);
  void setClassifier(const Classifier &classifier);
  void setFaceDetector(const FaceDetector &detector);
  void setFaceAttributeClassifier(const FaceAttributeClassifier &classifier);
  void setFeatureExtractor(const FeatureExtractor &feature_extractor);
  void setPlateRecognizer(const PlateRecognizer &recognizer);
  void setMmf(const Mmf::Config &config);
  void setSensorMedia(const SensorMedia::Config &config);
  void clearBootstrap();
  void setSink(std::unique_ptr<FrameSink> sink);
  void setNullSink();
  void setRtspSink(const RtspFrameSink::Config &config);

  bool open(std::string *error = nullptr);
  bool runOnce(const InferOptions &options, AlgorithmResult *result,
               std::string *error = nullptr);
  bool runOnce(float threshold, AlgorithmResult *result,
               std::string *error = nullptr);
  void close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class VisionPipeline {
 public:
  VisionPipeline();
  ~VisionPipeline();

  VisionPipeline(const VisionPipeline &) = delete;
  VisionPipeline &operator=(const VisionPipeline &) = delete;

  void setSource(std::unique_ptr<FrameSource> source);
  void setModel(std::shared_ptr<NnBase> model);
  void setSink(std::unique_ptr<FrameSink> sink);

  bool open(std::string *error = nullptr);
  bool runOnce(float threshold, AlgorithmResult *result,
               std::string *error = nullptr);
  bool runOnce(const InferOptions &options, AlgorithmResult *result,
               std::string *error = nullptr);
  void close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

using ProcessingPipeline = VisionPipeline;

}  // namespace tdl_app
