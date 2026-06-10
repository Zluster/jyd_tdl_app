#include "tdl_app/semantic_segmenter.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_utils.h"
#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/tdl_sdk_utils.hpp"

namespace tdl_app {
namespace {

struct SemanticSegRuntime {
  virtual ~SemanticSegRuntime() = default;
  virtual bool run(const std::string &image_path,
                   SemanticSegmentationResult *result,
                   std::string *error) = 0;
  virtual bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                        std::string *error) = 0;
  virtual void reset() = 0;
  virtual bool initialized() const = 0;
};

class TopformerRuntime : public SemanticSegRuntime {
 public:
  bool open(const SemanticSegmenter::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "TOPFORMER_SEG_PERSON_FACE_VEHICLE",
        error);
    if (model_type.empty()) {
      return false;
    }

    if (!loadModelDescriptor(config.model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.runtime.empty()) {
      descriptor_.runtime = "semantic_segmentation";
    }
    if (descriptor_.task_name.empty()) {
      descriptor_.task_name = "segmentation";
    }
    if (descriptor_.input_type.empty()) {
      descriptor_.input_type = "rgb";
    }

    EngineConfig engine_config;
    engine_config.model_descriptor_file = config.model_spec;
    engine_config.model_dir = config.model_dir;
    engine_config.bmrt_firmware = config.firmware;
    if (!session_.open(engine_config, descriptor_, error)) {
      return false;
    }

    const bm_net_info_t *net_info = session_.netInfo();
    if (!net_info || net_info->input_num != 1) {
      private_tdl_sdk::setError(
          error,
          "custom semantic segmentation runtime currently supports exactly one input");
      session_.close();
      return false;
    }
    if (net_info->output_num < 1) {
      private_tdl_sdk::setError(error, "semantic segmentation model has no outputs");
      session_.close();
      return false;
    }

    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f / 255.0f);

    if (!buildOutputs(error)) {
      session_.close();
      return false;
    }

    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, SemanticSegmentationResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      private_tdl_sdk::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return infer(image, result, error);
  }

  bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                std::string *error) override {
    if (frame.image_path.empty()) {
      private_tdl_sdk::setError(
          error, "custom semantic segmentation runtime currently supports image_path only");
      return false;
    }
    return run(frame.image_path, result, error);
  }

  void reset() override { session_.close(); }
  bool initialized() const override { return session_.opened(); }

 private:
  bool buildOutputs(std::string *error) {
    argmax_index_ = -1;
    conf_index_ = -1;
    output_width_ = 0;
    output_height_ = 0;

    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];
    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (shape.num_dims == 3) {
        argmax_index_ = i;
        output_height_ = shape.dims[1];
        output_width_ = shape.dims[2];
      } else if (shape.num_dims == 4 && shape.dims[0] == 1) {
        conf_index_ = i;
      }
    }

    if (argmax_index_ < 0 || output_width_ <= 0 || output_height_ <= 0) {
      private_tdl_sdk::setError(error, "unable to locate argmax output for segmentation");
      return false;
    }
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor) const {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(session_.inputWidth(), session_.inputHeight()),
               0, 0, cv::INTER_LINEAR);
    bmrt_runtime::writeImageToTensor(
        resized, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  bool infer(const cv::Mat &image, SemanticSegmentationResult *result,
             std::string *error) {
    if (!result) {
      private_tdl_sdk::setError(error,
                                "semantic segmentation result pointer is null");
      return false;
    }

    std::vector<float> input_tensor;
    preprocess(image, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (argmax_index_ < 0 || argmax_index_ >= static_cast<int>(outputs.size())) {
      private_tdl_sdk::setError(error, "invalid argmax output index");
      return false;
    }

    result->clear();
    result->width = image.cols;
    result->height = image.rows;
    result->output_width = output_width_;
    result->output_height = output_height_;

    const auto &argmax = outputs[static_cast<size_t>(argmax_index_)].data;
    result->class_id.resize(argmax.size(), 0);
    for (size_t i = 0; i < argmax.size(); ++i) {
      const float value = std::max(0.0f, std::min(argmax[i], 255.0f));
      result->class_id[i] = static_cast<std::uint8_t>(value);
    }

    result->class_conf.resize(argmax.size(), 255);
    if (conf_index_ >= 0 && conf_index_ < static_cast<int>(outputs.size())) {
      const auto &conf = outputs[static_cast<size_t>(conf_index_)].data;
      for (size_t i = 0; i < result->class_conf.size() && i < conf.size(); ++i) {
        const float value = std::max(0.0f, std::min(conf[i] * 255.0f, 255.0f));
        result->class_conf[i] = static_cast<std::uint8_t>(value);
      }
    }
    return true;
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  int argmax_index_ = -1;
  int conf_index_ = -1;
  int output_width_ = 0;
  int output_height_ = 0;
};

class LegacySemanticSegRuntime : public SemanticSegRuntime {
 public:
  bool load(const SemanticSegmenter::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "TOPFORMER_SEG_PERSON_FACE_VEHICLE",
        error);
    if (model_type.empty()) {
      return false;
    }
    if (!session_.open(config, model_type, error)) {
      return false;
    }
    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, SemanticSegmentationResult *result,
           std::string *error) override {
    private_tdl_sdk::ImageGuard image;
    if (!image.load(image_path, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                std::string *error) override {
    private_tdl_sdk::ImageGuard image;
    if (!image.wrap(frame, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  void reset() override { session_.close(); }
  bool initialized() const override { return session_.initialized(); }

 private:
  bool infer(TDLImage image, SemanticSegmentationResult *result,
             std::string *error) {
    if (!session_.initialized()) {
      private_tdl_sdk::setError(error, "semantic segmenter is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(error,
                                "semantic segmentation result pointer is null");
      return false;
    }

    TDLSegmentation meta;
    std::memset(&meta, 0, sizeof(meta));
    const int ret = TDL_SemanticSegmentation(session_.handle(),
                                             session_.modelId(), image, &meta);
    if (ret != 0) {
      private_tdl_sdk::setError(
          error, "TDL_SemanticSegmentation failed, ret=" + std::to_string(ret));
      return false;
    }

    result->clear();
    result->width = static_cast<int>(meta.width);
    result->height = static_cast<int>(meta.height);
    result->output_width = static_cast<int>(meta.output_width);
    result->output_height = static_cast<int>(meta.output_height);
    const int pixels = result->output_width * result->output_height;
    if (meta.class_id && pixels > 0) {
      result->class_id.assign(meta.class_id, meta.class_id + pixels);
    }
    if (meta.class_conf && pixels > 0) {
      result->class_conf.assign(meta.class_conf, meta.class_conf + pixels);
    }
    TDL_ReleaseSemanticSegMeta(&meta);
    return true;
  }

  private_tdl_sdk::Session session_;
};

}  // namespace

class SemanticSegmenter::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "TOPFORMER_SEG_PERSON_FACE_VEHICLE",
        error);
    if (model_type.empty()) {
      return false;
    }

    runtime_.reset();
    const bool prefer_custom =
        bmrt_runtime::startsWith(
            bmrt_runtime::toUpper(model_type), "TOPFORMER_SEG_PERSON_FACE_VEHICLE");
    if (prefer_custom) {
      std::unique_ptr<TopformerRuntime> runtime(new TopformerRuntime());
      if (!runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(runtime);
      return true;
    }
    private_tdl_sdk::setError(
        error,
        "unsupported semantic segmentation model_type for custom BMRT runtime: " +
            model_type);
    return false;
  }

  bool run(const std::string &image_path, SemanticSegmentationResult *result,
           std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "semantic segmenter is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                std::string *error) {
    if (!runtime_) {
      private_tdl_sdk::setError(error, "semantic segmenter is not initialized");
      return false;
    }
    return runtime_->runFrame(frame, result, error);
  }

  void reset() {
    if (runtime_) {
      runtime_->reset();
    }
  }

  bool initialized() const {
    return runtime_ && runtime_->initialized();
  }

 private:
  std::unique_ptr<SemanticSegRuntime> runtime_;
};

SemanticSegmenter::SemanticSegmenter() = default;

SemanticSegmenter::SemanticSegmenter(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

SemanticSegmenter::~SemanticSegmenter() {
  reset();
  delete impl_;
}

SemanticSegmenter::SemanticSegmenter(SemanticSegmenter &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

SemanticSegmenter &SemanticSegmenter::operator=(
    SemanticSegmenter &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  delete impl_;
  requested_model_type_ = std::move(other.requested_model_type_);
  config_ = std::move(other.config_);
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool SemanticSegmenter::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
}

bool SemanticSegmenter::load(const std::string &model_spec,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool SemanticSegmenter::load(const std::string &model_spec,
                             const std::string &firmware,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool SemanticSegmenter::load(const std::string &model_spec,
                             const std::string &firmware,
                             const std::string &model_dir,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool SemanticSegmenter::run(const std::string &image_path,
                            SemanticSegmentationResult *result,
                            std::string *error) {
  return segment(image_path, result, error);
}

bool SemanticSegmenter::runFrame(const Frame &frame,
                                 SemanticSegmentationResult *result,
                                 std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "semantic segmenter is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool SemanticSegmenter::segment(const std::string &image_path,
                                SemanticSegmentationResult *result,
                                std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "semantic segmenter is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool SemanticSegmenter::initialized() const {
  return impl_ && impl_->initialized();
}

std::string SemanticSegmenter::modelType() const {
  return requested_model_type_;
}

void SemanticSegmenter::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
