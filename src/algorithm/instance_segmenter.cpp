#include "tdl_app/instance_segmenter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/vpss_preprocessor.hpp"

namespace tdl_app {
namespace {

constexpr int kRegMax = 16;
constexpr int kBoxChannels = 4 * kRegMax;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string resolveModelType(const InstanceSegmenter::Config &config,
                             const std::string &requested_model_type,
                             std::string *error) {
  const std::string model_type =
      !config.model_type.empty() ? config.model_type
      : !requested_model_type.empty() ? requested_model_type : "YOLOV8_SEG_COCO80";
  if (model_type.empty()) {
    setError(error, "instance segmentation model_type is empty");
  }
  return model_type;
}

float sigmoid(float value) {
  return 1.0f / (1.0f + std::exp(-value));
}

bool segDebugEnabled() {
  const char *value = std::getenv("TDL_APP_SEG_DEBUG");
  if (!value) {
    return false;
  }
  return std::string(value) != "0";
}

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

std::vector<int> nonMaxSuppression(const std::vector<Box> &boxes,
                                   float iou_threshold) {
  std::vector<int> order(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    order[i] = static_cast<int>(i);
  }
  std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
    return boxes[lhs].score > boxes[rhs].score;
  });

  std::vector<int> kept;
  std::vector<bool> removed(boxes.size(), false);
  for (size_t i = 0; i < order.size(); ++i) {
    const int index = order[i];
    if (removed[index]) {
      continue;
    }
    kept.push_back(index);
    for (size_t j = i + 1; j < order.size(); ++j) {
      const int other = order[j];
      if (removed[other]) {
        continue;
      }
      if (boxes[index].class_id != boxes[other].class_id) {
        continue;
      }
      if (intersectionOverUnion(boxes[index], boxes[other]) > iou_threshold) {
        removed[other] = true;
      }
    }
  }
  return kept;
}

std::vector<float> decodeDfl(const std::vector<float> &bbox, int anchor_index,
                             int anchor_count) {
  std::vector<float> values(4, 0.0f);
  for (int side = 0; side < 4; ++side) {
    const int offset = side * kRegMax;
    float max_logit = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < kRegMax; ++i) {
      max_logit = std::max(max_logit,
                           bbox[(offset + i) * anchor_count + anchor_index]);
    }
    float sum = 0.0f;
    float weighted = 0.0f;
    for (int i = 0; i < kRegMax; ++i) {
      const float expv = std::exp(
          bbox[(offset + i) * anchor_count + anchor_index] - max_logit);
      sum += expv;
      weighted += expv * static_cast<float>(i);
    }
    values[side] = sum > 0.0f ? weighted / sum : 0.0f;
  }
  return values;
}

struct InstanceSegRuntime {
  virtual ~InstanceSegRuntime() = default;
  virtual bool run(const std::string &image_path,
                   InstanceSegmentationResult *result,
                   std::string *error) = 0;
  virtual bool runFrame(const Frame &frame, InstanceSegmentationResult *result,
                        std::string *error) = 0;
  virtual void reset() = 0;
  virtual bool initialized() const = 0;
};

class YoloV8SegRuntime : public InstanceSegRuntime {
 public:
  bool open(const InstanceSegmenter::Config &config,
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
      descriptor_.runtime = "instance_segmentation";
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

    mean_ = bmrt_runtime::expandChannelValues(descriptor_.mean, 0.0f);
    scale_ = bmrt_runtime::expandChannelValues(descriptor_.scale, 1.0f / 255.0f);
    if (!buildOutputs(error)) {
      session_.close();
      return false;
    }

    if (!session_.nchwLayout() ||
        (session_.inputDtype() != BM_INT8 &&
         session_.inputDtype() != BM_UINT8)) {
      setError(error, "instance segmentation VPSS path requires NCHW INT8/UINT8 input");
      session_.close();
      return false;
    }
    bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = session_.inputWidth();
    vpss_config.height = session_.inputHeight();
    vpss_config.rgb = bmrt_runtime::wantsRgbInput(descriptor_, true);
    vpss_config.keep_aspect_ratio = true;
    vpss_config.padding = {{114, 114, 114}};
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

  bool run(const std::string &image_path, InstanceSegmentationResult *result,
           std::string *error) override {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      setError(error, "failed to read image: " + image_path);
      return false;
    }
    return infer(image, result, error);
  }

