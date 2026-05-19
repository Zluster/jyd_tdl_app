#include "media/private/sensor_media_impl.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sched.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_common.h"
#include "cvi_comm_sys.h"
#include "cvi_isp.h"
#include "cvi_mipi.h"
#include "cvi_sensor.h"
#include "cvi_sys.h"
#include "cvi_vi.h"
#include "media/private/media_common.hpp"

namespace tdl_app {
namespace private_media {

using detail::makeAeLib;
using detail::makeAwbLib;
using detail::makeViChnAttr;
using detail::makeViDevAttr;
using detail::makeViPipeAttr;
using detail::setError;

namespace {

void *runIspThread(void *arg) {
  int pipe = *static_cast<int *>(arg);
  std::free(arg);

  char thread_name[20];
  std::snprintf(thread_name, sizeof(thread_name), "ISP%d_RUN", pipe);
  prctl(PR_SET_NAME, thread_name, 0, 0, 0);

  std::fprintf(stderr, "sensor_media: isp thread start pipe=%d\n", pipe);
  const int ret = CVI_ISP_Run(pipe);
  std::fprintf(stderr, "sensor_media: isp thread exit pipe=%d ret=%d\n", pipe, ret);
  return nullptr;
}

bool startIspThread(int pipe, pthread_t *thread, std::string *error) {
  int *arg = static_cast<int *>(std::malloc(sizeof(int)));
  if (!arg) {
    setError(error, "malloc failed for isp thread arg");
    return false;
  }
  *arg = pipe;

  pthread_attr_t attr;
  pthread_attr_init(&attr);

  struct sched_param param;
  std::memset(&param, 0, sizeof(param));
  param.sched_priority = 80;
  pthread_attr_setschedpolicy(&attr, SCHED_RR);
  pthread_attr_setschedparam(&attr, &param);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

  int ret = pthread_create(thread, &attr, runIspThread, arg);
  pthread_attr_destroy(&attr);
  if (ret != 0) {
    ret = pthread_create(thread, nullptr, runIspThread, arg);
  }
  if (ret != 0) {
    std::free(arg);
    setError(error, "pthread_create for isp thread failed, ret=" + std::to_string(ret));
    return false;
  }
  std::fprintf(stderr, "sensor_media: isp thread created pipe=%d\n", pipe);
  return true;
}

bool configureIspStats(int pipe, const ISP_PUB_ATTR_S &pub_attr, std::string *error) {
  ISP_STATISTICS_CFG_S stats_cfg;
  std::memset(&stats_cfg, 0, sizeof(stats_cfg));

  int ret = CVI_ISP_GetStatisticsConfig(pipe, &stats_cfg);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_GetStatisticsConfig failed, ret=" + std::to_string(ret));
    return false;
  }

  stats_cfg.stAECfg.stCrop[0].bEnable = 0;
  stats_cfg.stAECfg.stCrop[0].u16X = 0;
  stats_cfg.stAECfg.stCrop[0].u16Y = 0;
  stats_cfg.stAECfg.stCrop[0].u16W = pub_attr.stWndRect.u32Width;
  stats_cfg.stAECfg.stCrop[0].u16H = pub_attr.stWndRect.u32Height;
  std::memset(stats_cfg.stAECfg.au8Weight, 1,
              AE_WEIGHT_ZONE_ROW * AE_WEIGHT_ZONE_COLUMN * sizeof(CVI_U8));

  stats_cfg.stWBCfg.u16ZoneRow = AWB_ZONE_ORIG_ROW;
  stats_cfg.stWBCfg.u16ZoneCol = AWB_ZONE_ORIG_COLUMN;
  stats_cfg.stWBCfg.stCrop.u16X = 0;
  stats_cfg.stWBCfg.stCrop.u16Y = 0;
  stats_cfg.stWBCfg.stCrop.u16W = pub_attr.stWndRect.u32Width;
  stats_cfg.stWBCfg.stCrop.u16H = pub_attr.stWndRect.u32Height;
  stats_cfg.stWBCfg.u16BlackLevel = 0;
  stats_cfg.stWBCfg.u16WhiteLevel = 4095;

  stats_cfg.unKey.bit1FEAeGloStat = 1;
  stats_cfg.unKey.bit1FEAeLocStat = 1;
  stats_cfg.unKey.bit1AwbStat1 = 1;
  stats_cfg.unKey.bit1AwbStat2 = 1;
  stats_cfg.unKey.bit1FEAfStat = 1;

