#include "tdl_app/vpss_group.hpp"

#include <cstring>

#include "cvi_comm_video.h"
#include "cvi_comm_vpss.h"
#include "cvi_vpss.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

VpssGroup::VpssGroup() = default;

VpssGroup::VpssGroup(const Config &config) : config_(config) {}

VpssGroup::~VpssGroup() { close(); }

bool VpssGroup::open(std::string *error) {
  if (started_) {
    return true;
  }

  VPSS_GRP_ATTR_S grp_attr;
  std::memset(&grp_attr, 0, sizeof(grp_attr));
  grp_attr.stFrameRate.s32SrcFrameRate = config_.group.frame_rate.src_fps;
  grp_attr.stFrameRate.s32DstFrameRate = config_.group.frame_rate.dst_fps;
  grp_attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config_.group.pixel_format);
  grp_attr.u32MaxW = static_cast<CVI_U32>(config_.group.max_size.width);
  grp_attr.u32MaxH = static_cast<CVI_U32>(config_.group.max_size.height);
  grp_attr.u8VpssDev = static_cast<CVI_U8>(config_.group.device);

  int ret = CVI_VPSS_CreateGrp(config_.group.group, &grp_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_CreateGrp failed, ret=" + std::to_string(ret));
    return false;
  }
  created_ = true;

  VPSS_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.u32Width = static_cast<CVI_U32>(config_.channel.output_size.width);
  chn_attr.u32Height = static_cast<CVI_U32>(config_.channel.output_size.height);
  chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  chn_attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config_.channel.pixel_format);
  chn_attr.stFrameRate.s32SrcFrameRate = config_.channel.frame_rate.src_fps;
  chn_attr.stFrameRate.s32DstFrameRate = config_.channel.frame_rate.dst_fps;
  chn_attr.u32Depth = static_cast<CVI_U32>(config_.channel.depth);
  chn_attr.bMirror = config_.channel.mirror ? CVI_TRUE : CVI_FALSE;
  chn_attr.bFlip = config_.channel.flip ? CVI_TRUE : CVI_FALSE;
  chn_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
  chn_attr.stNormalize.bEnable = config_.channel.normalize ? CVI_TRUE : CVI_FALSE;

  ret = CVI_VPSS_SetChnAttr(config_.group.group, config_.channel.channel, &chn_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_SetChnAttr failed, ret=" + std::to_string(ret));
    close();
    return false;
  }

  ret = CVI_VPSS_EnableChn(config_.group.group, config_.channel.channel);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_EnableChn failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  channel_enabled_ = true;

  ret = CVI_VPSS_StartGrp(config_.group.group);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_StartGrp failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  started_ = true;
  return true;
}

void VpssGroup::close() {
  if (started_) {
    CVI_VPSS_StopGrp(config_.group.group);
    started_ = false;
  }
  if (channel_enabled_) {
    CVI_VPSS_DisableChn(config_.group.group, config_.channel.channel);
    channel_enabled_ = false;
  }
  if (created_) {
    CVI_VPSS_DestroyGrp(config_.group.group);
    created_ = false;
  }
}

bool VpssGroup::isOpen() const { return started_; }

}  // namespace tdl_app
