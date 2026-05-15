#include "tdl_app/osd_region.hpp"

#include <cstdio>

#include "cvi_comm_region.h"
#include "cvi_region.h"
#include "media/private/media_common.hpp"

namespace tdl_app {
OsdRegion::OsdRegion() = default;

OsdRegion::OsdRegion(const Config &config) : config_(config) {}

OsdRegion::~OsdRegion() {
  detach();
  destroy();
}

bool OsdRegion::create(std::string *error) {
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

  std::fprintf(stderr,
               "osd: create begin handle=%d size=%dx%d fmt=%d canvas=%d\n",
               config_.handle, config_.size.width, config_.size.height,
               config_.pixel_format, config_.canvas_count);
  const int ret = CVI_RGN_Create(config_.handle, &attr);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_Create failed, ret=" + std::to_string(ret));
    return false;
  }

  std::fprintf(stderr, "osd: create done handle=%d\n", config_.handle);
  created_ = true;
  return true;
}

bool OsdRegion::attach(const MediaChannel &channel, int x, int y, int layer,
                       std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "osd region is not created");
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
  attached_x_ = x;
  attached_y_ = y;
  attached_layer_ = layer;
  attached_ = true;
  return true;
}

bool OsdRegion::setBitmap(const OverlayBitmap &bitmap, std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "osd region is not created");
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

bool OsdRegion::setVisible(bool visible, std::string *error) {
  if (!attached_) {
    private_media::detail::setError(error, "osd region is not attached");
    return false;
  }

  MMF_CHN_S mmf_chn = private_media::detail::toMmfChannel(attached_channel_);
  RGN_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  const int ret = CVI_RGN_GetDisplayAttr(config_.handle, &mmf_chn, &chn_attr);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_GetDisplayAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  chn_attr.bShow = visible ? CVI_TRUE : CVI_FALSE;
  const int set_ret =
      CVI_RGN_SetDisplayAttr(config_.handle, &mmf_chn, &chn_attr);
  if (set_ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_SetDisplayAttr failed, ret=" + std::to_string(set_ret));
    return false;
  }
  return true;
}

bool OsdRegion::moveTo(int x, int y, std::string *error) {
  if (!attached_) {
    private_media::detail::setError(error, "osd region is not attached");
    return false;
  }

  MMF_CHN_S mmf_chn = private_media::detail::toMmfChannel(attached_channel_);
  RGN_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  const int ret = CVI_RGN_GetDisplayAttr(config_.handle, &mmf_chn, &chn_attr);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_GetDisplayAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  chn_attr.unChnAttr.stOverlayChn.stPoint.s32X = x;
  chn_attr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
  const int set_ret =
      CVI_RGN_SetDisplayAttr(config_.handle, &mmf_chn, &chn_attr);
  if (set_ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_SetDisplayAttr failed, ret=" + std::to_string(set_ret));
    return false;
  }

  attached_x_ = x;
  attached_y_ = y;
  return true;
}

bool OsdRegion::getCanvas(OsdCanvas *canvas, std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "osd region is not created");
    return false;
  }
  if (!canvas) {
    private_media::detail::setError(error, "osd canvas pointer is null");
    return false;
  }

  RGN_CANVAS_INFO_S info;
  std::memset(&info, 0, sizeof(info));
  const int ret = CVI_RGN_GetCanvasInfo(config_.handle, &info);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_GetCanvasInfo failed, ret=" + std::to_string(ret));
    return false;
  }

  canvas->data = info.pu8VirtAddr;
  canvas->width = static_cast<int>(info.stSize.u32Width);
  canvas->height = static_cast<int>(info.stSize.u32Height);
  canvas->stride = static_cast<int>(info.u32Stride);
  canvas->pixel_format = static_cast<int>(info.enPixelFormat);
  return true;
}

bool OsdRegion::updateCanvas(std::string *error) {
  if (!created_) {
    private_media::detail::setError(error, "osd region is not created");
    return false;
  }

  const int ret = CVI_RGN_UpdateCanvas(config_.handle);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_RGN_UpdateCanvas failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

void OsdRegion::detach() {
  if (!attached_) {
    return;
  }
  MMF_CHN_S mmf_chn = private_media::detail::toMmfChannel(attached_channel_);
  CVI_RGN_DetachFromChn(config_.handle, &mmf_chn);
  attached_ = false;
}

void OsdRegion::destroy() {
  if (!created_) {
    return;
  }
  std::fprintf(stderr, "osd: destroy begin handle=%d\n", config_.handle);
  CVI_RGN_Destroy(config_.handle);
  std::fprintf(stderr, "osd: destroy done handle=%d\n", config_.handle);
  created_ = false;
}

bool OsdRegion::isCreated() const { return created_; }

bool OsdRegion::isAttached() const { return attached_; }

}  // namespace tdl_app