  ret = CVI_ISP_SetStatisticsConfig(pipe, &stats_cfg);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_SetStatisticsConfig failed, ret=" + std::to_string(ret));
    return false;
  }

  ISP_CTRL_PARAM_S ctrl_param;
  std::memset(&ctrl_param, 0, sizeof(ctrl_param));
  ctrl_param.u32AEStatIntvl = 1;
  ctrl_param.u32AWBStatIntvl = 6;
  ctrl_param.u32AFStatIntvl = 1;
  ret = CVI_ISP_SetCtrlParam(pipe, &ctrl_param);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_SetCtrlParam failed, ret=" + std::to_string(ret));
    return false;
  }

  return true;
}

}  // namespace

bool SensorMediaImpl::startViDev(std::string *error) {
  const int dev_num = sensor_cfg_.sns_ini_cfg.devNum;
  for (int i = 0; i < dev_num; ++i) {
    VI_DEV_ATTR_S dev_attr = makeViDevAttr(sensor_cfg_.sns_cfg, i);
    VI_DEV_ATTR_EX_S dev_attr_ex;
    std::memset(&dev_attr_ex, 0, sizeof(dev_attr_ex));
    VI_DEV_BIND_PIPE_S bind_attr;
    std::memset(&bind_attr, 0, sizeof(bind_attr));
    bind_attr.MipiDev = sensor_cfg_.sns_ini_cfg.MipiDev[i];
    bind_attr.u32Num = 1;
    bind_attr.PipeId[0] = i;

    int ret = CVI_VI_SetDevBindAttr(i, &bind_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_SetDevBindAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_VI_SetDevAttr(i, &dev_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_SetDevAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    if (sensor_cfg_.sns_ini_cfg.u8MuxDev[i]) {
      dev_attr_ex.bMuxDev = CVI_TRUE;
      dev_attr_ex.u8SnsrNum = static_cast<CVI_U8>(dev_num);
      dev_attr_ex.phyDev = sensor_cfg_.sns_ini_cfg.u8AttachDev[i];
      for (int j = 0; j < VI_MAX_DEV_SWITCH_DEPTH; ++j) {
        const int port = sensor_cfg_.sns_ini_cfg.s32SwitchPort[i][j];
        dev_attr_ex.stGpioCfg[j].bEnable = port != -1 ? CVI_TRUE : CVI_FALSE;
        dev_attr_ex.stGpioCfg[j].s32GpioPort = port;
        dev_attr_ex.stGpioCfg[j].s32GpioPin =
            sensor_cfg_.sns_ini_cfg.s32SwitchPin[i][j];
        dev_attr_ex.stGpioCfg[j].s32GpioPol =
            sensor_cfg_.sns_ini_cfg.s32SwitchPol[i][j];
      }
      ret = CVI_VI_SetDevAttrEx(i, &dev_attr_ex);
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VI_SetDevAttrEx failed, ret=" + std::to_string(ret));
        return false;
      }
    }
    ret = CVI_VI_EnableDev(i);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_EnableDev failed, ret=" + std::to_string(ret));
      return false;
    }
    vi_dev_enabled_[i] = true;
  }
  return true;
}

