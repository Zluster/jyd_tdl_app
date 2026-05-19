#include "tdl_app/multi_stage_pipeline.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tdl_app/classifier.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/face_attribute_classifier.hpp"
#include "tdl_app/face_detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/nn_base.hpp"
#include "tdl_app/nn_face_attribute.hpp"
#include "tdl_app/nn_plate_recognizer.hpp"
#include "tdl_app/nn_scrfd.hpp"
#include "tdl_app/plate_recognizer.hpp"
#include "private/pipeline_bootstrap.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

enum class StageKind {
  FrameModel,
  FaceAttributeCrop,
  PlateRecognizerCrop,
};

struct StageDef {
  StageKind kind = StageKind::FrameModel;
  std::string name;
  int source_stage = -1;
  std::shared_ptr<NnBase> frame_model;
  std::shared_ptr<NnFaceAttribute> face_attribute_model;
  std::shared_ptr<NnPlateRecognizer> plate_recognizer_model;
};

}  // namespace

class MultiStagePipeline::Impl {
 public:
  void setSource(std::unique_ptr<FrameSource> source) {
    close();
    source_ = std::move(source);
  }

  void setBootstrap(std::unique_ptr<PipelineBootstrap> bootstrap) {
    close();
    bootstrap_ = std::move(bootstrap);
  }

  void clearBootstrap() {
    close();
    bootstrap_.reset();
  }

  void setSink(std::unique_ptr<FrameSink> sink) {
    close();
    sink_ = std::move(sink);
  }

  int addStage(StageDef stage) {
    close();
    stages_.push_back(std::move(stage));
    return static_cast<int>(stages_.size() - 1);
  }

