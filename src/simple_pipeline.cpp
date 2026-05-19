#include "tdl_app/simple_pipeline.hpp"

#include <memory>
#include <utility>

#include "tdl_app/classifier.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/camera.hpp"
#include "tdl_app/face_attribute_classifier.hpp"
#include "tdl_app/face_detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_face_attribute.hpp"
#include "tdl_app/nn_plate_recognizer.hpp"
#include "tdl_app/nn_scrfd.hpp"
#include "tdl_app/pipeline.hpp"
#include "tdl_app/plate_recognizer.hpp"
#include "private/pipeline_bootstrap.hpp"

namespace tdl_app {

class Pipeline::Impl {
 public:
  void setSource(std::unique_ptr<FrameSource> source) {
    close();
    pipeline_.setSource(std::move(source));
  }

  void setModel(std::shared_ptr<NnBase> model) {
    close();
    pipeline_.setModel(std::move(model));
  }

  void setSink(std::unique_ptr<FrameSink> sink) {
    close();
    pipeline_.setSink(std::move(sink));
  }

  void setBootstrap(std::unique_ptr<PipelineBootstrap> bootstrap) {
    close();
    bootstrap_ = std::move(bootstrap);
  }

  void clearBootstrap() {
    close();
    bootstrap_.reset();
  }

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }
    if (bootstrap_ && !bootstrap_->open(error)) {
      return false;
    }
    if (!pipeline_.open(error)) {
      if (bootstrap_) {
        bootstrap_->close();
      }
      return false;
    }
    opened_ = true;
    return true;
  }

  bool runOnce(const InferOptions &options, AlgorithmResult *result,
               std::string *error) {
    if (!opened_ && !open(error)) {
      return false;
    }
    return pipeline_.runOnce(options, result, error);
  }

  bool runOnce(float threshold, AlgorithmResult *result,
               std::string *error) {
    if (!opened_ && !open(error)) {
      return false;
    }
    return pipeline_.runOnce(threshold, result, error);
  }

  void close() {
    pipeline_.close();
    if (bootstrap_) {
      bootstrap_->close();
    }
    opened_ = false;
  }

 private:
  VisionPipeline pipeline_;
  std::unique_ptr<PipelineBootstrap> bootstrap_;
  bool opened_ = false;
};

Pipeline::Pipeline() : impl_(new Impl) {}

Pipeline::~Pipeline() = default;

void Pipeline::setSource(std::unique_ptr<FrameSource> source) {
  impl_->setSource(std::move(source));
}

void Pipeline::setImage(const std::string &image_path) {
  setSource(std::unique_ptr<FrameSource>(new ImageFileSource(image_path)));
}

void Pipeline::setCamera(const Camera::Config &config) {
  setSource(std::unique_ptr<FrameSource>(new CameraSource(config)));
}

void Pipeline::setModel(std::shared_ptr<NnBase> model) {
  impl_->setModel(std::move(model));
}

void Pipeline::setDetector(const Detector &detector) {
  impl_->setModel(detector.model_);
}

void Pipeline::setClassifier(const Classifier &classifier) {
  impl_->setModel(classifier.model_);
}

void Pipeline::setFaceDetector(const FaceDetector &detector) {
  impl_->setModel(std::static_pointer_cast<NnBase>(detector.model_));
}

void Pipeline::setFaceAttributeClassifier(
    const FaceAttributeClassifier &classifier) {
  impl_->setModel(std::static_pointer_cast<NnBase>(classifier.model_));
}

void Pipeline::setFeatureExtractor(const FeatureExtractor &feature_extractor) {
  impl_->setModel(feature_extractor.model_);
}

void Pipeline::setPlateRecognizer(const PlateRecognizer &recognizer) {
  impl_->setModel(std::static_pointer_cast<NnBase>(recognizer.model_));
}

void Pipeline::setMmf(const Mmf::Config &config) {
  impl_->setBootstrap(std::unique_ptr<PipelineBootstrap>(
      new MmfBootstrap(config)));
}

void Pipeline::setSensorMedia(const SensorMedia::Config &config) {
  impl_->setBootstrap(std::unique_ptr<PipelineBootstrap>(
      new SensorMediaBootstrap(config)));
}

void Pipeline::clearBootstrap() {
  impl_->clearBootstrap();
}

void Pipeline::setSink(std::unique_ptr<FrameSink> sink) {
  impl_->setSink(std::move(sink));
}

void Pipeline::setNullSink() {
  setSink(std::unique_ptr<FrameSink>(new NullFrameSink()));
}

void Pipeline::setRtspSink(const RtspFrameSink::Config &config) {
  setSink(std::unique_ptr<FrameSink>(new RtspFrameSink(config)));
}

bool Pipeline::open(std::string *error) {
  return impl_->open(error);
}

bool Pipeline::runOnce(const InferOptions &options, AlgorithmResult *result,
                       std::string *error) {
  return impl_->runOnce(options, result, error);
}

bool Pipeline::runOnce(float threshold, AlgorithmResult *result,
                       std::string *error) {
  return impl_->runOnce(threshold, result, error);
}

void Pipeline::close() {
  impl_->close();
}

}  // namespace tdl_app