bool SensorMediaImpl::startMipi(std::string *error) {
  const int dev_num = sensor_cfg_.sns_ini_cfg.devNum;
  for (int i = 0; i < dev_num; ++i) {
    const int mipi_dev = sensor_cfg_.sns_ini_cfg.MipiDev[i];
    std::fprintf(stderr,
                 "sensor_media: sensor reset mipi_dev=%d rst_port=%d rst_pin=%d rst_pol=%d\n",
                 mipi_dev, sensor_cfg_.sns_ini_cfg.s32RstPort[i],
                 sensor_cfg_.sns_ini_cfg.s32RstPin[i], sensor_cfg_.sns_ini_cfg.s32RstPol[i]);
    int ret = CVI_MIPI_SetSensorReset(
        mipi_dev, sensor_cfg_.sns_ini_cfg.s32RstPort[i],
        sensor_cfg_.sns_ini_cfg.s32RstPin[i], sensor_cfg_.sns_ini_cfg.s32RstPol[i], 1);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_MIPI_SetSensorReset(1) failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_MIPI_SetMipiReset(mipi_dev, 1);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_MIPI_SetMipiReset(1) failed, ret=" + std::to_string(ret));
      return false;
    }
  }

  for (int i = 0; i < dev_num; ++i) {
    const int vi_pipe = i;
    const int mipi_dev = sensor_cfg_.sns_ini_cfg.MipiDev[i];
    SNS_COMBO_DEV_ATTR_S rx_attr;
    std::memset(&rx_attr, 0, sizeof(rx_attr));
    int ret = CVI_SNS_GetSnsRxAttr(i, &rx_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_SNS_GetSnsRxAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_MIPI_SetMipiAttr(mipi_dev, &rx_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_MIPI_SetMipiAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    const int mclk_en = sensor_cfg_.sns_ini_cfg.stMclkAttr[i].bMclkEn ? 1 : 0;
    const int mclk_sel = sensor_cfg_.sns_ini_cfg.stMclkAttr[i].u8Mclk;
    std::fprintf(stderr,
                 "sensor_media: set sensor clock vi_pipe=%d mipi_dev=%d mclk_en=%d mclk=%d\n",
                 vi_pipe, mipi_dev, mclk_en, mclk_sel);
    ret = CVI_MIPI_SetSensorClock(vi_pipe, mclk_en);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_MIPI_SetSensorClock failed, ret=" + std::to_string(ret));
      return false;
    }
    usleep(20);
    ret = CVI_MIPI_SetSensorReset(
        mipi_dev, sensor_cfg_.sns_ini_cfg.s32RstPort[i],
        sensor_cfg_.sns_ini_cfg.s32RstPin[i], sensor_cfg_.sns_ini_cfg.s32RstPol[i], 0);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_MIPI_SetSensorReset(0) failed, ret=" + std::to_string(ret));
      return false;
    }
  }
  return true;
}

bool SensorMediaImpl::probeSensor(std::string *error) {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    const int ret = CVI_SNS_SetSnsProbe(i);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_SNS_SetSnsProbe failed, ret=" + std::to_string(ret));
      return false;
    }
  }
  return true;
}

bool SensorMediaImpl::startViPipe(std::string *error) {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    VI_PIPE_ATTR_S pipe_attr = makeViPipeAttr(sensor_cfg_.sns_cfg, i);
    int ret = CVI_VI_CreatePipe(i, &pipe_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_CreatePipe failed, ret=" + std::to_string(ret));
      return false;
    }
    vi_pipe_created_[i] = true;

    VI_PIPE_ATTR_S current_attr;
    std::memset(&current_attr, 0, sizeof(current_attr));
    ret = CVI_VI_GetPipeAttr(i, &current_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_GetPipeAttr failed, ret=" + std::to_string(ret));
      return false;
    }

    ret = CVI_VI_StartPipe(i);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_StartPipe failed, ret=" + std::to_string(ret));
      return false;
    }
    vi_pipe_started_[i] = true;
  }
  return true;
}

bool SensorMediaImpl::startIsp(std::string *error) {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    ALG_LIB_S ae_lib = makeAeLib(i);
    ALG_LIB_S awb_lib = makeAwbLib(i);

    int ret = CVI_AE_Register(i, &ae_lib);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_AE_Register failed, ret=" + std::to_string(ret));
      return false;
    }
    ae_registered_[i] = true;

    ret = CVI_AWB_Register(i, &awb_lib);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_AWB_Register failed, ret=" + std::to_string(ret));
      return false;
    }
    awb_registered_[i] = true;

    if (!configureIsp(i, error)) {
      return false;
    }
    if (!startIspThread(i, &isp_threads_[i], error)) {
      return false;
    }
    isp_thread_started_[i] = true;
    isp_running_[i] = true;
  }
  return true;
}

