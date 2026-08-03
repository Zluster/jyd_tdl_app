#include "tdl_app/nn_face_attribute.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"

namespace tdl_app {
namespace {

void addAttribute(AlgorithmResult *result, const std::string &name, float value) {
  Attribute attr;
  attr.name = name;
  attr.value = value;
  result->attributes.push_back(attr);
}

bool toVpssRoi(const Box &box, int image_width, int image_height,
               bmrt_runtime::VpssPreprocessor::Roi *roi,
               std::string *error) {
  if (!roi || image_width <= 0 || image_height <= 0 || box.x2 <= box.x1 ||
      box.y2 <= box.y1) {
    bmrt_runtime::setError(error, "face attribute ROI is invalid");
    return false;
  }

  const int x = std::max(0, std::min(static_cast<int>(box.x1), image_width - 1));
  const int y = std::max(0, std::min(static_cast<int>(box.y1), image_height - 1));
  const int right = std::max(x + 1, std::min(static_cast<int>(box.x2), image_width));
  const int bottom = std::max(y + 1, std::min(static_cast<int>(box.y2), image_height));
  roi->x = x;
  roi->y = y;
  roi->width = right - x;
  roi->height = bottom - y;
  return true;
}

}  // namespace

class NnFaceAttribute::CustomRuntime {
 public:
  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    descriptor_ = descriptor;
    if (!session_.open(config, descriptor, error)) {
      return false;
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 0.0f);
    scale_ =
        bmrt_runtime::expandChannelValues(descriptor.scale, 0.00392156862745098f);
    if (!mapOutputs()) {
      return false;
    }
    if (session_.nchwLayout() &&
        (session_.inputDtype() == BM_INT8 ||
         session_.inputDtype() == BM_UINT8)) {
      bmrt_runtime::VpssPreprocessor::Config vpss_config;
      vpss_config.width = session_.inputWidth();
      vpss_config.height = session_.inputHeight();
      vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
      vpss_config.input_dtype = session_.inputDtype();
      vpss_config.input_scale = session_.inputScale();
      vpss_config.input_zero_point = session_.inputZeroPoint();
      for (int i = 0; i < 3; ++i) {
        vpss_config.mean[static_cast<size_t>(i)] = mean_[static_cast<size_t>(i)];
        vpss_config.scale[static_cast<size_t>(i)] = scale_[static_cast<size_t>(i)];
      }
      std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor(
          new bmrt_runtime::VpssPreprocessor());
      if (preprocessor->open(session_.handle(), vpss_config, &hardware_error_)) {
        hardware_preprocessor_ = std::move(preprocessor);
      }
    } else {
      hardware_error_ = "require NCHW int8/uint8 model input";
    }
    return true;
  }