  void clearStages() {
    close();
    stages_.clear();
  }

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }
    if (!source_) {
      setError(error, "multi-stage pipeline source is not set");
      return false;
    }
    if (stages_.empty()) {
      setError(error, "multi-stage pipeline has no stages");
      return false;
    }
    for (std::size_t i = 0; i < stages_.size(); ++i) {
      if (!validateStage(stages_[i], static_cast<int>(i), error)) {
        return false;
      }
    }

    if (bootstrap_ && !bootstrap_->open(error)) {
      return false;
    }
    if (!source_->open(error)) {
      if (bootstrap_) {
        bootstrap_->close();
      }
      return false;
    }
    if (sink_ && !sink_->open(error)) {
      sink_->close();
      source_->close();
      if (bootstrap_) {
        bootstrap_->close();
      }
      return false;
    }
    opened_ = true;
    return true;
  }

  bool runOnce(const InferOptions &options, Result *result, std::string *error) {
    if (!result) {
      setError(error, "result pointer is null");
      return false;
    }
    if (!opened_ && !open(error)) {
      return false;
    }

    Frame frame;
    if (!source_->read(&frame, error)) {
      return false;
    }

    result->primary = AlgorithmResult{};
    result->stages.clear();
    bool primary_set = false;

    for (std::size_t stage_index = 0; stage_index < stages_.size(); ++stage_index) {
      const StageDef &stage = stages_[stage_index];
      if (stage.kind == StageKind::FrameModel) {
        StageResult stage_result;
        stage_result.stage_index = static_cast<int>(stage_index);
        stage_result.name = stage.name;
        stage_result.source_stage = -1;
        stage_result.source_result_index = -1;
        stage_result.source_box_index = -1;
        if (!stage.frame_model->predictFrame(frame, options, &stage_result.output,
                                             error)) {
          return false;
        }
        if (!primary_set) {
          result->primary = stage_result.output;
          primary_set = true;
        }
        result->stages.push_back(std::move(stage_result));
        continue;
      }

      if (frame.image_path.empty()) {
        setError(error,
                 "crop-based multi-stage pipeline currently requires image_path input");
        return false;
      }

      std::vector<const StageResult *> source_results;
      for (std::size_t i = 0; i < result->stages.size(); ++i) {
        if (result->stages[i].stage_index == stage.source_stage) {
          source_results.push_back(&result->stages[i]);
        }
      }
      if (source_results.empty()) {
        continue;
      }

      for (std::size_t source_index = 0; source_index < source_results.size();
           ++source_index) {
        const AlgorithmResult &source_output = source_results[source_index]->output;
        for (std::size_t box_index = 0; box_index < source_output.boxes.size();
             ++box_index) {
          StageResult stage_result;
          stage_result.stage_index = static_cast<int>(stage_index);
          stage_result.name = stage.name;
          stage_result.source_stage = stage.source_stage;
          stage_result.source_result_index = static_cast<int>(source_index);
          stage_result.source_box_index = static_cast<int>(box_index);

          const Box &roi = source_output.boxes[box_index];
          bool ok = false;
          if (stage.kind == StageKind::FaceAttributeCrop &&
              stage.face_attribute_model) {
            ok = stage.face_attribute_model->predictCrop(frame.image_path, roi, options,
                                                         &stage_result.output, error);
          } else if (stage.kind == StageKind::PlateRecognizerCrop &&
                     stage.plate_recognizer_model) {
            ok = stage.plate_recognizer_model->predictCrop(
                frame.image_path, roi, options, &stage_result.output, error);
          }
          if (!ok) {
            return false;
          }
          result->stages.push_back(std::move(stage_result));
        }
      }
    }

    if (!primary_set && !result->stages.empty()) {
      result->primary = result->stages.front().output;
      primary_set = true;
    }
    if (!primary_set) {
      setError(error, "multi-stage pipeline produced no outputs");
      return false;
    }

    if (sink_ && !sink_->write(frame, result->primary, error)) {
      return false;
    }
    return true;
  }

  bool runOnce(float threshold, Result *result, std::string *error) {
    InferOptions options;
    options.threshold = threshold;
    return runOnce(options, result, error);
  }

  void close() {
    if (sink_) {
      sink_->close();
    }
    if (source_) {
      source_->close();
    }
    if (bootstrap_) {
      bootstrap_->close();
    }
    opened_ = false;
  }

 private:
  bool validateStage(const StageDef &stage, int index, std::string *error) const {
    switch (stage.kind) {
      case StageKind::FrameModel:
        if (!stage.frame_model || !stage.frame_model->initialized()) {
          setError(error, "stage " + std::to_string(index) +
                              " model is not initialized");
          return false;
        }
        return true;
      case StageKind::FaceAttributeCrop:
        if (!stage.face_attribute_model ||
            !stage.face_attribute_model->initialized()) {
          setError(error, "stage " + std::to_string(index) +
                              " face attribute model is not initialized");
          return false;
        }
        if (stage.source_stage < 0 || stage.source_stage >= index) {
          setError(error, "stage " + std::to_string(index) +
                              " has invalid source_stage");
          return false;
        }
        return true;
      case StageKind::PlateRecognizerCrop:
        if (!stage.plate_recognizer_model ||
            !stage.plate_recognizer_model->initialized()) {
          setError(error, "stage " + std::to_string(index) +
                              " plate recognizer model is not initialized");
          return false;
        }
        if (stage.source_stage < 0 || stage.source_stage >= index) {
          setError(error, "stage " + std::to_string(index) +
                              " has invalid source_stage");
          return false;
        }
        return true;
    }
    return false;
  }

  std::unique_ptr<FrameSource> source_;
  std::unique_ptr<PipelineBootstrap> bootstrap_;
  std::unique_ptr<FrameSink> sink_;
  std::vector<StageDef> stages_;
  bool opened_ = false;
};

MultiStagePipeline::MultiStagePipeline() : impl_(new Impl) {}

MultiStagePipeline::~MultiStagePipeline() = default;