bool SensorMediaImpl::configureIsp(int pipe, std::string *error) {
  ISP_BIND_ATTR_S bind_attr;
  std::memset(&bind_attr, 0, sizeof(bind_attr));
  bind_attr.stAeLib = makeAeLib(pipe);
  bind_attr.stAwbLib = makeAwbLib(pipe);
  bind_attr.sensorId = pipe;

  int ret = CVI_ISP_SetBindAttr(pipe, &bind_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_SetBindAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_ISP_MemInit(pipe);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_MemInit failed, ret=" + std::to_string(ret));
    return false;
  }

  ISP_PUB_ATTR_S pub_attr;
  std::memset(&pub_attr, 0, sizeof(pub_attr));
  pub_attr.stWndRect.s32X = 0;
  pub_attr.stWndRect.s32Y = 0;
  pub_attr.stWndRect.u32Width = sensor_cfg_.sns_cfg.u32ImageWigth[pipe];
  pub_attr.stWndRect.u32Height = sensor_cfg_.sns_cfg.u32ImageHeight[pipe];
  pub_attr.stSnsSize.u32Width = sensor_cfg_.sns_cfg.u32ImageWigth[pipe];
  pub_attr.stSnsSize.u32Height = sensor_cfg_.sns_cfg.u32ImageHeight[pipe];
  pub_attr.f32FrameRate = sensor_cfg_.sns_cfg.f32FrameRate[pipe];
  pub_attr.enBayer =
      static_cast<ISP_BAYER_FORMAT_E>(sensor_cfg_.sns_cfg.enBayerFormat[pipe]);
  pub_attr.enWDRMode = sensor_cfg_.sns_cfg.enWDRMode[pipe];

  std::fprintf(stderr,
               "sensor_media: configure isp pipe=%d size=%ux%u fps=%.2f bayer=%d wdr=%d\n",
               pipe, pub_attr.stWndRect.u32Width, pub_attr.stWndRect.u32Height,
               pub_attr.f32FrameRate, static_cast<int>(pub_attr.enBayer),
               static_cast<int>(pub_attr.enWDRMode));

  ret = CVI_ISP_SetPubAttr(pipe, &pub_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  if (!configureIspStats(pipe, pub_attr, error)) {
    return false;
  }

  ret = CVI_ISP_Init(pipe);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ISP_Init failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool SensorMediaImpl::startSensorInit(std::string *error) {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    int ret = CVI_SNS_SetSnsInit(i);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_SNS_SetSnsInit failed, ret=" + std::to_string(ret));
      return false;
    }
  }
  return true;
}

bool SensorMediaImpl::startViChannel(std::string *error) {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    VI_CHN_ATTR_S chn_attr = makeViChnAttr(sensor_cfg_.sns_cfg, i);
    if (sensor_cfg_.sns_ini_cfg.u8Orien[i] <= 3) {
      chn_attr.bMirror = sensor_cfg_.sns_ini_cfg.u8Orien[i] & 0x1;
      chn_attr.bFlip = (sensor_cfg_.sns_ini_cfg.u8Orien[i] & 0x2) >> 1;
    } else {
      chn_attr.bMirror = CVI_FALSE;
      chn_attr.bFlip = CVI_FALSE;
    }

    int ret = CVI_VI_SetChnAttr(i, config_.vi_channel, &chn_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_SetChnAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_SNS_SetVIFlipMirrorCB(i, i);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_SNS_SetVIFlipMirrorCB failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_VI_EnableChn(i, config_.vi_channel);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VI_EnableChn failed, ret=" + std::to_string(ret));
      return false;
    }
    vi_chn_enabled_[i] = true;
  }
  return true;
}

void SensorMediaImpl::stopIsp() {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    if (isp_running_[i]) {
      CVI_ISP_Exit(i);
    }
    if (isp_thread_started_[i]) {
      pthread_join(isp_threads_[i], nullptr);
      isp_thread_started_[i] = false;
      isp_threads_[i] = pthread_t {};
    }
    isp_running_[i] = false;
    if (ae_registered_[i]) {
      ALG_LIB_S ae = makeAeLib(i);
      CVI_AE_UnRegister(i, &ae);
      ae_registered_[i] = false;
    }
    if (awb_registered_[i]) {
      ALG_LIB_S awb = makeAwbLib(i);
      CVI_AWB_UnRegister(i, &awb);
      awb_registered_[i] = false;
    }
  }
}

void SensorMediaImpl::stopVi() {
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    if (vi_chn_enabled_[i]) {
      CVI_VI_DisableChn(i, config_.vi_channel);
      vi_chn_enabled_[i] = false;
    }
    if (vi_pipe_started_[i]) {
      CVI_VI_StopPipe(i);
      vi_pipe_started_[i] = false;
    }
    if (vi_pipe_created_[i]) {
      CVI_VI_DestroyPipe(i);
      vi_pipe_created_[i] = false;
    }
    if (vi_dev_enabled_[i]) {
      CVI_VI_DisableDev(i);
      vi_dev_enabled_[i] = false;
    }
  }
}

}  // namespace private_media
}  // namespace tdl_app
