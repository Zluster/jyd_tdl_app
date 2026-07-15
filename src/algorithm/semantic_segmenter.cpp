#include "tdl_app/semantic_segmenter.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string resolveModelType(const SemanticSegmenter::Config &config,
                             const std::string &requested_model_type,
                             std::string *error) {
  const std::string model_type =
      !config.model_type.empty() ? config.model_type
      : !requested_model_type.empty() ? requested_model_type
                                     : "TOPFORMER_SEG_PERSON_FACE_VEHICLE";
  if (model_type.empty()) {
    setError(error, "semantic segmentation model_type is empty");
  }
  return model_type;
}

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
    const std::string model_type =
        resolveModelType(config, requested_model_type, error);
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
      setError(error,
               "semantic segmentation runtime supports exactly one input");
      session_.close();
      return false;
    }
    if (net_info->output_num < 1) {
      setError(error, "semantic segmentation model has no outputs");
      session_.close();
      return false;
    }

    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f / 255.0f);

    if (!buildOutputs(error)) {
      session_.close();
      return false;
    }

    if (!session_.nchwLayout() ||
        (session_.inputDtype() != BM_INT8 &&
         session_.inputDtype() != BM_UINT8)) {
      setError(error, "semantic segmentation VPSS path requires NCHW INT8/UINT8 input");
      session_.close();
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = session_.inputWidth();
    vpss_config.height = session_.inputHeight();
    vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
    vpss_config.input_dtype = session_.inputDtype();
    vpss_config.input_scale = session_.inputScale();
    vpss_config.input_zero_point = session_.inputZeroPoint();
    vpss_config.mean = {{mean_[0], mean_[1], mean_[2]}};
    vpss_config.scale = {{scale_[0], scale_[1], scale_[2]}};
    std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor(
        new bmrt_runtime::VpssPreprocessor());
    if (!preprocessor->open(session_.handle(), vpss_config, error)) {
      session_.close();
      return false;
    }
    preprocessor_ = std::move(preprocessor);

    if (resolved_model_type) {
      *resolved_model_type = model_type;
    }
    return true;
  }

  bool run(const std::string &image_path, SemanticSegmentationResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      setError(error, "failed to read image: " + image_path);
      return false;
    }
    return infer(image, result, error);
  }

  bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                std::string *error) override {
    if (!frame.image_path.empty()) {
      return run(frame.image_path, result, error);
    }
    if (!result || !frame.native) {
      setError(error, "semantic segmentation frame/result pointer is null");
      return false;
    }
    if (!preprocessor_) {
      setError(error, "semantic segmentation VPSS preprocessor is unavailable");
      return false;
    }
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    const int source_width = static_cast<int>(video->stVFrame.u32Width);
    const int source_height = static_cast<int>(video->stVFrame.u32Height);
    if (source_width <= 0 || source_height <= 0) {
      setError(error, "semantic segmentation source frame dimensions are invalid");
      return false;
    }
    if (!preprocessor_->preprocess(video, error)) {
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launchDevice(preprocessor_->inputMemory(), &outputs, error)) {
      return false;
    }
    return writeResult(outputs, source_width, source_height, result, error);
  }

  void reset() override {
    preprocessor_.reset();
    session_.close();
  }
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
      setError(error, "unable to locate argmax output for segmentation");
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

  bool writeResult(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                   int source_width, int source_height,
                   SemanticSegmentationResult *result, std::string *error) {
    if (!result) {
      setError(error, "semantic segmentation result pointer is null");
      return false;
    }
    if (argmax_index_ < 0 || argmax_index_ >= static_cast<int>(outputs.size())) {
      setError(error, "invalid argmax output index");
      return false;
    }

    result->clear();
    result->width = source_width;
    result->height = source_height;
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

  bool infer(const cv::Mat &image, SemanticSegmentationResult *result,
             std::string *error) {
    std::vector<float> input_tensor;
    preprocess(image, &input_tensor);
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    return writeResult(outputs, image.cols, image.rows, result, error);
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor_;
  int argmax_index_ = -1;
  int conf_index_ = -1;
  int output_width_ = 0;
  int output_height_ = 0;
};

}  // namespace

class SemanticSegmenter::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type =
        resolveModelType(config, requested_model_type, error);
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
    setError(
        error,
        "unsupported semantic segmentation model_type for custom BMRT runtime: " +
            model_type);
    return false;
  }

  bool run(const std::string &image_path, SemanticSegmentationResult *result,
           std::string *error) {
    if (!runtime_) {
      setError(error, "semantic segmenter is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, SemanticSegmentationResult *result,
                std::string *error) {
    if (!runtime_) {
      setError(error, "semantic segmenter is not initialized");
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
    setError(error, "semantic segmenter is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool SemanticSegmenter::segment(const std::string &image_path,
                                SemanticSegmentationResult *result,
                                std::string *error) {
  if (!impl_) {
    setError(error, "semantic segmenter is not initialized");
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
