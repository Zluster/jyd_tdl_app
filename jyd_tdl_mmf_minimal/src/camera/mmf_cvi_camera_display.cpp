#include "mmf_cv184x_common.hpp"

namespace mmf_cvi {
void setError(std::string* error, const std::string& message) {
  if (error)
    *error = message;
}
bool ensureMmfRuntimeInitialized(std::string* error) {
  static std::mutex mutex;
  static bool initialized = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (initialized)
    return true;
  int ret = CVI_SYS_Init();
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_Init failed, ret=" + std::to_string(ret));
    return false;
  }
  initialized = true;
  return true;
}
MMF_CHN_S toMmfChannel(const MediaChannel& channel) {
  MMF_CHN_S out;
  std::memset(&out, 0, sizeof(out));
  switch (channel.module) {
    case MediaModule::Vpss:
      out.enModId = CVI_ID_VPSS;
      out.s32DevId = channel.device;
      out.s32ChnId = channel.channel;
      break;
    case MediaModule::Vo:
      out.enModId = CVI_ID_VO;
      out.s32DevId = channel.device;
      out.s32ChnId = channel.channel;
      break;
    case MediaModule::Vdec:
      out.enModId = CVI_ID_VDEC;
      out.s32ChnId = channel.channel;
      break;
    default:
      out.enModId = CVI_ID_BUTT;
      break;
  }
  return out;
}
Camera::Camera() = default;
Camera::Camera(const Config& config) : config_(config) {}
Camera::~Camera() {
  close();
}
Camera::Config Camera::forSource(CameraSourceId source, int timeout_ms) {
  Config c;
  c.timeout_ms = timeout_ms;
  switch (source) {
    case CameraSourceId::Main:
      c.group = DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kMainChannel;
      c.width = DualOsLayout::kMainWidth;
      c.height = DualOsLayout::kMainHeight;
      c.pixel_format = PixelFormat::NV12;
      break;
    case CameraSourceId::Ai:
      c.group = DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kAiChannel;
      c.width = DualOsLayout::kAiWidth;
      c.height = DualOsLayout::kAiHeight;
      c.pixel_format = PixelFormat::RGB888_PLANAR;
      break;
    case CameraSourceId::SubRgb:
      c.group = DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kSubRgbChannel;
      c.width = DualOsLayout::kSubRgbWidth;
      c.height = DualOsLayout::kSubRgbHeight;
      c.pixel_format = PixelFormat::NV21;
      break;
    case CameraSourceId::Screen:
      c.group = DualOsLayout::kDisplayVpssGroup;
      c.channel = DualOsLayout::kDisplayChannel;
      c.width = DualOsLayout::kScreenWidth;
      c.height = DualOsLayout::kScreenHeight;
      c.pixel_format = PixelFormat::NV12;
      break;
    case CameraSourceId::Live:
    default:
      c.group = DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kLiveChannel;
      c.width = DualOsLayout::kLiveWidth;
      c.height = DualOsLayout::kLiveHeight;
      c.pixel_format = PixelFormat::NV12;
      break;
  }
  return c;
}
bool Camera::open(std::string* error) {
  if (opened_)
    return true;
  if (!ensureMmfRuntimeInitialized(error))
    return false;
  opened_ = true;
  return true;
}
bool Camera::read(Frame* frame, std::string* error) {
  if (!frame)
    return false;
  if (!open(error))
    return false;
  releaseFrame();
  int ret = CVI_VPSS_GetChnFrame(config_.group, config_.channel, &frame_info_, config_.timeout_ms);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_GetChnFrame failed, group=" + std::to_string(config_.group) +
                        " chn=" + std::to_string(config_.channel) + " ret=" + std::to_string(ret));
    return false;
  }
  frame_valid_ = true;
  VIDEO_FRAME_S& v = frame_info_.stVFrame;
  frame->native = &frame_info_;
  frame->width = v.u32Width;
  frame->height = v.u32Height;
  frame->format = v.enPixelFormat;
  frame->sequence = v.u32TimeRef;
  frame->timestamp_us = v.u64PTS;
  return true;
}
void Camera::releaseFrame() {
  if (frame_valid_) {
    CVI_VPSS_ReleaseChnFrame(config_.group, config_.channel, &frame_info_);
    std::memset(&frame_info_, 0, sizeof(frame_info_));
    frame_valid_ = false;
  }
}
void Camera::close() {
  releaseFrame();
  opened_ = false;
}
bool Camera::snapshot(const std::string& path, std::string* error) {
  Frame frame;
  if (!read(&frame, error))
    return false;
  VencChannel venc(VencChannel::mjpeg(0, frame.width, frame.height, 0, 25, 25, 92));
  VencChannel::EncodedPacket packet;
  bool ok = venc.open(error) && venc.encode(frame, &packet, error);
  if (ok) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
      setError(error, "open snapshot file failed: " + path);
      ok = false;
    } else {
      for (const auto& b : packet.blocks)
        std::fwrite(b.data(), 1, b.size(), fp);
      std::fclose(fp);
    }
  }
  releaseFrame();
  return ok;
}
Display::Display() = default;
Display::Display(const Config& config) : config_(config) {}
Display::~Display() {
  close();
}

