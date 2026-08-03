#include "mmf_cv184x_common.hpp"

#include "cvi_common.h"

using namespace mmf_cv184x;

extern "C" {
typedef struct cviIPCMSG_CONNECT_S {
  CVI_U32 u32RemoteId;
  CVI_U32 u32Port;
  CVI_U32 u32Priority;
} CVI_IPCMSG_CONNECT_S;

typedef struct cviIPCMSG_MESSAGE_S {
  CVI_BOOL bIsResp;
  CVI_U64 u64Id;
  CVI_U32 u32Module;
  CVI_U32 u32CMD;
  CVI_S32 s32RetVal;
  CVI_U32 u32BodyLen;
  CVI_S32 as32PrivData[8];
  CVI_VOID* pBody;
#ifdef __arm__
  CVI_U32 u32VirAddrPadding;
#endif
} CVI_IPCMSG_MESSAGE_S;

typedef void (*CVI_IPCMSG_HANDLE_FN_PTR)(CVI_S32 s32Id, CVI_IPCMSG_MESSAGE_S* pstMsg);

CVI_S32 CVI_IPCMSG_Connect(CVI_S32* ps32Id, const CVI_CHAR* pszServiceName,
                           CVI_IPCMSG_HANDLE_FN_PTR pfnMessageHandle);
CVI_S32 CVI_IPCMSG_Disconnect(CVI_S32 s32Id);
CVI_IPCMSG_MESSAGE_S* CVI_IPCMSG_CreateMessage(CVI_U32 u32Module, CVI_U32 u32CMD,
                                               CVI_VOID* pBody, CVI_U32 u32BodyLen);
CVI_S32 CVI_IPCMSG_SendSync(CVI_S32 s32Id, CVI_IPCMSG_MESSAGE_S* pstMsg,
                            CVI_IPCMSG_MESSAGE_S** ppstMsg, CVI_S32 s32TimeoutMs);
CVI_VOID CVI_IPCMSG_DestroyMessage(CVI_IPCMSG_MESSAGE_S* pstMsg);
}

struct mmf_camera {
  explicit mmf_camera(const mmf_camera_config_t& cfg)
      : config(cfg),
        camera(mmf_cvi::Camera::forSource(to_native_source(cfg.source),
                                          static_cast<int>(cfg.device),
                                          static_cast<int>(cfg.timeout_ms))) {}
  mmf_camera_config_t config;
  mmf_cvi::Camera camera;
  mmf_cvi::Frame frame;
  bool opened = false;
  bool frame_valid = false;
};

