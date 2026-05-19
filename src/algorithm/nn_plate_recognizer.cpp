#include "tdl_app/nn_plate_recognizer.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"

namespace tdl_app {
namespace {

const std::vector<std::string> kPlateChars = {
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙",
    "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵",
    "云", "藏", "陕", "甘", "青", "宁", "新", "学", "警", "港", "澳", "挂",
    "使", "领", "民", "深", "危", "险", "空", "0",  "1",  "2",  "3",  "4",
    "5",  "6",  "7",  "8",  "9",  "A",  "B",  "C",  "D",  "E",  "F",  "G",
    "H",  "J",  "K",  "L",  "M",  "N",  "P",  "Q",  "R",  "S",  "T",  "U",
    "V",  "W",  "X",  "Y",  "Z",  "I",  "O",  "-"};

}  // namespace

class NnPlateRecognizer::CustomRuntime {
 public:
  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    descriptor_ = descriptor;
    if (!session_.open(config, descriptor, error)) {
      return false;
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 127.5f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f / 128.0f);
    return true;
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
        resized, bmrt_runtime::wantsRgbInput(descriptor_, false),
        session_.nchwLayout(), mean_, scale_, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (outputs.empty()) {
      bmrt_runtime::setError(error, "plate recognizer produced no outputs");
      return false;
    }

    *result = AlgorithmResult{};
    result->text = greedyDecode(outputs[0]);
    return true;
  }

 private:
  std::string greedyDecode(const bmrt_runtime::OutputTensor &tensor) const {
    if (tensor.shape.num_dims < 2 || tensor.data.empty()) {
      return std::string();
    }

    const int cols = tensor.shape.dims[tensor.shape.num_dims - 1];
    int rows = 1;
    for (int d = 1; d < tensor.shape.num_dims - 1; ++d) {
      rows *= tensor.shape.dims[d];
    }
    if (cols <= 0 || rows <= 0) {
      return std::string();
    }

    std::vector<int> index(static_cast<size_t>(cols), 0);
    for (int col = 0; col < cols; ++col) {
      float max_value = tensor.data[static_cast<size_t>(col)];
      int max_index = 0;
      for (int row = 1; row < rows; ++row) {
        const float value = tensor.data[static_cast<size_t>(col + row * cols)];
        if (value > max_value) {
          max_value = value;
          max_index = row;
        }
      }
      index[static_cast<size_t>(col)] = max_index;
    }

    std::vector<int> dedup;
    int prev = index[0];
    if (prev >= 0 && prev < static_cast<int>(kPlateChars.size()) &&
        prev != static_cast<int>(kPlateChars.size() - 1)) {
      dedup.push_back(prev);
    }
    for (int col = 1; col < cols; ++col) {
      const int current = index[static_cast<size_t>(col)];
      if (current == prev || current == static_cast<int>(kPlateChars.size() - 1)) {
        if (current == static_cast<int>(kPlateChars.size() - 1)) {
          prev = current;
        }
        continue;
      }
      dedup.push_back(current);
      prev = current;
    }

    std::string text;
    for (int value : dedup) {
      if (value >= 0 && value < static_cast<int>(kPlateChars.size())) {
        text += kPlateChars[static_cast<size_t>(value)];
      }
    }
    return text;
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
};

NnPlateRecognizer::NnPlateRecognizer(std::string model_type)
    : model_type_(std::move(model_type)) {}

NnPlateRecognizer::~NnPlateRecognizer() = default;

TaskType NnPlateRecognizer::task() const { return TaskType::Ocr; }

std::string NnPlateRecognizer::modelType() const { return model_type_; }

bool NnPlateRecognizer::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    bmrt_runtime::setError(
        error,
        "plate recognizer requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnPlateRecognizer::load(EngineConfig config, std::string *error) {
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

bool NnPlateRecognizer::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnPlateRecognizer::predict(const std::string &image_path,
                                const InferOptions &options,
                                AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnPlateRecognizer::predictFrame(const Frame &frame,
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
                           "plate recognizer runtime currently supports image_path only");
    return false;
  }
  return custom_runtime_->inferImage(frame.image_path, nullptr, result, error);
}

bool NnPlateRecognizer::predictCrop(const std::string &image_path, const Box &roi,
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