namespace {
ROTATION_E toRotation(int rotation) {
  switch (rotation) {
    case 90:
      return ROTATION_90;
    case 180:
      return ROTATION_180;
    case 270:
      return ROTATION_270;
    case 0:
    default:
      return ROTATION_0;
  }
}
}  // namespace

bool Display::open(std::string* error) {
  if (device_enabled_)
    return true;
  if (!ensureMmfRuntimeInitialized(error))
    return false;

  VO_PUB_ATTR_S pub_attr;
  std::memset(&pub_attr, 0, sizeof(pub_attr));
  pub_attr.enIntfType = static_cast<VO_INTF_TYPE_E>(config_.interface_type);
  pub_attr.enIntfSync = static_cast<VO_INTF_SYNC_E>(config_.interface_sync);

  int ret = CVI_VO_SetPubAttr(static_cast<VO_DEV>(config_.device), &pub_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_VO_Enable(static_cast<VO_DEV>(config_.device));
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
  layer_attr.u32DispFrmRt = static_cast<CVI_U32>(config_.frame_rate);
  layer_attr.enPixFormat = static_cast<PIXEL_FORMAT_E>(config_.pixel_format);

  ret = CVI_VO_SetVideoLayerAttr(static_cast<VO_LAYER>(config_.layer), &layer_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetVideoLayerAttr failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  ret = CVI_VO_EnableVideoLayer(static_cast<VO_LAYER>(config_.layer));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_EnableVideoLayer failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  layer_enabled_ = true;

  if (config_.display_buf_len > 0) {
    (void)CVI_VO_SetDisplayBufLen(static_cast<VO_LAYER>(config_.layer),
                                  static_cast<CVI_U32>(config_.display_buf_len));
  }

  VO_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.stRect.s32X = config_.channel_x;
  chn_attr.stRect.s32Y = config_.channel_y;
  chn_attr.stRect.u32Width = static_cast<CVI_U32>(config_.width);
  chn_attr.stRect.u32Height = static_cast<CVI_U32>(config_.height);
  chn_attr.u32Priority = static_cast<CVI_U32>(config_.priority);

  ret = CVI_VO_SetChnAttr(static_cast<VO_LAYER>(config_.layer),
                          static_cast<VO_CHN>(config_.channel), &chn_attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetChnAttr failed, ret=" + std::to_string(ret));
    close();
    return false;
  }

  ret = CVI_VO_SetChnRotation(static_cast<VO_LAYER>(config_.layer),
                              static_cast<VO_CHN>(config_.channel),
                              toRotation(config_.rotation));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_SetChnRotation failed, ret=" + std::to_string(ret));
    close();
    return false;
  }

  ret =
      CVI_VO_EnableChn(static_cast<VO_LAYER>(config_.layer), static_cast<VO_CHN>(config_.channel));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VO_EnableChn failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  channel_enabled_ = true;
  return true;
}
CameraSourceId Display::toCameraSource(Input input) {
  if (input == Input::Ai)
    return CameraSourceId::Ai;
  if (input == Input::Main)
    return CameraSourceId::Main;
  if (input == Input::SubRgb)
    return CameraSourceId::SubRgb;
  if (input == Input::Screen)
    return CameraSourceId::Screen;
  return CameraSourceId::Live;
}
bool Display::show(Input input, std::string* error) {
  if (input == Input::None)
    return hideLive(error);
  if (!open(error))
    return false;
  hideLive(nullptr);
  Camera::Config src_cfg = Camera::forSource(toCameraSource(input), 1000);
  bound_source_ = MediaChannel::vpss(src_cfg.group, src_cfg.channel);
  MMF_CHN_S src = toMmfChannel(bound_source_);
  MMF_CHN_S dst = toMmfChannel(MediaChannel::vo(config_.layer, config_.channel));
  int ret = CVI_SYS_Bind(&src, &dst);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_Bind display failed, ret=" + std::to_string(ret));
    return false;
  }
  input_ = input;
  bound_ = true;
  return true;
}
bool Display::hideLive(std::string* error) {
  (void)error;
  if (bound_) {
    MMF_CHN_S src = toMmfChannel(bound_source_);
    MMF_CHN_S dst = toMmfChannel(MediaChannel::vo(config_.layer, config_.channel));
    CVI_SYS_UnBind(&src, &dst);
    bound_ = false;
  }
  input_ = Input::None;
  return true;
}
bool Display::snapshot(const std::string& path, int timeout_ms, std::string* error) {
  Camera camera(
      Camera::forSource(toCameraSource(input_ == Input::None ? Input::Live : input_), timeout_ms));
  return camera.snapshot(path, error);
}
void Display::close() {
  hideLive(nullptr);
  if (channel_enabled_) {
    CVI_VO_DisableChn(static_cast<VO_LAYER>(config_.layer),
                      static_cast<VO_CHN>(config_.channel));
    channel_enabled_ = false;
  }
  if (layer_enabled_) {
    CVI_VO_DisableVideoLayer(static_cast<VO_LAYER>(config_.layer));
    layer_enabled_ = false;
  }
  if (device_enabled_) {
    CVI_VO_Disable(static_cast<VO_DEV>(config_.device));
    device_enabled_ = false;
  }
}
}  // namespace mmf_cvi