  bool inferImage(const std::string &image_path, const Box *roi,
                  AlgorithmResult *result, std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!result) {
      bmrt_runtime::setError(error, "result pointer is null");
      return false;
    }

    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return false;
    }

    cv::Mat cropped = image;
    if (roi) {
      cropped = image(bmrt_runtime::clampRoi(*roi, image.cols, image.rows)).clone();
    }

    cv::Mat resized;
    cv::resize(cropped, resized,
               cv::Size(session_.inputWidth(), session_.inputHeight()), 0, 0,
               cv::INTER_LINEAR);

    std::vector<float> input_tensor;
    bmrt_runtime::writeImageToTensor(
        resized, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }

    *result = AlgorithmResult{};
    decode(outputs, result);
    return true;
  }

  bool inferFrame(const Frame &frame, const Box *roi, AlgorithmResult *result,
                  std::string *error) {
    std::lock_guard<std::mutex> lock(infer_mutex_);
    if (!result || !frame.native) {
      bmrt_runtime::setError(error, "face attribute frame/result pointer is null");
      return false;
    }
    if (!hardware_preprocessor_) {
      const std::string reason = hardware_error_.empty()
                                     ? "VPSS preprocessor is unavailable"
                                     : hardware_error_;
      bmrt_runtime::setError(error,
                             "face attribute VPSS input is unavailable: " + reason);
      return false;
    }
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    bmrt_runtime::VpssPreprocessor::Roi hardware_roi;
    if (roi &&
        !toVpssRoi(*roi, static_cast<int>(video->stVFrame.u32Width),
                   static_cast<int>(video->stVFrame.u32Height), &hardware_roi,
                   error)) {
      return false;
    }
    if (!hardware_preprocessor_->preprocess(frame.native,
                                             roi ? &hardware_roi : nullptr,
                                             error)) {
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launchDevice(hardware_preprocessor_->inputMemory(), &outputs,
                               error)) {
      return false;
    }
    *result = AlgorithmResult{};
    decode(outputs, result);
    return true;
  }

 private:
  bool mapOutputs() {
    gender_index_ = -1;
    age_index_ = -1;
    glass_index_ = -1;
    mask_index_ = -1;
    emotion_index_ = -1;

    const bm_net_info_t *net_info = session_.netInfo();
    std::vector<int> scalar_candidates;
    for (int i = 0; i < net_info->output_num; ++i) {
      const std::string name = bmrt_runtime::toUpper(net_info->output_names[i]);
      if (name.find("GENDER") != std::string::npos) {
        gender_index_ = i;
        continue;
      }
      if (name.find("AGE") != std::string::npos) {
        age_index_ = i;
        continue;
      }
      if (name.find("GLASS") != std::string::npos) {
        glass_index_ = i;
        continue;
      }
      if (name.find("MASK") != std::string::npos) {
        mask_index_ = i;
        continue;
      }
      if (name.find("EMOTION") != std::string::npos) {
        emotion_index_ = i;
        continue;
      }

      const bm_shape_t &shape = net_info->stages[0].output_shapes[i];
      size_t element_count = 1;
      for (int d = 1; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      if (element_count == 7) {
        emotion_index_ = i;
      } else if (element_count == 1) {
        scalar_candidates.push_back(i);
      }
    }

    auto assignScalar = [&](int *target) {
      if (*target >= 0 || scalar_candidates.empty()) {
        return;
      }
      *target = scalar_candidates.front();
      scalar_candidates.erase(scalar_candidates.begin());
    };

    assignScalar(&gender_index_);
    assignScalar(&age_index_);
    assignScalar(&glass_index_);
    assignScalar(&mask_index_);
    return true;
  }

  void decode(const std::vector<bmrt_runtime::OutputTensor> &outputs,
              AlgorithmResult *result) const {
    if (gender_index_ >= 0 &&
        !outputs[static_cast<size_t>(gender_index_)].data.empty()) {
      addAttribute(result, "gender",
                   outputs[static_cast<size_t>(gender_index_)].data[0]);
    }
    if (age_index_ >= 0 && !outputs[static_cast<size_t>(age_index_)].data.empty()) {
      addAttribute(result, "age", outputs[static_cast<size_t>(age_index_)].data[0]);
    }
    if (glass_index_ >= 0 &&
        !outputs[static_cast<size_t>(glass_index_)].data.empty()) {
      addAttribute(result, "glasses",
                   outputs[static_cast<size_t>(glass_index_)].data[0]);
    }
    if (mask_index_ >= 0 &&
        !outputs[static_cast<size_t>(mask_index_)].data.empty()) {
      addAttribute(result, "mask", outputs[static_cast<size_t>(mask_index_)].data[0]);
    }
    if (emotion_index_ >= 0 &&
        !outputs[static_cast<size_t>(emotion_index_)].data.empty()) {
      const auto &data = outputs[static_cast<size_t>(emotion_index_)].data;
      const auto it = std::max_element(data.begin(), data.end());
      addAttribute(result, "emotion",
                   static_cast<float>(std::distance(data.begin(), it)));
    }
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::string hardware_error_;
  std::mutex infer_mutex_;
  int gender_index_ = -1;
  int age_index_ = -1;
  int glass_index_ = -1;
  int mask_index_ = -1;
  int emotion_index_ = -1;
};

NnFaceAttribute::NnFaceAttribute(std::string model_type)
    : model_type_(std::move(model_type)) {}

NnFaceAttribute::~NnFaceAttribute() = default;

TaskType NnFaceAttribute::task() const { return TaskType::FaceAttribute; }

std::string NnFaceAttribute::modelType() const { return model_type_; }

bool NnFaceAttribute::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    bmrt_runtime::setError(
        error,
        "face attribute runtime requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnFaceAttribute::load(EngineConfig config, std::string *error) {
  initialized_ = false;
  custom_runtime_.reset();
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
    return false;
  }
  custom_runtime_.reset(new CustomRuntime());
  if (!custom_runtime_->open(config_, descriptor_, error)) {
    custom_runtime_.reset();
    return false;
  }
  initialized_ = true;
  return true;
}

bool NnFaceAttribute::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnFaceAttribute::predict(const std::string &image_path,
                              const InferOptions &options,
                              AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnFaceAttribute::predictFrame(const Frame &frame,
                                   const InferOptions &options,
                                   AlgorithmResult *result,
                                   std::string *error) {
  (void)options;
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, nullptr, result, error);
  }
  if (frame.native) {
    return custom_runtime_->inferFrame(frame, nullptr, result, error);
  }
  bmrt_runtime::setError(error,
                         "face attribute frame has neither native data nor image_path");
  return false;
}

bool NnFaceAttribute::predictFrameCrop(const Frame &frame, const Box &roi,
                                       const InferOptions &options,
                                       AlgorithmResult *result,
                                       std::string *error) {
  (void)options;
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  if (!frame.native) {
    bmrt_runtime::setError(error,
                           "face attribute crop requires a native frame");
    return false;
  }
  return custom_runtime_->inferFrame(frame, &roi, result, error);
}

bool NnFaceAttribute::predictCrop(const std::string &image_path, const Box &roi,
                                  const InferOptions &options,
                                  AlgorithmResult *result,
                                  std::string *error) {
  (void)options;
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  return custom_runtime_->inferImage(image_path, &roi, result, error);
}

}  // namespace tdl_app
