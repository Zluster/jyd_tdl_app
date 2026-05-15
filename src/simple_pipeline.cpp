#include "tdl_app/simple_pipeline.hpp"

#include <memory>
#include <utility>

#include "tdl_app/classifier.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/camera.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/pipeline.hpp"

namespace tdl_app {

Pipeline::Pipeline() : pipeline_(new VisionPipeline()) {}

Pipeline::~Pipeline() = default;

void Pipeline::setSource(std::unique_ptr<FrameSource> source) {
  pipeline_->setSource(std::move(source));
}

void Pipeline::setImage(const std::string &image_path) {
  setSource(std::unique_ptr<FrameSource>(new ImageFileSource(image_path)));
}

void Pipeline::setCamera(const Camera::Config &config) {
  setSource(std::unique_ptr<FrameSource>(new CameraSource(config)));
}

void Pipeline::setModel(std::shared_ptr<NnBase> model) {
  pipeline_->setModel(std::move(model));
}

void Pipeline::setDetector(const Detector &detector) {
  pipeline_->setModel(detector.model_);
}

void Pipeline::setClassifier(const Classifier &classifier) {
  pipeline_->setModel(classifier.model_);
}

void Pipeline::setFeatureExtractor(const FeatureExtractor &feature_extractor) {
  pipeline_->setModel(feature_extractor.model_);
}

void Pipeline::setSink(std::unique_ptr<FrameSink> sink) {
  pipeline_->setSink(std::move(sink));
}

void Pipeline::setNullSink() {
  setSink(std::unique_ptr<FrameSink>(new NullFrameSink()));
}

void Pipeline::setRtspSink(const RtspFrameSink::Config &config) {
  setSink(std::unique_ptr<FrameSink>(new RtspFrameSink(config)));
}

bool Pipeline::open(std::string *error) {
  return pipeline_->open(error);
}

bool Pipeline::runOnce(const InferOptions &options, AlgorithmResult *result,
                       std::string *error) {
  return pipeline_->runOnce(options, result, error);
}

bool Pipeline::runOnce(float threshold, AlgorithmResult *result,
                       std::string *error) {
  return pipeline_->runOnce(threshold, result, error);
}

void Pipeline::close() {
  pipeline_->close();
}

}  // namespace tdl_app
