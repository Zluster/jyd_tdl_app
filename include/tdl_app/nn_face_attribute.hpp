#pragma once

#include <memory>
#include <string>

#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/nn_base.hpp"

namespace tdl_app {

class NnFaceAttribute final : public NnBase {
 public:
  explicit NnFaceAttribute(std::string model_type = "FACE_ATTRIBUTE");
  ~NnFaceAttribute() override;

  TaskType task() const override;
  std::string modelType() const override;

  bool load(EngineConfig config, std::string *error = nullptr);
  bool initialize(EngineConfig config, std::string *error = nullptr);
  bool predict(const std::string &image_path, const InferOptions &options,
               AlgorithmResult *result, std::string *error = nullptr) override;
  bool predictFrame(const Frame &frame, const InferOptions &options,
                    AlgorithmResult *result,
                    std::string *error = nullptr) override;
  bool predictCrop(const std::string &image_path, const Box &roi,
                   const InferOptions &options, AlgorithmResult *result,
                   std::string *error = nullptr);

 private:
  class CustomRuntime;

  bool loadDescriptor(std::string *error);

  std::string model_type_;
  EngineConfig config_;
  ModelDescriptor descriptor_;
  bool descriptor_loaded_ = false;
  std::unique_ptr<CustomRuntime> custom_runtime_;
};

}  // namespace tdl_app
