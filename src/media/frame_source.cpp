#include "tdl_app/frame_source.hpp"

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "cvi_type.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "cvi_errno.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

const char *backendName(CameraSource::Backend backend) {
  return backend == CameraSource::Backend::Vi ? "VI" : "VPSS";
}

std::string hexCode(int ret) {
  std::ostringstream oss;
  oss << "0x" << std::uppercase << std::hex
      << static_cast<unsigned int>(ret);
  return oss.str();
}

std::string describeCameraConfig(const CameraSource::Config &config) {
  std::ostringstream oss;
  oss << "backend=" << backendName(config.backend)
      << " pipe=" << config.pipe
      << " group=" << config.group
      << " channel=" << config.channel
      << " width=" << config.width
      << " height=" << config.height
      << " pixel_format=" << config.pixel_format
      << " timeout_ms=" << config.timeout_ms;
  return oss.str();
}

}  // namespace

ImageFileSource::ImageFileSource(std::string path) : path_(std::move(path)) {}

bool ImageFileSource::open(std::string *error) {
  (void)error;
  consumed_ = false;
  return true;
}

bool ImageFileSource::read(Frame *frame, std::string *error) {
  if (!frame) {
    setError(error, "frame pointer is null");
    return false;
  }
  if (consumed_) {
    setError(error, "image file source has no more frames");
    return false;
  }
  frame->image_path = path_;
  frame->native = nullptr;
  frame->width = 0;
  frame->height = 0;
  frame->format = 0;
  consumed_ = true;
  return true;
}

void ImageFileSource::close() { consumed_ = true; }

class CameraSource::Impl {
 public:
  explicit Impl(Config config) : config_(config) {}

  ~Impl() { close(); }

  bool open(std::string *error) {
    close();
    return true;
  }

  bool read(Frame *frame, std::string *error) {
    if (!frame) {
      setError(error, "frame pointer is null");
      return false;
    }

    releaseCurrentFrame();

    int ret = CVI_FAILURE;
    if (config_.backend == CameraSource::Backend::Vi) {
      ret = CVI_VI_GetChnFrame(config_.pipe, config_.channel, &frame_info_,
                               config_.timeout_ms);
    } else {
      ret = CVI_VPSS_GetChnFrame(config_.group, config_.channel, &frame_info_,
                                 config_.timeout_ms);
    }
    if (ret != CVI_SUCCESS) {
      setError(error, "failed to get camera frame: " +
                          describeCameraConfig(config_) +
                          " ret=" + std::to_string(ret) +
                          " (" + hexCode(ret) + ")");
      return false;
    }

    frame_valid_ = true;
    frame->image_path.clear();
    frame->native = &frame_info_;
    frame->width = static_cast<int>(frame_info_.stVFrame.u32Width);
    frame->height = static_cast<int>(frame_info_.stVFrame.u32Height);
    frame->format = static_cast<int>(frame_info_.stVFrame.enPixelFormat);
    frame->sequence = frame_info_.stVFrame.u32TimeRef;
    frame->timestamp_us = frame_info_.stVFrame.u64PTS;
    return true;
  }

  void close() { releaseCurrentFrame(); }

 private:
  void releaseCurrentFrame() {
    if (!frame_valid_) {
      return;
    }
    if (config_.backend == CameraSource::Backend::Vi) {
      CVI_VI_ReleaseChnFrame(config_.pipe, config_.channel, &frame_info_);
    } else {
      CVI_VPSS_ReleaseChnFrame(config_.group, config_.channel, &frame_info_);
    }
    std::memset(&frame_info_, 0, sizeof(frame_info_));
    frame_valid_ = false;
  }

  Config config_;
  VIDEO_FRAME_INFO_S frame_info_ {};
  bool frame_valid_ = false;
};

CameraSource::CameraSource() : CameraSource(Config{}) {}

CameraSource::CameraSource(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

CameraSource::~CameraSource() = default;

bool CameraSource::open(std::string *error) { return impl_->open(error); }

bool CameraSource::read(Frame *frame, std::string *error) {
  return impl_->read(frame, error);
}

void CameraSource::close() { impl_->close(); }

}  // namespace tdl_app