namespace {

struct CameraControlCache {
  mmf_camera_ae_mode_t ae_mode = MMF_CAMERA_AE_AUTO;
  mmf_camera_awb_mode_t awb_mode = MMF_CAMERA_AWB_AUTO;
  uint32_t exposure_us = 0;
  uint32_t analog_gain = 0;
  uint32_t isp_gain = 0;
  mmf_camera_wb_t wb{0, 0, 0};
};

std::mutex g_camera_ctrl_mutex;
CameraControlCache g_camera_ctrl;

constexpr const char* kMsgServiceName = "CVI_MMF_MSG";
constexpr CVI_S32 kIpcTimeoutMs = 3000;
constexpr CVI_U32 kDefaultIspPipe = 0;
// Must match cvi_msg/internal_include/msg/msg_isp.h on the small core.
enum IspMsgCommand : CVI_U32 {
  kIspCmdGetAeMode = 36,
  kIspCmdSetAeMode = 37,
  kIspCmdSetExposureTime = 38,
  kIspCmdSetAnalogGain = 39,
  kIspCmdSetIspDGain = 40,
  kIspCmdQueryExposureInfo = 41,
  kIspCmdGetAwbMode = 42,
  kIspCmdSetAwbMode = 43,
  kIspCmdSetWbGain = 44,
  kIspCmdQueryWbInfo = 45,
};

struct MsgIspU32 {
  uint32_t value;
};

struct MsgIspWbGain {
  uint16_t rgain;
  uint16_t grgain;
  uint16_t gbgain;
  uint16_t bgain;
};

struct MsgIspExposureInfo {
  uint32_t exp_time;
  uint32_t exposure;
  uint32_t again;
  uint32_t dgain;
  uint32_t ispdgain;
  uint32_t iso;
  uint32_t fps;
  uint32_t ave_luma;
  uint32_t stable;
  uint32_t over_range;
};

struct MsgIspWbInfo {
  uint32_t rgain;
  uint32_t grgain;
  uint32_t gbgain;
  uint32_t bgain;
  uint32_t saturation;
  uint32_t color_temp;
  int32_t bv;
};

std::mutex g_isp_msg_mutex;
bool g_msg_service_added = false;
CVI_S32 g_isp_msg_si_id = -1;

CVI_U32 msg_modfd(CVI_U32 mod, CVI_U32 dev, CVI_U32 chn, bool block) {
  return ((mod & 0xffu) << 24) | ((dev & 0xffu) << 16) | ((chn & 0xffu) << 8) |
         (block ? 1u : 0u);
}

mmf_result_t ensure_msg_service() {
  if (g_msg_service_added) {
    return MMF_OK;
  }
  // CVI_SYS_Init() has already registered CVI_MMF_MSG for this process.
  // Registering the same service again can corrupt the IPC pool on CV184x dual OS.
  g_msg_service_added = true;
  return MMF_OK;
}

mmf_result_t ensure_isp_msg_connection() {
  if (g_isp_msg_si_id >= 0) {
    return MMF_OK;
  }
  CVI_S32 ret = CVI_IPCMSG_Connect(&g_isp_msg_si_id, kMsgServiceName, nullptr);
  if (ret != CVI_SUCCESS) {
    g_isp_msg_si_id = -1;
    set_last_error("CVI_IPCMSG_Connect(CVI_MMF_MSG) failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  return MMF_OK;
}

mmf_result_t isp_msg_request(CVI_U32 command, const void* request, CVI_U32 request_len,
                             void* response, CVI_U32 response_len) {
  std::string error;
  if (!mmf_cvi::ensureMmfRuntimeInitialized(&error)) {
    return ok_or_error(false, error);
  }

  std::lock_guard<std::mutex> lock(g_isp_msg_mutex);
  mmf_result_t ready = ensure_msg_service();
  if (ready != MMF_OK) {
    return ready;
  }

  ready = ensure_isp_msg_connection();
  if (ready != MMF_OK) {
    return ready;
  }

  CVI_IPCMSG_MESSAGE_S* msg = CVI_IPCMSG_CreateMessage(
      msg_modfd(CVI_ID_ISP, kDefaultIspPipe, 0, true), command,
      const_cast<void*>(request), request_len);
  if (msg == nullptr) {
    set_last_error("CVI_IPCMSG_CreateMessage(ISP) failed");
    return MMF_EIO;
  }

  CVI_IPCMSG_MESSAGE_S* resp = nullptr;
  CVI_S32 ret = CVI_IPCMSG_SendSync(g_isp_msg_si_id, msg, &resp, kIpcTimeoutMs);
  CVI_IPCMSG_DestroyMessage(msg);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_IPCMSG_SendSync(ISP) failed, ret=" + std::to_string(ret) +
                   " cmd=" + std::to_string(command));
    return MMF_EIO;
  }

  mmf_result_t result = MMF_OK;
  if (resp == nullptr) {
    set_last_error("ISP response is null");
    result = MMF_EIO;
  } else if (resp->s32RetVal != CVI_SUCCESS) {
    set_last_error("ISP command failed, ret=" + std::to_string(resp->s32RetVal) +
                   " cmd=" + std::to_string(command));
    result = MMF_EIO;
  } else if (response_len > 0 &&
             (resp->pBody == nullptr || resp->u32BodyLen < response_len)) {
    set_last_error("ISP response body is invalid");
    result = MMF_EIO;
  } else if (response_len > 0) {
    std::memcpy(response, resp->pBody, response_len);
  }

  if (resp != nullptr) {
    CVI_IPCMSG_DestroyMessage(resp);
  }
  return result;
}

mmf_result_t isp_msg_set_u32(CVI_U32 command, uint32_t value) {
  MsgIspU32 request{value};
  return isp_msg_request(command, &request, sizeof(request), nullptr, 0);
}

mmf_result_t isp_msg_get_u32(CVI_U32 command, uint32_t* value) {
  if (value == nullptr) {
    return MMF_EINVAL;
  }
  MsgIspU32 response{0};
  mmf_result_t ret = isp_msg_request(command, nullptr, 0, &response, sizeof(response));
  if (ret == MMF_OK) {
    *value = response.value;
  }
  return ret;
}

mmf_result_t isp_msg_get_exposure(MsgIspExposureInfo* info) {
  if (info == nullptr) {
    return MMF_EINVAL;
  }
  std::memset(info, 0, sizeof(*info));
  return isp_msg_request(kIspCmdQueryExposureInfo, nullptr, 0, info, sizeof(*info));
}

mmf_result_t isp_msg_get_wb(MsgIspWbInfo* info) {
  if (info == nullptr) {
    return MMF_EINVAL;
  }
  std::memset(info, 0, sizeof(*info));
  return isp_msg_request(kIspCmdQueryWbInfo, nullptr, 0, info, sizeof(*info));
}

uint16_t clamp_wb_gain(uint32_t value) {
  if (value < 1U) {
    return 1U;
  }
  if (value > 0x3fffU) {
    return 0x3fffU;
  }
  return static_cast<uint16_t>(value);
}

}  // namespace

