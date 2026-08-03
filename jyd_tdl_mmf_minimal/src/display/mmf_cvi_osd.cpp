#include "mmf_cv184x_common.hpp"

namespace mmf_cvi {
OsdRegion::OsdRegion() = default;
OsdRegion::OsdRegion(const Config& config) : config_(config) {}
OsdRegion::~OsdRegion() {
  detach();
  destroy();
}
bool OsdRegion::create(std::string* error) {
  if (created_)
    return true;
  RGN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = OVERLAY_RGN;
  attr.unAttr.stOverlay.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config_.pixel_format);
  attr.unAttr.stOverlay.stSize.u32Width = config_.size.width;
  attr.unAttr.stOverlay.stSize.u32Height = config_.size.height;
  attr.unAttr.stOverlay.u32BgColor = config_.bg_color;
  attr.unAttr.stOverlay.u32CanvasNum = config_.canvas_count;
  attr.unAttr.stOverlay.stCompressInfo.enOSDCompressMode = OSD_COMPRESS_MODE_NONE;
  int ret = CVI_RGN_Create(config_.handle, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_Create failed, ret=" + std::to_string(ret));
    return false;
  }
  created_ = true;
  return true;
}
bool OsdRegion::attach(const MediaChannel& channel, int x, int y, int layer, std::string* error) {
  if (!created_) {
    setError(error, "osd not created");
    return false;
  }
  if (attached_)
    return true;
  MMF_CHN_S chn = toMmfChannel(channel);
  RGN_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.bShow = CVI_TRUE;
  attr.enType = OVERLAY_RGN;
  attr.unChnAttr.stOverlayChn.stPoint.s32X = x;
  attr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
  attr.unChnAttr.stOverlayChn.u32Layer = layer;
  int ret = CVI_RGN_AttachToChn(config_.handle, &chn, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_AttachToChn failed, ret=" + std::to_string(ret));
    return false;
  }
  attached_channel_ = channel;
  attached_ = true;
  return true;
}
bool OsdRegion::getCanvas(OsdCanvas* canvas, std::string* error) {
  if (!canvas)
    return false;
  RGN_CANVAS_INFO_S info;
  std::memset(&info, 0, sizeof(info));
  int ret = CVI_RGN_GetCanvasInfo(config_.handle, &info);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_GetCanvasInfo failed, ret=" + std::to_string(ret));
    return false;
  }
  canvas->data = info.pu8VirtAddr;
  canvas->width = info.stSize.u32Width;
  canvas->height = info.stSize.u32Height;
  canvas->stride = info.u32Stride;
  canvas->pixel_format = info.enPixelFormat;
  return true;
}
bool OsdRegion::updateCanvas(std::string* error) {
  int ret = CVI_RGN_UpdateCanvas(config_.handle);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_UpdateCanvas failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
bool OsdRegion::setVisible(bool visible, std::string* error) {
  if (!attached_)
    return true;
  MMF_CHN_S chn = toMmfChannel(attached_channel_);
  RGN_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  int ret = CVI_RGN_GetDisplayAttr(config_.handle, &chn, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_GetDisplayAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  attr.bShow = visible ? CVI_TRUE : CVI_FALSE;
  ret = CVI_RGN_SetDisplayAttr(config_.handle, &chn, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_RGN_SetDisplayAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
void OsdRegion::detach() {
  if (!attached_)
    return;
  MMF_CHN_S chn = toMmfChannel(attached_channel_);
  CVI_RGN_DetachFromChn(config_.handle, &chn);
  attached_ = false;
}
void OsdRegion::destroy() {
  if (!created_)
    return;
  CVI_RGN_Destroy(config_.handle);
  created_ = false;
}
}  // namespace mmf_cvi