  bool runFrame(const Frame &frame, InstanceSegmentationResult *result,
                std::string *error) override {
    if (!frame.image_path.empty()) {
      return run(frame.image_path, result, error);
    }
    if (!result || !frame.native) {
      setError(error, "instance segmentation frame/result pointer is null");
      return false;
    }
    if (!preprocessor_) {
      setError(error, "instance segmentation VPSS preprocessor is unavailable");
      return false;
    }
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    const int source_width = static_cast<int>(video->stVFrame.u32Width);
    const int source_height = static_cast<int>(video->stVFrame.u32Height);
    if (source_width <= 0 || source_height <= 0) {
      setError(error, "instance segmentation source frame dimensions are invalid");
      return false;
    }
    const float ratio = std::min(
        static_cast<float>(session_.inputHeight()) / source_height,
        static_cast<float>(session_.inputWidth()) / source_width);
    const int top = (session_.inputHeight() -
                     static_cast<int>(std::round(source_height * ratio))) / 2;
    const int left = (session_.inputWidth() -
                      static_cast<int>(std::round(source_width * ratio))) / 2;
    if (!preprocessor_->preprocess(video, error)) {
      return false;
    }
    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!session_.launchDevice(preprocessor_->inputMemory(), &outputs, error)) {
      return false;
    }
    return decodeOutputs(outputs, source_width, source_height, ratio, top, left,
                         result, error);
  }

  void reset() override {
    preprocessor_.reset();
    session_.close();
  }
  bool initialized() const override { return session_.opened(); }

 private:
  struct Branch {
    int stride = 0;
    int feat_w = 0;
    int feat_h = 0;
    int bbox_index = -1;
    int cls_index = -1;
    int det_index = -1;
    int coeff_index = -1;
    int cls_offset = 0;
    int coeff_offset = 0;
  };

  struct Candidate {
    Box box;
    std::vector<float> coeffs;
  };

  bool buildOutputs(std::string *error) {
    branches_.clear();
    const bm_net_info_t *net_info = session_.netInfo();
    const auto &stage = net_info->stages[0];
    num_classes_ = static_cast<int>(descriptor_.labels.size());
    proto_index_ = -1;
    mask_channels_ = 0;
    proto_width_ = 0;
    proto_height_ = 0;

    struct TempBranch {
      int stride = 0;
      int feat_w = 0;
      int feat_h = 0;
      int bbox_index = -1;
      int cls_index = -1;
      int det_index = -1;
      int coeff_index = -1;
      int cls_offset = 0;
      int coeff_offset = 0;
    };
    std::vector<TempBranch> temp;

    // First pass: locate proto output. For YOLOv8/11 seg exported heads this is
    // the largest non-input-resolution feature map with mask channel count.
    int best_proto_area = -1;
    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (segDebugEnabled()) {
        std::cout << "seg debug: output[" << i << "] shape=";
        for (int d = 0; d < shape.num_dims; ++d) {
          std::cout << (d == 0 ? "[" : ",") << shape.dims[d];
        }
        std::cout << "]\n";
      }
      if (shape.num_dims != 4) {
        continue;
      }
      const int channel = shape.dims[1];
      const int feat_h = shape.dims[2];
      const int feat_w = shape.dims[3];
      if (feat_h <= 0 || feat_w <= 0 || channel <= 0) {
        continue;
      }
      if (feat_h >= session_.inputHeight() || feat_w >= session_.inputWidth()) {
        continue;
      }
      const int stride_h = session_.inputHeight() / feat_h;
      const int stride_w = session_.inputWidth() / feat_w;
      const bool is_detection_branch =
          stride_h == stride_w && (stride_h == 8 || stride_h == 16 || stride_h == 32);
      if (is_detection_branch) {
        continue;
      }
      const int area = feat_h * feat_w;
      if (area > best_proto_area) {
        best_proto_area = area;
        proto_index_ = i;
        proto_width_ = feat_w;
        proto_height_ = feat_h;
        mask_channels_ = channel;
        if (segDebugEnabled()) {
          std::cout << "seg debug: proto output=" << i
                    << " channels=" << mask_channels_
                    << " size=" << proto_width_ << "x" << proto_height_ << "\n";
        }
      }
    }

    for (int i = 0; i < net_info->output_num; ++i) {
      const bm_shape_t &shape = stage.output_shapes[i];
      if (shape.num_dims != 4) {
        continue;
      }
      const int channel = shape.dims[1];
      const int feat_h = shape.dims[2];
      const int feat_w = shape.dims[3];
      if (feat_h <= 0 || feat_w <= 0) {
        setError(error, "invalid instance segmentation output shape");
        return false;
      }

      const int stride_h = session_.inputHeight() / feat_h;
      const int stride_w = session_.inputWidth() / feat_w;
      if (stride_h == stride_w && stride_h > 0 &&
          (stride_h == 8 || stride_h == 16 || stride_h == 32)) {
        auto it = std::find_if(temp.begin(), temp.end(), [&](const TempBranch &branch) {
          return branch.stride == stride_h;
        });
        if (it == temp.end()) {
          temp.push_back(
              TempBranch{stride_h, feat_w, feat_h, -1, -1, -1, -1, 0, 0});
          it = temp.end() - 1;
        }

        if (channel == kBoxChannels) {
          it->bbox_index = i;
          if (segDebugEnabled()) {
            std::cout << "seg debug: stride=" << stride_h << " bbox_index=" << i << "\n";
          }
        } else if (num_classes_ > 0 && channel == num_classes_) {
          it->cls_index = i;
          it->cls_offset = 0;
          if (segDebugEnabled()) {
            std::cout << "seg debug: stride=" << stride_h << " cls_index=" << i << "\n";
          }
        } else if (mask_channels_ > 0 && channel == mask_channels_) {
          it->coeff_index = i;
          it->coeff_offset = 0;
          if (segDebugEnabled()) {
            std::cout << "seg debug: stride=" << stride_h << " coeff_index=" << i << "\n";
          }
        } else if (num_classes_ > 0 && channel == kBoxChannels + num_classes_) {
          it->det_index = i;
          if (segDebugEnabled()) {
            std::cout << "seg debug: stride=" << stride_h << " det(box+cls)_index=" << i << "\n";
          }
        } else if (num_classes_ > 0 && channel > kBoxChannels + num_classes_) {
          if (mask_channels_ == 0 && proto_index_ < 0) {
            mask_channels_ = channel - (kBoxChannels + num_classes_);
          }
          if (channel == kBoxChannels + num_classes_ + mask_channels_) {
            it->det_index = i;
            it->bbox_index = i;
            it->cls_index = i;
            it->coeff_index = i;
            it->cls_offset = kBoxChannels;
            it->coeff_offset = kBoxChannels + num_classes_;
            if (segDebugEnabled()) {
              std::cout << "seg debug: stride=" << stride_h
                        << " det(box+cls+coeff)_index=" << i << "\n";
            }
          }
        }
        continue;
      }
    }

    if (proto_index_ < 0 || mask_channels_ <= 0) {
      setError(error, "unable to locate segmentation prototype output");
      return false;
    }

    if (num_classes_ <= 0) {
      for (const auto &branch : temp) {
        if (branch.det_index < 0) {
          continue;
        }
        const int det_channels =
            stage.output_shapes[branch.det_index].dims[1];
        if (det_channels > kBoxChannels + mask_channels_) {
          num_classes_ = det_channels - kBoxChannels - mask_channels_;
          break;
        }
      }
    }
    if (num_classes_ <= 0) {
      setError(error, "unable to infer segmentation class count");
      return false;
    }

    for (auto &branch : temp) {
      if (branch.det_index < 0 && branch.bbox_index < 0) {
        continue;
      }
      if (branch.det_index >= 0) {
        const int det_channels = stage.output_shapes[branch.det_index].dims[1];
        if (det_channels == kBoxChannels + num_classes_) {
          branch.bbox_index = branch.det_index;
          branch.cls_index = branch.det_index;
          branch.cls_offset = kBoxChannels;
        } else if (det_channels == kBoxChannels + num_classes_ + mask_channels_) {
          branch.bbox_index = branch.det_index;
          branch.cls_index = branch.det_index;
          branch.coeff_index = branch.det_index;
          branch.cls_offset = kBoxChannels;
          branch.coeff_offset = kBoxChannels + num_classes_;
        }
      }

      if (branch.bbox_index < 0 || branch.cls_index < 0 || branch.coeff_index < 0) {
        setError(error, "incomplete segmentation output branches");
        return false;
      }
      branches_.push_back(
          Branch{branch.stride, branch.feat_w, branch.feat_h, branch.bbox_index,
                 branch.cls_index, branch.det_index, branch.coeff_index,
                 branch.cls_offset, branch.coeff_offset});
      if (segDebugEnabled()) {
        std::cout << "seg debug: branch stride=" << branch.stride
                  << " feat=" << branch.feat_w << "x" << branch.feat_h
                  << " bbox=" << branch.bbox_index
                  << " cls=" << branch.cls_index
                  << " coeff=" << branch.coeff_index
                  << " cls_offset=" << branch.cls_offset
                  << " coeff_offset=" << branch.coeff_offset << "\n";
      }
    }

    if (branches_.empty()) {
      setError(error, "no segmentation decode branches found");
      return false;
    }

    std::sort(branches_.begin(), branches_.end(),
              [](const Branch &lhs, const Branch &rhs) {
                return lhs.stride < rhs.stride;
              });
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
                   cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(*left, *top, resized_w, resized_h)));

    bmrt_runtime::writeImageToTensor(
        padded, bmrt_runtime::wantsRgbInput(descriptor_, true),
        session_.nchwLayout(), mean_, scale_, tensor);
  }

  std::vector<Candidate> decodeCandidates(
      const std::vector<bmrt_runtime::OutputTensor> &outputs, int image_width,
      int image_height, float ratio, int top, int left) const {
    std::vector<Candidate> candidates;

    for (const Branch &branch : branches_) {
      const auto &bbox = outputs[static_cast<size_t>(branch.bbox_index)].data;
      const auto &cls_out =
          outputs[static_cast<size_t>(branch.cls_index)].data;
      const auto &coeff = outputs[static_cast<size_t>(branch.coeff_index)].data;
      const int anchor_count = branch.feat_w * branch.feat_h;

      for (int anchor = 0; anchor < anchor_count; ++anchor) {
        int best_class = -1;
        float best_logit = -std::numeric_limits<float>::infinity();
        for (int cls_id = 0; cls_id < num_classes_; ++cls_id) {
          const float logit =
              cls_out[(branch.cls_offset + cls_id) * anchor_count + anchor];
          if (logit > best_logit) {
            best_logit = logit;
            best_class = cls_id;
          }
        }
        const float score = sigmoid(best_logit);
        if (best_class < 0 || score < 0.25f) {
          continue;
        }

        const std::vector<float> distances =
            decodeDfl(bbox, anchor, anchor_count);
        const int anchor_y = anchor / branch.feat_w;
        const int anchor_x = anchor % branch.feat_w;
        const float grid_x = static_cast<float>(anchor_x) + 0.5f;
        const float grid_y = static_cast<float>(anchor_y) + 0.5f;
        const float x1 = (grid_x - distances[0]) * branch.stride;
        const float y1 = (grid_y - distances[1]) * branch.stride;
        const float x2 = (grid_x + distances[2]) * branch.stride;
        const float y2 = (grid_y + distances[3]) * branch.stride;
        if (x2 <= x1 || y2 <= y1) {
          continue;
        }

        Candidate candidate;
        candidate.box.class_id = best_class;
        candidate.box.score = score;
        candidate.box.x1 =
            std::max(0.0f, std::min((x1 - left) / ratio,
                                    static_cast<float>(image_width)));
        candidate.box.y1 =
            std::max(0.0f, std::min((y1 - top) / ratio,
                                    static_cast<float>(image_height)));
        candidate.box.x2 =
            std::max(0.0f, std::min((x2 - left) / ratio,
                                    static_cast<float>(image_width)));
        candidate.box.y2 =
            std::max(0.0f, std::min((y2 - top) / ratio,
                                    static_cast<float>(image_height)));
        candidate.coeffs.resize(static_cast<size_t>(mask_channels_), 0.0f);
        for (int c = 0; c < mask_channels_; ++c) {
          candidate.coeffs[static_cast<size_t>(c)] =
              coeff[(branch.coeff_offset + c) * anchor_count + anchor];
        }
        candidates.push_back(std::move(candidate));
      }
    }

    return candidates;
  }

  cv::Mat buildMask(const std::vector<float> &coeffs,
                    const bmrt_runtime::OutputTensor &proto) const {
    cv::Mat mask(proto_height_, proto_width_, CV_32FC1, cv::Scalar(0));
    const int plane_size = proto_height_ * proto_width_;
    for (int y = 0; y < proto_height_; ++y) {
      float *row = mask.ptr<float>(y);
      for (int x = 0; x < proto_width_; ++x) {
        float value = 0.0f;
        const int index = y * proto_width_ + x;
        for (int c = 0; c < mask_channels_; ++c) {
          value += coeffs[static_cast<size_t>(c)] *
                   proto.data[static_cast<size_t>(c * plane_size + index)];
        }
        row[x] = sigmoid(value);
      }
    }
    return mask;
  }

  bool infer(const cv::Mat &image, InstanceSegmentationResult *result,
             std::string *error) {
    if (!result) {
      setError(error, "instance segmentation result pointer is null");
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
    return decodeOutputs(outputs, image.cols, image.rows, ratio, top, left,
                         result, error);
  }

  bool decodeOutputs(const std::vector<bmrt_runtime::OutputTensor> &outputs,
                     int image_width, int image_height, float ratio, int top,
                     int left, InstanceSegmentationResult *result,
                     std::string *error) {
    if (proto_index_ < 0 || proto_index_ >= static_cast<int>(outputs.size())) {
      setError(error, "invalid proto output index");
      return false;
    }

    const std::vector<Candidate> candidates =
        decodeCandidates(outputs, image_width, image_height, ratio, top, left);
    std::vector<Box> raw_boxes;
    raw_boxes.reserve(candidates.size());
    for (const auto &candidate : candidates) {
      raw_boxes.push_back(candidate.box);
    }
    const std::vector<int> kept = nonMaxSuppression(raw_boxes, 0.45f);

    result->clear();
    result->width = image_width;
    result->height = image_height;
    result->mask_width = image_width;
    result->mask_height = image_height;
    result->instances.reserve(kept.size());

    const bmrt_runtime::OutputTensor &proto =
        outputs[static_cast<size_t>(proto_index_)];
    for (int index : kept) {
      const Candidate &candidate = candidates[static_cast<size_t>(index)];
      cv::Mat lowres = buildMask(candidate.coeffs, proto);
      cv::Mat resized;
      cv::resize(lowres, resized, cv::Size(session_.inputWidth(),
                                           session_.inputHeight()),
                 0, 0, cv::INTER_LINEAR);

      cv::Rect roi(left, top,
                   std::max(1, static_cast<int>(std::round(image_width * ratio))),
                   std::max(1, static_cast<int>(std::round(image_height * ratio))));
      roi &= cv::Rect(0, 0, resized.cols, resized.rows);
      if (roi.width <= 0 || roi.height <= 0) {
        continue;
      }

      cv::Mat cropped = resized(roi);
      cv::Mat full_mask;
      cv::resize(cropped, full_mask, cv::Size(image_width, image_height), 0, 0,
                 cv::INTER_LINEAR);

      cv::Mat binary;
      cv::threshold(full_mask, binary, 0.5, 255.0, cv::THRESH_BINARY);
      binary.convertTo(binary, CV_8UC1);

      cv::Mat object_only(binary.size(), CV_8UC1, cv::Scalar(0));
      const cv::Rect box_roi = bmrt_runtime::clampRoi(candidate.box, image_width,
                                                      image_height);
      binary(box_roi).copyTo(object_only(box_roi));

      InstanceSegment instance;
      instance.box = candidate.box;
      instance.mask.assign(object_only.data,
                           object_only.data + object_only.total());

      std::vector<std::vector<cv::Point>> contours;
      cv::findContours(object_only, contours, cv::RETR_EXTERNAL,
                       cv::CHAIN_APPROX_SIMPLE);
      if (!contours.empty()) {
        const auto best_it = std::max_element(
            contours.begin(), contours.end(),
            [](const std::vector<cv::Point> &lhs,
               const std::vector<cv::Point> &rhs) {
              return cv::contourArea(lhs) < cv::contourArea(rhs);
            });
        instance.outline.reserve(best_it->size());
        for (const auto &point : *best_it) {
          Point outline_point;
          outline_point.x = static_cast<float>(point.x);
          outline_point.y = static_cast<float>(point.y);
          outline_point.score = candidate.box.score;
          instance.outline.push_back(outline_point);
        }
      }

      result->instances.push_back(std::move(instance));
    }
    return true;
  }

  ModelDescriptor descriptor_;
  std::vector<float> mean_;
  std::vector<float> scale_;
  bmrt_runtime::Session session_;
  std::unique_ptr<bmrt_runtime::VpssPreprocessor> preprocessor_;
  std::vector<Branch> branches_;
  int proto_index_ = -1;
  int mask_channels_ = 0;
  int proto_width_ = 0;
  int proto_height_ = 0;
  int num_classes_ = 0;
};

}  // namespace

