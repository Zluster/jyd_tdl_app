#include "media/private/sensor_media_impl.hpp"

#include <cstring>

#include "cvi_comm_sys.h"
#include "cvi_comm_vb.h"
#include "cvi_comm_vpss.h"
#include "cvi_sys.h"
#include "cvi_vpss.h"
#include "media/private/media_common.hpp"

namespace tdl_app {
namespace private_media {

using detail::makeVpssChnAttr;
using detail::makeVpssGrpAttr;
using detail::setError;

bool SensorMediaImpl::startVpss(std::string *error) {
  if (!config_.enable_vpss) {
    return true;
  }
  const int pipe = 0;
  for (auto &output : vpss_outputs_) {
    int ret = CVI_SUCCESS;

    VPSS_GRP_ATTR_S grp_attr =
        makeVpssGrpAttr(sensor_cfg_.sns_cfg, pipe, output.config.output_pixel_format);
    ret = CVI_VPSS_CreateGrp(output.config.group, &grp_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_CreateGrp failed, group=" +
                          std::to_string(output.config.group) +
                          ", ret=" + std::to_string(ret));
      return false;
    }
    output.created = true;

    VPSS_CHN_ATTR_S chn_attr = makeVpssChnAttr(output.config);
    ret = CVI_VPSS_SetChnAttr(output.config.group, output.config.channel, &chn_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_SetChnAttr failed, group=" +
                          std::to_string(output.config.group) +
                          ", channel=" + std::to_string(output.config.channel) +
                          ", ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_VPSS_EnableChn(output.config.group, output.config.channel);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_EnableChn failed, group=" +
                          std::to_string(output.config.group) +
                          ", channel=" + std::to_string(output.config.channel) +
                          ", ret=" + std::to_string(ret));
      return false;
    }
    output.channel_enabled = true;

    ret = CVI_VPSS_StartGrp(output.config.group);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_StartGrp failed, group=" +
                          std::to_string(output.config.group) +
                          ", ret=" + std::to_string(ret));
      return false;
    }
    output.started = true;

    if (output.config.bind_from_vi) {
      MMF_CHN_S src;
      MMF_CHN_S dst;
      std::memset(&src, 0, sizeof(src));
      std::memset(&dst, 0, sizeof(dst));
      src.enModId = CVI_ID_VI;
      src.s32DevId = config_.vi_pipe;
      src.s32ChnId = config_.vi_channel;
      dst.enModId = CVI_ID_VPSS;
      dst.s32DevId = output.config.group;
      dst.s32ChnId = output.config.channel;
      ret = CVI_SYS_Bind(&src, &dst);
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_SYS_Bind(VI->VPSS) failed, group=" +
                            std::to_string(output.config.group) +
                            ", channel=" + std::to_string(output.config.channel) +
                            ", ret=" + std::to_string(ret));
        return false;
      }
      output.vi_bound = true;
    }
  }
  return true;
}

void SensorMediaImpl::stopVpss() {
  for (auto it = vpss_outputs_.rbegin(); it != vpss_outputs_.rend(); ++it) {
    auto &output = *it;
    if (output.vi_bound) {
      MMF_CHN_S src;
      MMF_CHN_S dst;
      std::memset(&src, 0, sizeof(src));
      std::memset(&dst, 0, sizeof(dst));
      src.enModId = CVI_ID_VI;
      src.s32DevId = config_.vi_pipe;
      src.s32ChnId = config_.vi_channel;
      dst.enModId = CVI_ID_VPSS;
      dst.s32DevId = output.config.group;
      dst.s32ChnId = output.config.channel;
      CVI_SYS_UnBind(&src, &dst);
      output.vi_bound = false;
    }
    if (output.started) {
      CVI_VPSS_StopGrp(output.config.group);
      output.started = false;
    }
    if (output.channel_enabled) {
      CVI_VPSS_DisableChn(output.config.group, output.config.channel);
      output.channel_enabled = false;
    }
    if (output.created) {
      CVI_VPSS_DestroyGrp(output.config.group);
      output.created = false;
    }
  }
}

}  // namespace private_media
}  // namespace tdl_app
