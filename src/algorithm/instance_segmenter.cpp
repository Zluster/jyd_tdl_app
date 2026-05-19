#include "tdl_app/instance_segmenter.hpp"

#include <cstring>
#include <utility>

#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_utils.h"
#include "algorithm/private/tdl_sdk_utils.hpp"

namespace tdl_app {

class InstanceSegmenter::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "YOLOV8_SEG_COCO80", error);
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

  bool run(const std::string &image_path, InstanceSegmentationResult *result,
           std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.load(image_path, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool runFrame(const Frame &frame, InstanceSegmentationResult *result,
                std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.wrap(frame, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool infer(TDLImage image, InstanceSegmentationResult *result,
             std::string *error) {
    if (!session_.initialized()) {
      private_tdl_sdk::setError(error, "instance segmenter is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(
          error, "instance segmentation result pointer is null");
      return false;
    }

    TDLInstanceSeg meta;
    std::memset(&meta, 0, sizeof(meta));
    const int ret = TDL_InstanceSegmentation(session_.handle(),
                                             session_.modelId(), image, &meta);
    if (ret != 0) {
      private_tdl_sdk::setError(
          error, "TDL_InstanceSegmentation failed, ret=" + std::to_string(ret));
      return false;
    }

    result->clear();
    result->width = static_cast<int>(meta.width);
    result->height = static_cast<int>(meta.height);
    result->mask_width = static_cast<int>(meta.mask_width);
    result->mask_height = static_cast<int>(meta.mask_height);
    result->instances.reserve(meta.size);
    const int mask_pixels = result->mask_width * result->mask_height;
    for (std::uint32_t i = 0; i < meta.size; ++i) {
      InstanceSegment instance;
      if (meta.info[i].obj_info) {
        instance.box.x1 = meta.info[i].obj_info->box.x1;
        instance.box.y1 = meta.info[i].obj_info->box.y1;
        instance.box.x2 = meta.info[i].obj_info->box.x2;
        instance.box.y2 = meta.info[i].obj_info->box.y2;
        instance.box.score = meta.info[i].obj_info->score;
        instance.box.class_id = meta.info[i].obj_info->class_id;
      }
      if (meta.info[i].mask && mask_pixels > 0) {
        instance.mask.assign(meta.info[i].mask, meta.info[i].mask + mask_pixels);
      }
      instance.outline.reserve(meta.info[i].mask_point_size);
      for (std::uint32_t j = 0; j < meta.info[i].mask_point_size; ++j) {
        Point point;
        point.x = meta.info[i].mask_point[2 * j];
        point.y = meta.info[i].mask_point[2 * j + 1];
        point.score = instance.box.score;
        instance.outline.push_back(point);
      }
      result->instances.push_back(instance);
    }
    TDL_ReleaseInstanceSegMeta(&meta);
    return true;
  }

  void reset() { session_.close(); }
  bool initialized() const { return session_.initialized(); }

 private:
  private_tdl_sdk::Session session_;
};

InstanceSegmenter::InstanceSegmenter() = default;

InstanceSegmenter::InstanceSegmenter(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

InstanceSegmenter::~InstanceSegmenter() {
  reset();
  delete impl_;
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
  delete impl_;
  requested_model_type_ = std::move(other.requested_model_type_);
  config_ = std::move(other.config_);
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool InstanceSegmenter::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
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
    private_tdl_sdk::setError(error, "instance segmenter is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool InstanceSegmenter::segment(const std::string &image_path,
                                InstanceSegmentationResult *result,
                                std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "instance segmenter is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool InstanceSegmenter::initialized() const {
  return impl_ && impl_->initialized();
}

std::string InstanceSegmenter::modelType() const {
  return requested_model_type_;
}

void InstanceSegmenter::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