extern "C" {

mmf_result_t mmf_camera_get_default_config(mmf_camera_source_t source,
                                           mmf_camera_config_t* config) {
  if (config == nullptr)
    return MMF_EINVAL;
  std::memset(config, 0, sizeof(*config));
  config->source = source;
  config->device = MMF_CAMERA_DEVICE_FRONT;
  config->timeout_ms = 1000;
  switch (source) {
    case MMF_CAMERA_SRC_MAIN:
      return MMF_ENOTSUP;
    case MMF_CAMERA_SRC_AI:
      config->width = mmf_cvi::DualOsLayout::kAiWidth;
      config->height = mmf_cvi::DualOsLayout::kAiHeight;
      config->pixel_format = MMF_PIXFMT_RGB888_PLANAR;
      config->scale_mode = MMF_SCALE_FIT_BLACK;
      break;
    case MMF_CAMERA_SRC_LIVE:
      config->width = mmf_cvi::DualOsLayout::kLiveWidth;
      config->height = mmf_cvi::DualOsLayout::kLiveHeight;
      config->pixel_format = MMF_PIXFMT_NV12;
      config->scale_mode = MMF_SCALE_STRETCH;
      break;
    case MMF_CAMERA_SRC_SUBRGB:
      config->width = mmf_cvi::DualOsLayout::kSubRgbWidth;
      config->height = mmf_cvi::DualOsLayout::kSubRgbHeight;
      config->pixel_format = MMF_PIXFMT_NV21;
      config->scale_mode = MMF_SCALE_FIT_BLACK;
      break;
    case MMF_CAMERA_SRC_SCREEN:
      config->width = mmf_cvi::DualOsLayout::kScreenWidth;
      config->height = mmf_cvi::DualOsLayout::kScreenHeight;
      config->pixel_format = MMF_PIXFMT_NV12;
      config->scale_mode = MMF_SCALE_FIT_BLACK;
      break;
    case MMF_CAMERA_SRC_RGB:
      config->width = mmf_cvi::DualOsLayout::kRgbWidth;
      config->height = mmf_cvi::DualOsLayout::kRgbHeight;
      config->pixel_format = MMF_PIXFMT_RGB888_PLANAR;
      config->scale_mode = MMF_SCALE_STRETCH;
      break;
    default:
      return MMF_EINVAL;
  }
  return MMF_OK;
}

mmf_result_t mmf_camera_open(const mmf_camera_config_t* config, mmf_camera_t** camera) {
  if (config == nullptr || camera == nullptr)
    return MMF_EINVAL;
  if (config->device != MMF_CAMERA_DEVICE_FRONT &&
      config->device != MMF_CAMERA_DEVICE_REAR) {
    return MMF_EINVAL;
  }
  if (config->source == MMF_CAMERA_SRC_MAIN) {
    return MMF_ENOTSUP;
  }
  std::unique_ptr<mmf_camera_t> ptr(new mmf_camera_t(*config));
  std::string error;
  if (!ptr->camera.open(&error)) {
    return ok_or_error(false, error);
  }
  ptr->opened = true;
  *camera = ptr.release();
  return MMF_OK;
}

void mmf_camera_close(mmf_camera_t* camera) {
  if (camera == nullptr)
    return;
  camera->camera.close();
  delete camera;
}

mmf_result_t mmf_camera_get_status(mmf_camera_t* camera, mmf_camera_status_t* status) {
  if (camera == nullptr || status == nullptr)
    return MMF_EINVAL;
  std::memset(status, 0, sizeof(*status));
  status->opened = camera->opened ? MMF_TRUE : MMF_FALSE;
  status->sensor_online = MMF_TRUE;
  status->sensor_id = camera->config.device;
  status->active_config = camera->config;
  return MMF_OK;
}

mmf_result_t mmf_camera_list_outputs(mmf_camera_output_desc_t* outputs, size_t capacity,
                                     size_t* count) {
  static const struct {
    mmf_camera_source_t source;
    mmf_camera_device_t device;
    const char* name;
  } sources[] = {
      {MMF_CAMERA_SRC_AI, MMF_CAMERA_DEVICE_FRONT, "ai"},
      {MMF_CAMERA_SRC_LIVE, MMF_CAMERA_DEVICE_FRONT, "front_live"},
      {MMF_CAMERA_SRC_SUBRGB, MMF_CAMERA_DEVICE_FRONT, "subrgb"},
      {MMF_CAMERA_SRC_SCREEN, MMF_CAMERA_DEVICE_FRONT, "screen"},
      {MMF_CAMERA_SRC_RGB, MMF_CAMERA_DEVICE_FRONT, "rgb"},
      {MMF_CAMERA_SRC_AI, MMF_CAMERA_DEVICE_REAR, "rear_ai"},
      {MMF_CAMERA_SRC_LIVE, MMF_CAMERA_DEVICE_REAR, "rear_live"},
      {MMF_CAMERA_SRC_SUBRGB, MMF_CAMERA_DEVICE_REAR, "rear_subrgb"},
      {MMF_CAMERA_SRC_SCREEN, MMF_CAMERA_DEVICE_REAR, "rear_screen"},
      {MMF_CAMERA_SRC_RGB, MMF_CAMERA_DEVICE_REAR, "rear_rgb"},
  };
  if (count != nullptr)
    *count = sizeof(sources) / sizeof(sources[0]);
  if (outputs == nullptr)
    return MMF_OK;
  if (capacity < sizeof(sources) / sizeof(sources[0]))
    return MMF_EINVAL;
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    mmf_camera_config_t cfg;
    mmf_camera_get_default_config(sources[i].source, &cfg);
    cfg.device = sources[i].device;
    std::memset(&outputs[i], 0, sizeof(outputs[i]));
    outputs[i].source = sources[i].source;
    outputs[i].device = sources[i].device;
    outputs[i].name = sources[i].name;
    outputs[i].width = cfg.width;
    outputs[i].height = cfg.height;
    outputs[i].pixel_format = cfg.pixel_format;
    outputs[i].scale_mode = cfg.scale_mode;
    outputs[i].depth = 2;
    outputs[i].available = MMF_TRUE;
    (void)source_to_vpss(sources[i].source, sources[i].device,
                         &outputs[i].vpss_group, &outputs[i].vpss_channel);
    VPSS_CHN_ATTR_S attr;
    std::memset(&attr, 0, sizeof(attr));
    if (CVI_VPSS_GetChnAttr(outputs[i].vpss_group, outputs[i].vpss_channel, &attr) == CVI_SUCCESS) {
      outputs[i].width = attr.u32Width;
      outputs[i].height = attr.u32Height;
      outputs[i].pixel_format = from_native_pixfmt(static_cast<int>(attr.enPixelFormat));
      outputs[i].scale_mode = from_vpss_scale(attr);
      outputs[i].mirror = attr.bMirror ? MMF_TRUE : MMF_FALSE;
      outputs[i].flip = attr.bFlip ? MMF_TRUE : MMF_FALSE;
    }
  }
  return MMF_OK;
}

