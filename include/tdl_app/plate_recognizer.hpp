#pragma once

#include <memory>
#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

class NnPlateRecognizer;
class MultiStagePipeline;
class Pipeline;

struct OcrProfile {
  double frame_convert_ms = 0.0;
  double det_preprocess_ms = 0.0;
  double det_inference_ms = 0.0;
  double det_postprocess_ms = 0.0;
  double rectify_ms = 0.0;
  double rec_preprocess_ms = 0.0;
  double rec_inference_ms = 0.0;
  double rec_decode_ms = 0.0;
  double total_ms = 0.0;
  int text_regions = 0;
  bool hardware_det_preprocess = false;
};

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
  bool runFrame(const Frame &frame, const InferOptions &options,
                AlgorithmResult *result, std::string *error = nullptr);
  bool runFrameProfiled(const Frame &frame, const InferOptions &options,
                        AlgorithmResult *result, OcrProfile *profile,
                        std::string *error = nullptr);
  bool runFrame(const Frame &frame, const Box &roi,
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
