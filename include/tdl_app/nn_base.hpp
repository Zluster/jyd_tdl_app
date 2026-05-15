#pragma once

#include <memory>
#include <string>

#include "tdl_app/frame_source.hpp"
#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnBase {
 public:
  virtual ~NnBase() = default;

  virtual TaskType task() const = 0;
  virtual std::string modelType() const = 0;

  virtual bool load(EngineConfig config, std::string *error = nullptr);
  virtual bool initialize(EngineConfig config, std::string *error = nullptr);

  virtual bool predict(const std::string &image_path, const InferOptions &options,
                       AlgorithmResult *result, std::string *error = nullptr);
  virtual bool infer(const std::string &image_path, float threshold,
                     AlgorithmResult *result, std::string *error = nullptr);

  virtual bool predictFrame(const Frame &frame, const InferOptions &options,
                            AlgorithmResult *result,
                            std::string *error = nullptr);
  virtual bool inferFrame(const Frame &frame, float threshold,
                          AlgorithmResult *result,
                          std::string *error = nullptr);

  bool initialized() const { return initialized_; }

 protected:
  virtual bool onInitialize(EngineConfig *config, std::string *error = nullptr);
  virtual bool postprocess(AlgorithmResult *result,
                           std::string *error = nullptr);

  AlgorithmEngine engine_;
  EngineConfig config_;
  bool initialized_ = false;
};

}  // namespace tdl_app