mmf_result_t mmf_camera_get_frame(mmf_camera_t* camera, mmf_video_frame_t* frame,
                                  uint32_t timeout_ms) {
  if (camera == nullptr || frame == nullptr)
    return MMF_EINVAL;
  camera->config.timeout_ms = timeout_ms;
  std::string error;
  if (!camera->camera.read(&camera->frame, &error)) {
    return ok_or_error(false, error);
  }
  std::memset(frame, 0, sizeof(*frame));
  frame->width = static_cast<uint32_t>(camera->frame.width);
  frame->height = static_cast<uint32_t>(camera->frame.height);
  frame->pixel_format = from_native_pixfmt(camera->frame.format);
  frame->sequence = camera->frame.sequence;
  frame->timestamp_us = camera->frame.timestamp_us;
  frame->priv = camera->frame.native;
  camera->frame_valid = true;
  return MMF_OK;
}

mmf_result_t mmf_camera_put_frame(mmf_camera_t* camera, mmf_video_frame_t* frame) {
  (void)frame;
  if (camera == nullptr)
    return MMF_EINVAL;
  camera->camera.releaseFrame();
  camera->frame_valid = false;
  return MMF_OK;
}

mmf_result_t mmf_camera_release_frame(mmf_camera_t* camera, mmf_video_frame_t* frame) {
  return mmf_camera_put_frame(camera, frame);
}

