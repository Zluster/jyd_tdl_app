#include "tdl_app/nn_scrfd.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"

namespace tdl_app {
namespace {

struct ScrfdBranch {
  int stride = 0;
  int feat_w = 0;
  int feat_h = 0;
  int num_anchors = 2;
  int score_index = -1;
  int bbox_index = -1;
  int landmark_index = -1;
};

float intersectionOverUnion(const Box &lhs, const Box &rhs) {
  const float x1 = std::max(lhs.x1, rhs.x1);
  const float y1 = std::max(lhs.y1, rhs.y1);
  const float x2 = std::min(lhs.x2, rhs.x2);
  const float y2 = std::min(lhs.y2, rhs.y2);
  const float w = std::max(0.0f, x2 - x1);
  const float h = std::max(0.0f, y2 - y1);
  const float inter = w * h;
  const float area_l =
      std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
  const float area_r =
      std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
  const float denom = area_l + area_r - inter;
  return denom <= 0.0f ? 0.0f : inter / denom;
}

std::vector<Box> nonMaxSuppression(const std::vector<Box> &boxes,
                                   float iou_threshold) {
  std::vector<int> order(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    order[i] = static_cast<int>(i);
  }
  std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
    return boxes[lhs].score > boxes[rhs].score;
  });

  std::vector<Box> kept;
  std::vector<bool> removed(boxes.size(), false);
  for (size_t i = 0; i < order.size(); ++i) {
    const int index = order[i];
    if (removed[index]) {
      continue;
    }
    kept.push_back(boxes[index]);
    for (size_t j = i + 1; j < order.size(); ++j) {
      const int other = order[j];
      if (removed[other]) {
        continue;
      }
      if (intersectionOverUnion(boxes[index], boxes[other]) > iou_threshold) {
        removed[other] = true;
      }
    }
  }
  return kept;
}

}  // namespace

