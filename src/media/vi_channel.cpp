#include "tdl_app/vi_channel.hpp"

#include <cstring>

#include "cvi_common.h"
#include "cvi_vi.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

ViChannel::ViChannel() = default;

ViChannel::ViChannel(const Config &config) : config_(config) {}

ViChannel::~ViChannel() { close(); }

bool ViChannel::open(std::string *error) {
  if (opened_) {
    return true;
  }

  VI_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config_.pixel_format);
  attr.stSize.u32Width = static_cast<CVI_U32>(config_.width);
  attr.stSize.u32Height = static_cast<CVI_U32>(config_.height);
  attr.u32Depth = static_cast<CVI_U32>(config_.depth);
  attr.bMirror = config_.mirror ? CVI_TRUE : CVI_FALSE;
  attr.bFlip = config_.flip ? CVI_TRUE : CVI_FALSE;
  attr.bSingleVb = config_.single_vb ? CVI_TRUE : CVI_FALSE;

  int ret = CVI_VI_SetChnAttr(config_.pipe, config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VI_SetChnAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_VI_EnableChn(config_.pipe, config_.channel);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VI_EnableChn failed, ret=" + std::to_string(ret));
    return false;
  }

  opened_ = true;
  return true;
}

void ViChannel::close() {
  if (!opened_) {
    return;
  }
  CVI_VI_DisableChn(config_.pipe, config_.channel);
  opened_ = false;
}

bool ViChannel::isOpen() const { return opened_; }

}  // namespace tdl_app