mmf_result_t mmf_camera_snapshot(mmf_camera_t* camera, const char* path, mmf_codec_t codec) {
  if (camera == nullptr || path == nullptr)
    return MMF_EINVAL;
  if (codec != MMF_CODEC_JPEG && codec != MMF_CODEC_NONE)
    return MMF_ENOTSUP;
  std::string error;
  return ok_or_error(camera->camera.snapshot(path, &error), error);
}

mmf_result_t mmf_camera_set_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t mode) {
  if (mode == MMF_SCALE_CENTER_CROP) {
    return MMF_ENOTSUP;
  }
  int group = 0;
  int channel = 0;
  if (!source_to_vpss(source, MMF_CAMERA_DEVICE_FRONT, &group, &channel))
    return MMF_EINVAL;
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  int ret = CVI_VPSS_GetChnAttr(group, channel, &attr);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_VPSS_GetChnAttr failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  apply_vpss_scale(mode, &attr);
  ret = CVI_VPSS_SetChnAttr(group, channel, &attr);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_VPSS_SetChnAttr failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  return MMF_OK;
}

mmf_result_t mmf_camera_get_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t* mode) {
  if (mode == nullptr)
    return MMF_EINVAL;
  int group = 0;
  int channel = 0;
  if (!source_to_vpss(source, MMF_CAMERA_DEVICE_FRONT, &group, &channel))
    return MMF_EINVAL;
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  int ret = CVI_VPSS_GetChnAttr(group, channel, &attr);
  if (ret != CVI_SUCCESS) {
    mmf_camera_config_t cfg;
    mmf_result_t cfg_ret = mmf_camera_get_default_config(source, &cfg);
    if (cfg_ret != MMF_OK)
      return cfg_ret;
    *mode = cfg.scale_mode;
    return MMF_OK;
  }
  *mode = from_vpss_scale(attr);
  return MMF_OK;
}

