#include "tdl_app/nn_base.hpp"

#include <utility>

namespace tdl_app {

bool NnBase::load(EngineConfig config, std::string *error) {
  if (!onInitialize(&config, error)) {
    return false;
  }
  config_ = std::move(config);
  initialized_ = engine_.initialize(config_, error);
  return initialized_;
}

bool NnBase::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnBase::predict(const std::string &image_path, const InferOptions &options,
                     AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnBase::infer(const std::string &image_path, float threshold,
                   AlgorithmResult *result, std::string *error) {
  InferOptions options;
  options.threshold = threshold;
  return predict(image_path, options, result, error);
}

bool NnBase::predictFrame(const Frame &frame, const InferOptions &options,
                         AlgorithmResult *result, std::string *error) {
  return inferFrame(frame, options.threshold, result, error);
}

bool NnBase::inferFrame(const Frame &frame, float threshold,
                        AlgorithmResult *result, std::string *error) {
  if (!initialized_) {
    if (error) {
      *error = "model is not initialized";
    }
    return false;
  }
  if (!engine_.runFrame(task(), modelType(), frame, threshold, result, error)) {
    return false;
  }
  return postprocess(result, error);
}

bool NnBase::postprocess(AlgorithmResult *result, std::string *error) {
  (void)result;
  (void)error;
  return true;
}

bool NnBase::onInitialize(EngineConfig *config, std::string *error) {
  (void)config;
  (void)error;
  return true;
}

}  // namespace tdl_app
