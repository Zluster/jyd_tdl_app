#include "tdl_app/frame_reader.hpp"

#include <sstream>
#include <iomanip>
#include <cstring>

#include "cvi_errno.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

bool isVi(const MediaChannel &channel) {
  return channel.module == MediaModule::Vi;
}

bool isVpss(const MediaChannel &channel) {
  return channel.module == MediaModule::Vpss;
}

const char *moduleName(const MediaChannel &channel) {
  if (isVi(channel)) {
    return "VI";
  }
  if (isVpss(channel)) {
    return "VPSS";
  }
  return "UNKNOWN";
}

std::string hexCode(int ret) {
  std::ostringstream oss;
  oss << "0x" << std::uppercase << std::hex
      << static_cast<unsigned int>(ret);
  return oss.str();
}

std::string describeReaderConfig(const FrameReader::Config &config) {
  std::ostringstream oss;
  oss << "module=" << moduleName(config.channel)
      << " device=" << config.channel.device
      << " channel=" << config.channel.channel
      << " timeout_ms=" << config.timeout_ms;
  return oss.str();
}

class FrameReaderImpl {
 public:
  explicit FrameReaderImpl(const FrameReader::Config &config) : config_(config) {}

  ~FrameReaderImpl() { close(); }

  bool open(std::string *error) {
    if (!isVi(config_.channel) && !isVpss(config_.channel)) {
      setError(error, "frame reader only supports VI or VPSS channels");
      return false;
    }
    opened_ = true;
    return true;
  }

  bool read(Frame *frame, std::string *error) {
    if (!opened_) {
      setError(error, "frame reader is not opened");
      return false;
    }
    if (!frame) {
      setError(error, "frame pointer is null");
      return false;
    }

    releaseCurrentFrame();

    int ret = CVI_FAILURE;
    if (isVi(config_.channel)) {
      ret = CVI_VI_GetChnFrame(config_.channel.device, config_.channel.channel,
                               &frame_info_, config_.timeout_ms);
    } else {
      ret = CVI_VPSS_GetChnFrame(config_.channel.device, config_.channel.channel,
                                 &frame_info_, config_.timeout_ms);
    }
    if (ret != CVI_SUCCESS) {
      setError(error, "failed to get frame: " + describeReaderConfig(config_) +
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

  void close() {
    releaseCurrentFrame();
    opened_ = false;
  }

  bool isOpen() const { return opened_; }

 private:
  void releaseCurrentFrame() {
    if (!frame_valid_) {
      return;
    }
    if (isVi(config_.channel)) {
      CVI_VI_ReleaseChnFrame(config_.channel.device, config_.channel.channel, &frame_info_);
    } else {
      CVI_VPSS_ReleaseChnFrame(config_.channel.device, config_.channel.channel, &frame_info_);
    }
    std::memset(&frame_info_, 0, sizeof(frame_info_));
    frame_valid_ = false;
  }

  FrameReader::Config config_;
  VIDEO_FRAME_INFO_S frame_info_ {};
  bool opened_ = false;
  bool frame_valid_ = false;
};

}  // namespace

class FrameReader::Impl : public FrameReaderImpl {
 public:
  explicit Impl(const FrameReader::Config &config) : FrameReaderImpl(config) {}
};

FrameReader::FrameReader() : FrameReader(Config{}) {}

FrameReader::FrameReader(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

FrameReader::~FrameReader() {
  delete impl_;
  impl_ = nullptr;
}

bool FrameReader::open(std::string *error) { return impl_->open(error); }

bool FrameReader::read(Frame *frame, std::string *error) {
  return impl_->read(frame, error);
}

void FrameReader::close() { impl_->close(); }

bool FrameReader::isOpen() const { return impl_->isOpen(); }

}  // namespace tdl_app
