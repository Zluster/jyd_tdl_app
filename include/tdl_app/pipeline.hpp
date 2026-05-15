#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/camera.hpp"
#include "tdl_app/frame_sink.hpp"

namespace tdl_app {

class Classifier;
class Detector;
class FeatureExtractor;
class FrameSource;
class NnBase;

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
  void setFeatureExtractor(const FeatureExtractor &feature_extractor);
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
  std::unique_ptr<class VisionPipeline> pipeline_;
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
