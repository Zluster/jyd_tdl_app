#include "tdl_app/nn_face_attribute.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"

namespace tdl_app {
namespace {

void addAttribute(AlgorithmResult *result, const std::string &name, float value) {
  Attribute attr;
  attr.name = name;
  attr.value = value;
  result->attributes.push_back(attr);
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
    return mapOutputs();
  }

  bool inferImage(const std::string &image_path, const Box *roi,
                  AlgorithmResult *result, std::string *error) {
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
  if (frame.image_path.empty()) {
    bmrt_runtime::setError(error,
                           "face attribute runtime currently supports image_path only");
    return false;
  }
  return custom_runtime_->inferImage(frame.image_path, nullptr, result, error);
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
