#include "tdl_app/vo_output.hpp"

#include <cstring>

#include "cvi_common.h"
#include "cvi_vo.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

VoOutput::VoOutput() = default;

VoOutput::VoOutput(const Config &config) : config_(config) {}

VoOutput::~VoOutput() { close(); }

bool VoOutput::open(std::string *error) {
  if (device_enabled_) {
    return true;
  }

  VO_PUB_ATTR_S pub_attr;
  std::memset(&pub_attr, 0, sizeof(pub_attr));
  pub_attr.enIntfType = static_cast<VO_INTF_TYPE_E>(config_.interface_type);
  pub_attr.enIntfSync = static_cast<VO_INTF_SYNC_E>(config_.interface_sync);

  int ret = CVI_VO_SetPubAttr(config_.device, &pub_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_VO_Enable(config_.device);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_Enable failed, ret=" + std::to_string(ret));
    return false;
  }
  device_enabled_ = true;

  VO_VIDEO_LAYER_ATTR_S layer_attr;
  std::memset(&layer_attr, 0, sizeof(layer_attr));
  layer_attr.stDispRect.s32X = 0;
  layer_attr.stDispRect.s32Y = 0;
  layer_attr.stDispRect.u32Width = static_cast<CVI_U32>(config_.width);
  layer_attr.stDispRect.u32Height = static_cast<CVI_U32>(config_.height);
  layer_attr.stImageSize.u32Width = static_cast<CVI_U32>(config_.width);
  layer_attr.stImageSize.u32Height = static_cast<CVI_U32>(config_.height);
  layer_attr.u32DispFrmRt = 25;

  ret = CVI_VO_SetVideoLayerAttr(config_.layer, &layer_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetVideoLayerAttr failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  ret = CVI_VO_EnableVideoLayer(config_.layer);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_EnableVideoLayer failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  layer_enabled_ = true;

  if (config_.display_buf_len > 0) {
    CVI_VO_SetDisplayBufLen(config_.layer,
                            static_cast<CVI_U32>(config_.display_buf_len));
  }

  VO_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.stRect.s32X = 0;
  chn_attr.stRect.s32Y = 0;
  chn_attr.stRect.u32Width = static_cast<CVI_U32>(config_.width);
  chn_attr.stRect.u32Height = static_cast<CVI_U32>(config_.height);
  chn_attr.u32Priority = 0;

  ret = CVI_VO_SetChnAttr(config_.layer, config_.channel, &chn_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetChnAttr failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  ret = CVI_VO_EnableChn(config_.layer, config_.channel);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_EnableChn failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  channel_enabled_ = true;
  return true;
}

void VoOutput::close() {
  if (channel_enabled_) {
    CVI_VO_DisableChn(config_.layer, config_.channel);
    channel_enabled_ = false;
  }
  if (layer_enabled_) {
    CVI_VO_DisableVideoLayer(config_.layer);
    layer_enabled_ = false;
  }
  if (device_enabled_) {
    CVI_VO_Disable(config_.device);
    device_enabled_ = false;
  }
}

bool VoOutput::isOpen() const { return device_enabled_; }

}  // namespace tdl_app