class NnScrfd::CustomRuntime {
 public:
  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    descriptor_ = descriptor;
    if (!session_.open(config, descriptor, error)) {
      return false;
    }
    mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 127.5f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f / 128.0f);
    return buildBranches(error);
  }

  bool inferImage(const std::string &image_path, const InferOptions &options,
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

    std::vector<float> input_tensor;
    float ratio = 1.0f;
    int top = 0;
    int left = 0;
    preprocess(image, &input_tensor, &ratio, &top, &left);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launch(input_tensor, &outputs, error)) {
      return false;
    }

    *result = AlgorithmResult{};
    result->boxes = decode(outputs, image.cols, image.rows, ratio, top, left,
                           options.threshold, options.iou_threshold);
    return true;
  }

 private:
  bool buildBranches(std::string *error) {
    branches_.clear();
    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];

    const int strides[] = {8, 16, 32};
    for (int stride : strides) {
      ScrfdBranch branch;
      branch.stride = stride;
      branch.feat_w = static_cast<int>(
          std::ceil(static_cast<float>(session_.inputWidth()) / stride));
      branch.feat_h = static_cast<int>(
          std::ceil(static_cast<float>(session_.inputHeight()) / stride));

      for (int i = 0; i < net_info->output_num; ++i) {
        const bm_shape_t &shape = stage.output_shapes[i];
        if (shape.num_dims != 4 || shape.dims[2] != branch.feat_h ||
            shape.dims[3] != branch.feat_w) {
          continue;
        }
        if (shape.dims[1] == branch.num_anchors) {
          branch.score_index = i;
        } else if (shape.dims[1] == branch.num_anchors * 4) {
          branch.bbox_index = i;
        } else if (shape.dims[1] == branch.num_anchors * 10) {
          branch.landmark_index = i;
        }
      }

      if (branch.score_index < 0 || branch.bbox_index < 0 ||
          branch.landmark_index < 0) {
        bmrt_runtime::setError(error, "incomplete SCRFD output branches");
        return false;
      }
      branches_.push_back(branch);
    }
    return true;
  }

  void preprocess(const cv::Mat &image, std::vector<float> *tensor, float *ratio,
                  int *top, int *left) const {
    *ratio = std::min(static_cast<float>(session_.inputHeight()) / image.rows,
                      static_cast<float>(session_.inputWidth()) / image.cols);
    const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
    const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));
    *top = (session_.inputHeight() - resized_h) / 2;
    *left = (session_.inputWidth() - resized_w) / 2;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0,
               cv::INTER_LINEAR);

    cv::Mat padded(session_.inputHeight(), session_.inputWidth(), CV_8UC3,
                   cv::Scalar(0, 0, 0));
    resized.copyTo(padded(cv::Rect(*left, *top, resized_w, resized_h)));

    bmrt_runtime::writeImageToTensor(
        padded, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  std::vector<Box> decode(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                          int image_width, int image_height, float ratio, int top,
                          int left, float threshold, float iou_threshold) const {
    constexpr int kLandmarkCount = 5;
    std::vector<Box> boxes;

    for (const ScrfdBranch &branch : branches_) {
      const auto &score = outputs[static_cast<size_t>(branch.score_index)].data;
      const auto &bbox = outputs[static_cast<size_t>(branch.bbox_index)].data;
      const auto &landmark =
          outputs[static_cast<size_t>(branch.landmark_index)].data;
      const int count = branch.feat_w * branch.feat_h;

      for (int anchor = 0; anchor < branch.num_anchors; ++anchor) {
        for (int index = 0; index < count; ++index) {
          const float conf = score[static_cast<size_t>(index + count * anchor)];
          if (conf < threshold) {
            continue;
          }

          const int grid_y = index / branch.feat_w;
          const int grid_x = index % branch.feat_w;
          const float center_x = static_cast<float>(grid_x * branch.stride);
          const float center_y = static_cast<float>(grid_y * branch.stride);

          const float x1 = center_x - bbox[static_cast<size_t>(
                                            index + count * (anchor * 4 + 0))] *
                                            branch.stride;
          const float y1 = center_y - bbox[static_cast<size_t>(
                                            index + count * (anchor * 4 + 1))] *
                                            branch.stride;
          const float x2 = center_x + bbox[static_cast<size_t>(
                                            index + count * (anchor * 4 + 2))] *
                                            branch.stride;
          const float y2 = center_y + bbox[static_cast<size_t>(
                                            index + count * (anchor * 4 + 3))] *
                                            branch.stride;
          if (x1 >= x2 || y1 >= y2) {
            continue;
          }

          Box box;
          box.class_id = 0;
          box.score = conf;
          box.x1 = (x1 - left) / ratio;
          box.y1 = (y1 - top) / ratio;
          box.x2 = (x2 - left) / ratio;
          box.y2 = (y2 - top) / ratio;
          box.x1 = std::max(0.0f, std::min(box.x1, static_cast<float>(image_width)));
          box.y1 =
              std::max(0.0f, std::min(box.y1, static_cast<float>(image_height)));
          box.x2 = std::max(0.0f, std::min(box.x2, static_cast<float>(image_width)));
          box.y2 =
              std::max(0.0f, std::min(box.y2, static_cast<float>(image_height)));

          for (int k = 0; k < kLandmarkCount; ++k) {
            Point point;
            point.x =
                (landmark[static_cast<size_t>(index +
                                              count * (anchor * 10 + k * 2))] *
                     branch.stride +
                 center_x - left) /
                ratio;
            point.y = (landmark[static_cast<size_t>(
                                   index + count * (anchor * 10 + k * 2 + 1))] *
                           branch.stride +
                       center_y - top) /
                      ratio;
            point.x = std::max(0.0f,
                               std::min(point.x, static_cast<float>(image_width)));
            point.y =
                std::max(0.0f, std::min(point.y, static_cast<float>(image_height)));
            box.landmarks.push_back(point);
          }
          boxes.push_back(box);
        }
      }
    }

    return nonMaxSuppression(boxes, iou_threshold > 0.0f ? iou_threshold : 0.5f);
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::vector<ScrfdBranch> branches_;
};

NnScrfd::NnScrfd(std::string model_type) : model_type_(std::move(model_type)) {}

NnScrfd::~NnScrfd() = default;

TaskType NnScrfd::task() const { return TaskType::FaceDetection; }

std::string NnScrfd::modelType() const { return model_type_; }

bool NnScrfd::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    bmrt_runtime::setError(error,
                           "SCRFD runtime requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnScrfd::load(EngineConfig config, std::string *error) {
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

bool NnScrfd::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnScrfd::predict(const std::string &image_path, const InferOptions &options,
                      AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnScrfd::predictFrame(const Frame &frame, const InferOptions &options,
                           AlgorithmResult *result, std::string *error) {
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  if (frame.image_path.empty()) {
    bmrt_runtime::setError(error, "SCRFD runtime currently supports image_path only");
    return false;
  }
  return custom_runtime_->inferImage(frame.image_path, options, result, error);
}

}  // namespace tdl_app
