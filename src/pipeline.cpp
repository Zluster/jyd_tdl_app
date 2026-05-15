#include "tdl_app/pipeline.hpp"

#include <memory>
#include <utility>

#include "tdl_app/frame_sink.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/nn_base.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

class VisionPipeline::Impl {
 public:
  void setSource(std::unique_ptr<FrameSource> source) {
    source_ = std::move(source);
  }

  void setModel(std::shared_ptr<NnBase> model) { model_ = std::move(model); }

  void setSink(std::unique_ptr<FrameSink> sink) { sink_ = std::move(sink); }

  bool open(std::string *error) {
    if (!source_) {
      setError(error, "pipeline source is not set");
      return false;
    }
    if (!model_) {
      setError(error, "pipeline model is not set");
      return false;
    }
    if (!model_->initialized()) {
      setError(error, "pipeline model is not initialized");
      return false;
    }
    if (!source_->open(error)) {
      return false;
    }
    if (sink_ && !sink_->open(error)) {
      source_->close();
      return false;
    }
    opened_ = true;
    return true;
  }

  bool runOnce(float threshold, AlgorithmResult *result, std::string *error) {
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

    if (!model_->inferFrame(frame, threshold, result, error)) {
      return false;
    }

    if (sink_ && !sink_->write(frame, *result, error)) {
      return false;
    }
    return true;
  }

  bool runOnce(const InferOptions &options, AlgorithmResult *result,
               std::string *error) {
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

    if (!model_->predictFrame(frame, options, result, error)) {
      return false;
    }

    if (sink_ && !sink_->write(frame, *result, error)) {
      return false;
    }
    return true;
  }

  void close() {
    if (sink_) {
      sink_->close();
    }
    if (source_) {
      source_->close();
    }
    opened_ = false;
  }

 private:
  std::unique_ptr<FrameSource> source_;
  std::shared_ptr<NnBase> model_;
  std::unique_ptr<FrameSink> sink_;
  bool opened_ = false;
};

VisionPipeline::VisionPipeline() : impl_(new Impl) {}

VisionPipeline::~VisionPipeline() = default;

void VisionPipeline::setSource(std::unique_ptr<FrameSource> source) {
  impl_->setSource(std::move(source));
}

void VisionPipeline::setModel(std::shared_ptr<NnBase> model) {
  impl_->setModel(std::move(model));
}

void VisionPipeline::setSink(std::unique_ptr<FrameSink> sink) {
  impl_->setSink(std::move(sink));
}

bool VisionPipeline::open(std::string *error) { return impl_->open(error); }

bool VisionPipeline::runOnce(float threshold, AlgorithmResult *result,
                             std::string *error) {
  return impl_->runOnce(threshold, result, error);
}

bool VisionPipeline::runOnce(const InferOptions &options,
                             AlgorithmResult *result, std::string *error) {
  return impl_->runOnce(options, result, error);
}

void VisionPipeline::close() { impl_->close(); }

}  // namespace tdl_app