void MultiStagePipeline::setSource(std::unique_ptr<FrameSource> source) {
  impl_->setSource(std::move(source));
}

void MultiStagePipeline::setImage(const std::string &image_path) {
  setSource(std::unique_ptr<FrameSource>(new ImageFileSource(image_path)));
}

void MultiStagePipeline::setCamera(const Camera::Config &config) {
  setSource(std::unique_ptr<FrameSource>(new CameraSource(config)));
}

void MultiStagePipeline::setMmf(const Mmf::Config &config) {
  impl_->setBootstrap(
      std::unique_ptr<PipelineBootstrap>(new MmfBootstrap(config)));
}

void MultiStagePipeline::setSensorMedia(const SensorMedia::Config &config) {
  impl_->setBootstrap(std::unique_ptr<PipelineBootstrap>(
      new SensorMediaBootstrap(config)));
}

void MultiStagePipeline::clearBootstrap() { impl_->clearBootstrap(); }

void MultiStagePipeline::setSink(std::unique_ptr<FrameSink> sink) {
  impl_->setSink(std::move(sink));
}

void MultiStagePipeline::setNullSink() {
  setSink(std::unique_ptr<FrameSink>(new NullFrameSink()));
}

void MultiStagePipeline::setRtspSink(const RtspFrameSink::Config &config) {
  setSink(std::unique_ptr<FrameSink>(new RtspFrameSink(config)));
}

int MultiStagePipeline::addDetectorStage(const std::string &name,
                                         const Detector &detector) {
  StageDef stage;
  stage.kind = StageKind::FrameModel;
  stage.name = name;
  stage.frame_model = detector.model_;
  return impl_->addStage(std::move(stage));
}

int MultiStagePipeline::addClassifierStage(const std::string &name,
                                           const Classifier &classifier) {
  StageDef stage;
  stage.kind = StageKind::FrameModel;
  stage.name = name;
  stage.frame_model = classifier.model_;
  return impl_->addStage(std::move(stage));
}

int MultiStagePipeline::addFeatureStage(
    const std::string &name, const FeatureExtractor &feature_extractor) {
  StageDef stage;
  stage.kind = StageKind::FrameModel;
  stage.name = name;
  stage.frame_model = feature_extractor.model_;
  return impl_->addStage(std::move(stage));
}

int MultiStagePipeline::addFaceDetectorStage(const std::string &name,
                                             const FaceDetector &detector) {
  StageDef stage;
  stage.kind = StageKind::FrameModel;
  stage.name = name;
  stage.frame_model = detector.model_;
  return impl_->addStage(std::move(stage));
}

int MultiStagePipeline::addFaceAttributeStage(
    const std::string &name, const FaceAttributeClassifier &classifier,
    int source_stage) {
  StageDef stage;
  stage.kind = StageKind::FaceAttributeCrop;
  stage.name = name;
  stage.source_stage = source_stage;
  stage.face_attribute_model = classifier.model_;
  return impl_->addStage(std::move(stage));
}

int MultiStagePipeline::addPlateRecognizerStage(
    const std::string &name, const PlateRecognizer &recognizer,
    int source_stage) {
  StageDef stage;
  stage.kind = StageKind::PlateRecognizerCrop;
  stage.name = name;
  stage.source_stage = source_stage;
  stage.plate_recognizer_model = recognizer.model_;
  return impl_->addStage(std::move(stage));
}

void MultiStagePipeline::clearStages() { impl_->clearStages(); }

bool MultiStagePipeline::open(std::string *error) { return impl_->open(error); }

bool MultiStagePipeline::runOnce(const InferOptions &options, Result *result,
                                 std::string *error) {
  return impl_->runOnce(options, result, error);
}

bool MultiStagePipeline::runOnce(float threshold, Result *result,
                                 std::string *error) {
  return impl_->runOnce(threshold, result, error);
}

void MultiStagePipeline::close() { impl_->close(); }

}  // namespace tdl_app
