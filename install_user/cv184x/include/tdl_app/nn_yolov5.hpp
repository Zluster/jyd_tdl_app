#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"

namespace tdl_app {

class NnYolov5 final : public NnBase {
 public:
  explicit NnYolov5(std::string model_type);
  ~NnYolov5() override;

  TaskType task() const override;
  std::string modelType() const override;

  bool load(EngineConfig config, std::string *error = nullptr);
  bool initialize(EngineConfig config, std::string *error = nullptr);
  bool detect(const std::string &image_path, const InferOptions &options,
              AlgorithmResult *result, std::string *error = nullptr);
  bool predict(const std::string &image_path, const InferOptions &options,
               AlgorithmResult *result, std::string *error = nullptr) override;
  bool predictFrame(const Frame &frame, const InferOptions &options,
                    AlgorithmResult *result,
                    std::string *error = nullptr) override;

 private:
  class CustomRuntime;

  bool loadDescriptor(std::string *error);
  bool shouldUseCustomRuntime() const;

  std::string model_type_;
  EngineConfig config_;
  ModelDescriptor descriptor_;
  bool descriptor_loaded_ = false;
  std::unique_ptr<CustomRuntime> custom_runtime_;
};

}  // namespace tdl_app
