#include "tdl_app/region_overlay.hpp"

#include "cvi_comm_region.h"
#include "cvi_region.h"
#include "media/private/media_common.hpp"

namespace tdl_app {
RegionOverlay::RegionOverlay() = default;

RegionOverlay::RegionOverlay(const OverlayRegionConfig &config) : config_(config) {}

RegionOverlay::~RegionOverlay() {
  detach();
  destroy();
}

bool RegionOverlay::create(std::string *error) {
  if (created_) {
    return true;
  }

  RGN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = OVERLAY_RGN;
  attr.unAttr.stOverlay.enPixelFormat =
      static_cast<PIXEL_FORMAT_E>(config_.pixel_format);
  attr.unAttr.stOverlay.stSize.u32Width =
      static_cast<CVI_U32>(config_.size.width);
  attr.unAttr.stOverlay.stSize.u32Height =
      static_cast<CVI_U32>(config_.size.height);
  attr.unAttr.stOverlay.u32BgColor = config_.bg_color;
  attr.unAttr.stOverlay.u32CanvasNum =
      static_cast<CVI_U32>(config_.canvas_count);
  attr.unAttr.stOverlay.stCompressInfo.enOSDCompressMode =
      OSD_COMPRESS_MODE_NONE;

  const int ret = CVI_RGN_Create(config_.handle, &attr);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_Create failed, ret=" + std::to_string(ret));
    return false;
  }
  created_ = true;
  return true;
}

bool RegionOverlay::attach(const MediaChannel &channel, int x, int y, int layer,
                           std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "overlay is not created");
    return false;
  }
  if (attached_) {
    return true;
  }

  MMF_CHN_S mmf_chn = private_media::detail::toMmfChannel(channel);
  RGN_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.bShow = CVI_TRUE;
  chn_attr.enType = OVERLAY_RGN;
  chn_attr.unChnAttr.stOverlayChn.stPoint.s32X = x;
  chn_attr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
  chn_attr.unChnAttr.stOverlayChn.u32Layer = static_cast<CVI_U32>(layer);

  const int ret = CVI_RGN_AttachToChn(config_.handle, &mmf_chn, &chn_attr);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_AttachToChn failed, ret=" + std::to_string(ret));
    return false;
  }

  attached_channel_ = channel;
  attached_ = true;
  return true;
}

bool RegionOverlay::setBitmap(const OverlayBitmap &bitmap, std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "overlay is not created");
    return false;
  }

  BITMAP_S bmp;
  std::memset(&bmp, 0, sizeof(bmp));
  bmp.pData = const_cast<void *>(bitmap.data);
  bmp.u32Width = static_cast<CVI_U32>(bitmap.size.width);
  bmp.u32Height = static_cast<CVI_U32>(bitmap.size.height);
  bmp.enPixelFormat = static_cast<PIXEL_FORMAT_E>(bitmap.pixel_format);

  const int ret = CVI_RGN_SetBitMap(config_.handle, &bmp);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_SetBitMap failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

void RegionOverlay::detach() {
  if (!attached_) {
    return;
  }
  MMF_CHN_S mmf_chn = private_media::detail::toMmfChannel(attached_channel_);
  CVI_RGN_DetachFromChn(config_.handle, &mmf_chn);
  attached_ = false;
}

void RegionOverlay::destroy() {
  if (!created_) {
    return;
  }
  CVI_RGN_Destroy(config_.handle);
  created_ = false;
}

bool RegionOverlay::isCreated() const { return created_; }

}  // namespace tdl_app