mmf_result_t mmf_camera_set_ae_mode(mmf_camera_ae_mode_t mode) {
  if (mode != MMF_CAMERA_AE_AUTO && mode != MMF_CAMERA_AE_MANUAL) {
    return MMF_EINVAL;
  }
  mmf_result_t ret =
      isp_msg_set_u32(kIspCmdSetAeMode, mode == MMF_CAMERA_AE_MANUAL ? 1U : 0U);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.ae_mode = mode;
  }
  return ret;
}

mmf_result_t mmf_camera_get_ae_mode(mmf_camera_ae_mode_t* mode) {
  if (mode == nullptr)
    return MMF_EINVAL;
  uint32_t value = 0;
  mmf_result_t ret = isp_msg_get_u32(kIspCmdGetAeMode, &value);
  if (ret != MMF_OK) {
    return ret;
  }
  *mode = value ? MMF_CAMERA_AE_MANUAL : MMF_CAMERA_AE_AUTO;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_awb_mode(mmf_camera_awb_mode_t mode) {
  if (mode != MMF_CAMERA_AWB_AUTO && mode != MMF_CAMERA_AWB_MANUAL) {
    return MMF_EINVAL;
  }
  mmf_result_t ret =
      isp_msg_set_u32(kIspCmdSetAwbMode, mode == MMF_CAMERA_AWB_MANUAL ? 1U : 0U);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.awb_mode = mode;
  }
  return ret;
}

mmf_result_t mmf_camera_get_awb_mode(mmf_camera_awb_mode_t* mode) {
  if (mode == nullptr)
    return MMF_EINVAL;
  uint32_t value = 0;
  mmf_result_t ret = isp_msg_get_u32(kIspCmdGetAwbMode, &value);
  if (ret != MMF_OK) {
    return ret;
  }
  *mode = value ? MMF_CAMERA_AWB_MANUAL : MMF_CAMERA_AWB_AUTO;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_exposure(uint32_t exposure_us) {
  mmf_result_t ret = isp_msg_set_u32(kIspCmdSetExposureTime, exposure_us);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.exposure_us = exposure_us;
  }
  return ret;
}

mmf_result_t mmf_camera_get_exposure(uint32_t* exposure_us) {
  if (exposure_us == nullptr)
    return MMF_EINVAL;
  MsgIspExposureInfo info;
  mmf_result_t ret = isp_msg_get_exposure(&info);
  if (ret != MMF_OK) {
    return ret;
  }
  *exposure_us = info.exp_time;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_gain(uint32_t analog_gain) {
  mmf_result_t ret = isp_msg_set_u32(kIspCmdSetAnalogGain, analog_gain);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.analog_gain = analog_gain;
  }
  return ret;
}

mmf_result_t mmf_camera_get_gain(uint32_t* analog_gain) {
  if (analog_gain == nullptr)
    return MMF_EINVAL;
  MsgIspExposureInfo info;
  mmf_result_t ret = isp_msg_get_exposure(&info);
  if (ret != MMF_OK) {
    return ret;
  }
  *analog_gain = info.again;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_isp_gain(uint32_t digital_gain) {
  mmf_result_t ret = isp_msg_set_u32(kIspCmdSetIspDGain, digital_gain);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.isp_gain = digital_gain;
  }
  return ret;
}