class InstanceSegmenterImpl {
 public:
  bool load(const InstanceSegmenter::Config &config,
            const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type =
        resolveModelType(config, requested_model_type, error);
    if (model_type.empty()) {
      return false;
    }

    runtime_.reset();
    const bool prefer_custom =
        bmrt_runtime::startsWith(bmrt_runtime::toUpper(model_type), "YOLOV8_SEG") ||
        bmrt_runtime::startsWith(bmrt_runtime::toUpper(model_type), "FASTSAM_SEG");
    if (prefer_custom) {
      std::unique_ptr<YoloV8SegRuntime> runtime(new YoloV8SegRuntime());
      if (!runtime->open(config, model_type, resolved_model_type, error)) {
        return false;
      }
      runtime_ = std::move(runtime);
      return true;
    }
    setError(
        error,
        "unsupported instance segmentation model_type for custom BMRT runtime: " +
            model_type);
    return false;
  }

  bool run(const std::string &image_path, InstanceSegmentationResult *result,
           std::string *error) {
    if (!runtime_) {
      setError(error, "instance segmenter is not initialized");
      return false;
    }
    return runtime_->run(image_path, result, error);
  }

  bool runFrame(const Frame &frame, InstanceSegmentationResult *result,
                std::string *error) {
    if (!runtime_) {
      setError(error, "instance segmenter is not initialized");
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
  std::unique_ptr<InstanceSegRuntime> runtime_;
};

InstanceSegmenter::InstanceSegmenter() = default;

InstanceSegmenter::InstanceSegmenter(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

InstanceSegmenter::~InstanceSegmenter() {
  reset();
  delete reinterpret_cast<InstanceSegmenterImpl *>(impl_);
}

InstanceSegmenter::InstanceSegmenter(InstanceSegmenter &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

InstanceSegmenter &InstanceSegmenter::operator=(
    InstanceSegmenter &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  delete reinterpret_cast<InstanceSegmenterImpl *>(impl_);
  requested_model_type_ = std::move(other.requested_model_type_);
  config_ = std::move(other.config_);
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool InstanceSegmenter::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = reinterpret_cast<Impl *>(new InstanceSegmenterImpl);
  }
  return reinterpret_cast<InstanceSegmenterImpl *>(impl_)->load(
      config_, requested_model_type_, &requested_model_type_, error);
}

bool InstanceSegmenter::load(const std::string &model_spec,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool InstanceSegmenter::load(const std::string &model_spec,
                             const std::string &firmware,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool InstanceSegmenter::load(const std::string &model_spec,
                             const std::string &firmware,
                             const std::string &model_dir,
                             std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool InstanceSegmenter::run(const std::string &image_path,
                            InstanceSegmentationResult *result,
                            std::string *error) {
  return segment(image_path, result, error);
}

bool InstanceSegmenter::runFrame(const Frame &frame,
                                 InstanceSegmentationResult *result,
                                 std::string *error) {
  if (!impl_) {
    setError(error, "instance segmenter is not initialized");
    return false;
  }
  return reinterpret_cast<InstanceSegmenterImpl *>(impl_)->runFrame(frame, result,
                                                                    error);
}

bool InstanceSegmenter::segment(const std::string &image_path,
                                InstanceSegmentationResult *result,
                                std::string *error) {
  if (!impl_) {
    setError(error, "instance segmenter is not initialized");
    return false;
  }
  return reinterpret_cast<InstanceSegmenterImpl *>(impl_)->run(image_path, result,
                                                               error);
}

bool InstanceSegmenter::initialized() const {
  return impl_ && reinterpret_cast<const InstanceSegmenterImpl *>(impl_)->initialized();
}

std::string InstanceSegmenter::modelType() const {
  return requested_model_type_;
}

void InstanceSegmenter::reset() {
  if (impl_) {
    reinterpret_cast<InstanceSegmenterImpl *>(impl_)->reset();
  }
}

}  // namespace tdl_app
