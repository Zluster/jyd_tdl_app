#include "tdl_app/lane_detector.hpp"

#include <cstring>
#include <utility>

#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_utils.h"
#include "algorithm/private/tdl_sdk_utils.hpp"

namespace tdl_app {

class LaneDetector::Impl {
 public:
  bool load(const Config &config, const std::string &requested_model_type,
            std::string *resolved_model_type, std::string *error) {
    const std::string model_type = private_tdl_sdk::resolveModelToken(
        config, requested_model_type, "LSTR_DET_LANE", error);
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

  bool run(const std::string &image_path, LaneDetectionResult *result,
           std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.load(image_path, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool runFrame(const Frame &frame, LaneDetectionResult *result,
                std::string *error) {
    private_tdl_sdk::ImageGuard image;
    if (!image.wrap(frame, error)) {
      return false;
    }
    return infer(image.get(), result, error);
  }

  bool infer(TDLImage image, LaneDetectionResult *result,
             std::string *error) {
    if (!session_.initialized()) {
      private_tdl_sdk::setError(error, "lane detector is not initialized");
      return false;
    }
    if (!result) {
      private_tdl_sdk::setError(error, "lane result pointer is null");
      return false;
    }

    TDLLane meta;
    std::memset(&meta, 0, sizeof(meta));
    const int ret =
        TDL_LaneDetection(session_.handle(), session_.modelId(), image, &meta);
    if (ret != 0) {
      private_tdl_sdk::setError(
          error, "TDL_LaneDetection failed, ret=" + std::to_string(ret));
      return false;
    }

    result->clear();
    result->width = static_cast<int>(meta.width);
    result->height = static_cast<int>(meta.height);
    result->lane_state = meta.lane_state;
    result->lanes.reserve(meta.size);
    for (std::uint32_t i = 0; i < meta.size; ++i) {
      LaneSegment lane;
      lane.start.x = meta.lane[i].x[0];
      lane.start.y = meta.lane[i].y[0];
      lane.end.x = meta.lane[i].x[1];
      lane.end.y = meta.lane[i].y[1];
      lane.score = meta.lane[i].score;
      result->lanes.push_back(lane);
    }
    TDL_ReleaseLaneMeta(&meta);
    return true;
  }

  void reset() { session_.close(); }
  bool initialized() const { return session_.initialized(); }

 private:
  private_tdl_sdk::Session session_;
};

LaneDetector::LaneDetector() = default;

LaneDetector::LaneDetector(std::string model_type)
    : requested_model_type_(std::move(model_type)) {}

LaneDetector::~LaneDetector() {
  reset();
  delete impl_;
}

LaneDetector::LaneDetector(LaneDetector &&other) noexcept
    : requested_model_type_(std::move(other.requested_model_type_)),
      config_(std::move(other.config_)),
      impl_(other.impl_) {
  other.impl_ = nullptr;
}

LaneDetector &LaneDetector::operator=(LaneDetector &&other) noexcept {
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

bool LaneDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, requested_model_type_, &requested_model_type_,
                     error);
}

bool LaneDetector::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool LaneDetector::load(const std::string &model_spec,
                        const std::string &firmware,
                        std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool LaneDetector::load(const std::string &model_spec,
                        const std::string &firmware,
                        const std::string &model_dir,
                        std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool LaneDetector::run(const std::string &image_path, LaneDetectionResult *result,
                       std::string *error) {
  return detect(image_path, result, error);
}

bool LaneDetector::runFrame(const Frame &frame, LaneDetectionResult *result,
                            std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "lane detector is not initialized");
    return false;
  }
  return impl_->runFrame(frame, result, error);
}

bool LaneDetector::detect(const std::string &image_path,
                          LaneDetectionResult *result,
                          std::string *error) {
  if (!impl_) {
    private_tdl_sdk::setError(error, "lane detector is not initialized");
    return false;
  }
  return impl_->run(image_path, result, error);
}

bool LaneDetector::initialized() const {
  return impl_ && impl_->initialized();
}

std::string LaneDetector::modelType() const { return requested_model_type_; }

void LaneDetector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