mmf_result_t mmf_camera_get_isp_gain(uint32_t* digital_gain) {
  if (digital_gain == nullptr)
    return MMF_EINVAL;
  MsgIspExposureInfo info;
  mmf_result_t ret = isp_msg_get_exposure(&info);
  if (ret != MMF_OK) {
    return ret;
  }
  *digital_gain = info.ispdgain;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_wb(const mmf_camera_wb_t* wb) {
  if (wb == nullptr)
    return MMF_EINVAL;
  MsgIspWbGain request{
      clamp_wb_gain(wb->red_gain),
      clamp_wb_gain(wb->green_gain),
      clamp_wb_gain(wb->green_gain),
      clamp_wb_gain(wb->blue_gain),
  };
  mmf_result_t ret = isp_msg_request(kIspCmdSetWbGain, &request, sizeof(request), nullptr, 0);
  if (ret == MMF_OK) {
    std::lock_guard<std::mutex> lock(g_camera_ctrl_mutex);
    g_camera_ctrl.wb = *wb;
  }
  return ret;
}

mmf_result_t mmf_camera_get_wb(mmf_camera_wb_t* wb) {
  if (wb == nullptr)
    return MMF_EINVAL;
  MsgIspWbInfo info;
  mmf_result_t ret = isp_msg_get_wb(&info);
  if (ret != MMF_OK) {
    return ret;
  }
  wb->red_gain = info.rgain;
  wb->green_gain = (info.grgain + info.gbgain) / 2;
  wb->blue_gain = info.bgain;
  return MMF_OK;
}

static mmf_result_t set_all_capture_orientation(bool mirror, bool flip, bool set_mirror,
                                                bool set_flip) {
  const int channels[] = {
      mmf_cvi::DualOsLayout::kAiChannel,
      mmf_cvi::DualOsLayout::kLiveChannel,
      mmf_cvi::DualOsLayout::kSubRgbChannel,
  };
  const int groups[] = {
      mmf_cvi::DualOsLayout::kCaptureVpssGroup,
      mmf_cvi::DualOsLayout::kRearVpssGroup,
  };
  for (size_t group = 0; group < sizeof(groups) / sizeof(groups[0]); ++group) {
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
      VPSS_CHN_ATTR_S attr;
      std::memset(&attr, 0, sizeof(attr));
      int ret = CVI_VPSS_GetChnAttr(groups[group], channels[i], &attr);
      if (ret != CVI_SUCCESS) {
        set_last_error("CVI_VPSS_GetChnAttr orientation failed, ret=" + std::to_string(ret));
        return MMF_EIO;
      }
      if (set_mirror)
        attr.bMirror = mirror ? CVI_TRUE : CVI_FALSE;
      if (set_flip)
        attr.bFlip = flip ? CVI_TRUE : CVI_FALSE;
      ret = CVI_VPSS_SetChnAttr(groups[group], channels[i], &attr);
      if (ret != CVI_SUCCESS) {
        set_last_error("CVI_VPSS_SetChnAttr orientation failed, ret=" + std::to_string(ret));
        return MMF_EIO;
      }
    }
  }
  return MMF_OK;
}

mmf_result_t mmf_camera_set_flip(mmf_bool_t enable) {
  return set_all_capture_orientation(false, enable == MMF_TRUE, false, true);
}

mmf_result_t mmf_camera_get_flip(mmf_bool_t* enable) {
  if (enable == nullptr)
    return MMF_EINVAL;
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  int ret = CVI_VPSS_GetChnAttr(mmf_cvi::DualOsLayout::kCaptureVpssGroup,
                                mmf_cvi::DualOsLayout::kLiveChannel, &attr);
  if (ret != CVI_SUCCESS)
    return MMF_EIO;
  *enable = attr.bFlip ? MMF_TRUE : MMF_FALSE;
  return MMF_OK;
}

mmf_result_t mmf_camera_set_mirror(mmf_bool_t enable) {
  return set_all_capture_orientation(enable == MMF_TRUE, false, true, false);
}

mmf_result_t mmf_camera_get_mirror(mmf_bool_t* enable) {
  if (enable == nullptr)
    return MMF_EINVAL;
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  int ret = CVI_VPSS_GetChnAttr(mmf_cvi::DualOsLayout::kCaptureVpssGroup,
                                mmf_cvi::DualOsLayout::kLiveChannel, &attr);
  if (ret != CVI_SUCCESS)
    return MMF_EIO;
  *enable = attr.bMirror ? MMF_TRUE : MMF_FALSE;
  return MMF_OK;
}

}  // extern "C"
