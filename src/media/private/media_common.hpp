#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_buffer.h"
#include "cvi_comm_vb.h"
#include "cvi_comm_sys.h"
#include "cvi_comm_vi.h"
#include "cvi_comm_video.h"
#include "cvi_comm_vpss.h"
#include "sensor_cfg.h"
#include "tdl_app/sensor_media.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_app {
namespace private_media {
namespace detail {

constexpr int kMaxPools = 16;

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

inline MOD_ID_E toModId(MediaModule module) {
  switch (module) {
    case MediaModule::Vi:
      return CVI_ID_VI;
    case MediaModule::Vpss:
      return CVI_ID_VPSS;
    case MediaModule::Venc:
      return CVI_ID_VENC;
    case MediaModule::Vo:
      return CVI_ID_VO;
    case MediaModule::Rgn:
      return CVI_ID_RGN;
    case MediaModule::Vdec:
      return CVI_ID_VDEC;
    case MediaModule::Unknown:
    default:
      return CVI_ID_BUTT;
  }
}

inline MMF_CHN_S toMmfChannel(const MediaChannel &channel) {
  MMF_CHN_S out;
  std::memset(&out, 0, sizeof(out));
  out.enModId = toModId(channel.module);
  out.s32DevId = channel.device;
  out.s32ChnId = channel.channel;
  return out;
}

inline bool addPool(VB_CONFIG_S *vb_config, CVI_U32 blk_size, CVI_U32 blk_cnt) {
  for (CVI_U32 i = 0; i < vb_config->u32MaxPoolCnt; ++i) {
    if (vb_config->astCommPool[i].u32BlkSize == blk_size) {
      vb_config->astCommPool[i].u32BlkCnt += blk_cnt;
      return true;
    }
  }
  if (vb_config->u32MaxPoolCnt >= kMaxPools) {
    return false;
  }
  VB_POOL_CONFIG_S *pool = &vb_config->astCommPool[vb_config->u32MaxPoolCnt++];
  std::memset(pool, 0, sizeof(*pool));
  pool->u32BlkSize = blk_size;
  pool->u32BlkCnt = blk_cnt;
  pool->enRemapMode = VB_REMAP_MODE_CACHED;
  return true;
}

inline VI_DEV_ATTR_S makeViDevAttr(const SNS_CFG_S &sns_cfg, int index) {
  VI_DEV_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  for (int i = 0; i < VI_MAX_ADCHN_NUM; ++i) {
    attr.as32AdChnId[i] = -1;
  }
  attr.snrFps = static_cast<CVI_U32>(sns_cfg.f32FrameRate[index]);
  attr.stSize.u32Width = sns_cfg.u32ImageWigth[index];
  attr.stSize.u32Height = sns_cfg.u32ImageHeight[index];
  attr.enIntfMode = static_cast<VI_INTF_MODE_E>(sns_cfg.enInterFaceMode[index]);
  attr.enInputDataType = static_cast<VI_DATA_TYPE_E>(sns_cfg.enFormatMode[index]);
  attr.enDataSeq = static_cast<VI_YUV_DATA_SEQ_E>(sns_cfg.enYuvFormat[index]);
  attr.stWDRAttr.enWDRMode = sns_cfg.enWDRMode[index];
  attr.stWDRAttr.u32CacheLine = sns_cfg.u32ImageHeight[index];
  attr.enWorkMode = static_cast<VI_WORK_MODE_E>(sns_cfg.enChnMode[index]);
  attr.enScanMode = VI_SCAN_PROGRESSIVE;
  attr.enBayerFormat = sns_cfg.enBayerFormat[index];
  return attr;
}

inline VI_PIPE_ATTR_S makeViPipeAttr(const SNS_CFG_S &sns_cfg, int index) {
  VI_PIPE_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enPipeBypassMode = VI_PIPE_BYPASS_NONE;
  attr.bYuvSkip = CVI_FALSE;
  attr.u32MaxW = sns_cfg.u32ImageWigth[index];
  attr.u32MaxH = sns_cfg.u32ImageHeight[index];
  attr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
  attr.enCompressMode = sns_cfg.bBypassIsp[index]
                            ? COMPRESS_MODE_NONE
                            : COMPRESS_MODE_TILE;
  attr.enBitWidth = DATA_BITWIDTH_12;
  attr.bSharpenEn = CVI_FALSE;
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.bNrEn = CVI_TRUE;
  attr.bDiscardProPic = CVI_FALSE;
  attr.bYuvBypassPath = sns_cfg.bBypassIsp[index];
  return attr;
}

inline VI_CHN_ATTR_S makeViChnAttr(const SNS_CFG_S &sns_cfg, int index) {
  VI_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.stSize.u32Width = sns_cfg.u32ImageWigth[index];
  attr.stSize.u32Height = sns_cfg.u32ImageHeight[index];
  attr.enPixelFormat = sns_cfg.bBypassIsp[index]
                           ? PIXEL_FORMAT_YUYV
                           : PIXEL_FORMAT_NV12;
  attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
  attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  // Align with the working ipcamera VI path, which uses tiled compression on
  // the NV12 VI channel after ISP.
  attr.enCompressMode = sns_cfg.bBypassIsp[index]
                            ? COMPRESS_MODE_NONE
                            : COMPRESS_MODE_TILE;
  attr.bMirror = CVI_FALSE;
  attr.bFlip = CVI_FALSE;
  attr.u32Depth = 1;
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.u32BindVbPool = static_cast<CVI_U32>(-1);
  attr.bSingleVb = CVI_FALSE;
  return attr;
}

inline VPSS_GRP_ATTR_S makeVpssGrpAttr(const SNS_CFG_S &sns_cfg, int index,
                                       int pixel_format) {
  VPSS_GRP_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(pixel_format);
  attr.u32MaxW = sns_cfg.u32ImageWigth[index];
  attr.u32MaxH = sns_cfg.u32ImageHeight[index];
  attr.u8VpssDev = 1;
  return attr;
}

inline VPSS_CHN_ATTR_S makeVpssChnAttr(
    const SensorMedia::VpssOutputConfig &config) {
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.u32Width = static_cast<CVI_U32>(config.output_width);
  attr.u32Height = static_cast<CVI_U32>(config.output_height);
  attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config.output_pixel_format);
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.u32Depth = 1;
  attr.bMirror = CVI_FALSE;
  attr.bFlip = CVI_FALSE;
  attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
  attr.stNormalize.bEnable = CVI_FALSE;
  return attr;
}

inline ALG_LIB_S makeAeLib(int id) {
  ALG_LIB_S lib;
  std::memset(&lib, 0, sizeof(lib));
  lib.s32Id = id;
  std::snprintf(lib.acLibName, sizeof(lib.acLibName), "%s", CVI_AE_LIB_NAME);
  return lib;
}

inline ALG_LIB_S makeAwbLib(int id) {
  ALG_LIB_S lib;
  std::memset(&lib, 0, sizeof(lib));
  lib.s32Id = id;
  std::snprintf(lib.acLibName, sizeof(lib.acLibName), "%s", CVI_AWB_LIB_NAME);
  return lib;
}

}  // namespace detail
}  // namespace private_media
}  // namespace tdl_app
