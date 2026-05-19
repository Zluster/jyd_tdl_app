#include "tdl_app/keypoint_detector.hpp"

#include <cstring>
#include <utility>

#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_utils.h"
#include "algorithm/private/tdl_sdk_utils.hpp"

namespace tdl_app {

class KeypointDetector::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "KEYPOINT_HAND", error);
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

  bool run(const std::string &image_path, KeypointResult *result,
           std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.load(image_path, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool runFrame(const Frame &frame, KeypointResult *result,
                std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.wrap(frame, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool infer(TDLImage image, KeypointResult *result, std::string *error) {
    if (!session_.initialized()) {
      private_tdl_sdk::setError(error, "keypoint detector is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(error, "keypoint result pointer is null");
      return false;
    }

    TDLKeypoint meta;
    std::memset(&meta, 0, sizeof(meta));
    const int ret =
        TDL_Keypoint(session_.handle(), session_.modelId(), image, &meta);
    if (ret != 0) {
      private_tdl_sdk::setError(error,
                                "TDL_Keypoint failed, ret=" + std::to_string(ret));
      return false;
    }

    result->clear();
    result->width = static_cast<int>(meta.width);
    result->height = static_cast<int>(meta.height);
    result->points.reserve(meta.size);
    for (std::uint32_t i = 0; i < meta.size; ++i) {
      Point point;
      point.x = meta.info[i].x * meta.width;
      point.y = meta.info[i].y * meta.height;
      point.score = meta.info[i].score;
      result->points.push_back(point);
    }
    TDL_ReleaseKeypointMeta(&meta);
    return true;
  }

  void reset() { session_.close(); }
  bool initialized() const { return session_.initialized(); }

 private:
  private_tdl_sdk::Session session_;
};

KeypointDetector::KeypointDetector() = default;

KeypointDetector::KeypointDetector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

KeypointDetector::~KeypointDetector() {
  reset();
  delete impl_;
}

KeypointDetector::KeypointDetector(KeypointDetector &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

KeypointDetector &KeypointDetector::operator=(KeypointDetector &&other) noexcept {
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

bool KeypointDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
}

bool KeypointDetector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool KeypointDetector::load(const std::string &model_spec,
                            const std::string &firmware,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool KeypointDetector::load(const std::string &model_spec,
                            const std::string &firmware,
                            const std::string &model_dir,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool KeypointDetector::run(const std::string &image_path, KeypointResult *result,
                           std::string *error) {
  return estimate(image_path, result, error);
}

bool KeypointDetector::runFrame(const Frame &frame, KeypointResult *result,
                                std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool KeypointDetector::estimate(const std::string &image_path,
                                KeypointResult *result,
                                std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "keypoint detector is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool KeypointDetector::initialized() const {
  return impl_ && impl_->initialized();
}

std::string KeypointDetector::modelType() const { return requested_model_type_; }

void KeypointDetector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
