#pragma once

#include <memory>
#include "tdl_app/nn_base.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {

class NnClassifier final : public NnBase {
 public:
  explicit NnClassifier(std::string model_type);
  ~NnClassifier() override;

  TaskType task() const override;
  std::string modelType() const override;

  bool load(EngineConfig config, std::string *error = nullptr);
  bool initialize(EngineConfig config, std::string *error = nullptr);
  bool classify(const std::string &image_path, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool predict(const std::string &image_path, const InferOptions &options,
               AlgorithmResult *result, std::string *error = nullptr) override;
  bool predictFrame(const Frame &frame, const InferOptions &options,
                    AlgorithmResult *result,
                    std::string *error = nullptr) override;

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
