#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnPlateRecognizer;
class MultiStagePipeline;
class Pipeline;

class PlateRecognizer {
 public:
  using Config = ModelSessionConfig;

  PlateRecognizer();
  explicit PlateRecognizer(std::string model_type);
  ~PlateRecognizer();

  static PlateRecognizer lpr() { return PlateRecognizer("PLATE_RECOGNIZER"); }

  PlateRecognizer(const PlateRecognizer &) = delete;
  PlateRecognizer &operator=(const PlateRecognizer &) = delete;
  PlateRecognizer(PlateRecognizer &&) noexcept;
  PlateRecognizer &operator=(PlateRecognizer &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::string &image_path, const InferOptions &options,
           AlgorithmResult *result, std::string *error = nullptr);
  bool run(const std::string &image_path, const Box &roi,
           const InferOptions &options, AlgorithmResult *result,
           std::string *error = nullptr);
  bool recognize(const std::string &image_path, const InferOptions &options,
                 AlgorithmResult *result, std::string *error = nullptr);
  bool recognizeCrop(const std::string &image_path, const Box &roi,
                     const InferOptions &options, AlgorithmResult *result,
                     std::string *error = nullptr);

  bool initialized() const;
  std::string modelType() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  friend class Pipeline;
  friend class MultiStagePipeline;

  std::string requested_model_type_;
  Config config_;
  std::shared_ptr<NnPlateRecognizer> model_;
};

}  // namespace tdl_app
