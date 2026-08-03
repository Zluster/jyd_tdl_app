#include "mmf_cv184x_common.hpp"

#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "cvi_vb.h"

namespace mmf_cvi {
namespace {

uint8_t clampToByte(int value) {
  return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

bool isRgbPlanar(PIXEL_FORMAT_E format) {
  return format == PIXEL_FORMAT_RGB_888_PLANAR ||
         format == PIXEL_FORMAT_BGR_888_PLANAR;
}

struct TemporaryNv12Frame {
  VIDEO_FRAME_INFO_S frame{};
  VB_BLK block = VB_INVALID_HANDLE;

  void release() {
    for (int plane = 0; plane < 2; ++plane) {
      if (frame.stVFrame.pu8VirAddr[plane] != nullptr) {
        CVI_SYS_Munmap(frame.stVFrame.pu8VirAddr[plane],
                       frame.stVFrame.u32Length[plane]);
        frame.stVFrame.pu8VirAddr[plane] = nullptr;
      }
    }
    if (block != VB_INVALID_HANDLE) {
      CVI_VB_ReleaseBlock(block);
      block = VB_INVALID_HANDLE;
    }
    std::memset(&frame, 0, sizeof(frame));
  }

  ~TemporaryNv12Frame() { release(); }
};

bool convertRgbPlanarToNv12(const Frame& source, TemporaryNv12Frame* destination,
                            std::string* error) {
  if (destination == nullptr || source.native == nullptr) {
    setError(error, "RGB-to-NV12 conversion received an invalid frame");
    return false;
  }

  const auto* input = static_cast<const VIDEO_FRAME_INFO_S*>(source.native);
  const VIDEO_FRAME_S& in = input->stVFrame;
  if (!isRgbPlanar(in.enPixelFormat) || in.u32Width == 0 || in.u32Height == 0 ||
      (in.u32Width & 1U) != 0 || (in.u32Height & 1U) != 0) {
    setError(error, "RGB-to-NV12 conversion requires an even-sized RGB planar frame");
    return false;
  }
  for (int plane = 0; plane < 3; ++plane) {
    if (in.u64PhyAddr[plane] == 0 || in.u32Length[plane] == 0 ||
        in.u32Stride[plane] < in.u32Width) {
      setError(error, "RGB-to-NV12 conversion received invalid RGB planar metadata");
      return false;
    }
  }

  VB_CAL_CONFIG_S calc;
  COMMON_GetPicBufferConfig(in.u32Width, in.u32Height, PIXEL_FORMAT_NV12,
                            DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN, &calc);

  VIDEO_FRAME_S& out = destination->frame.stVFrame;
  out.enCompressMode = COMPRESS_MODE_NONE;
  out.enPixelFormat = PIXEL_FORMAT_NV12;
  out.enVideoFormat = VIDEO_FORMAT_LINEAR;
  out.enColorGamut = COLOR_GAMUT_BT709;
  out.enDynamicRange = DYNAMIC_RANGE_SDR8;
  out.u32Width = in.u32Width;
  out.u32Height = in.u32Height;
  out.u32Stride[0] = calc.u32MainStride;
  out.u32Stride[1] = calc.u32CStride;
  out.u32Length[0] = calc.u32MainYSize;
  out.u32Length[1] = calc.u32MainCSize;

  destination->block = CVI_VB_GetBlock(VB_INVALID_POOLID, calc.u32VBSize);
  if (destination->block == VB_INVALID_HANDLE) {
    setError(error, "CVI_VB_GetBlock for RGB-to-NV12 snapshot failed");
    return false;
  }
  destination->frame.u32PoolId = CVI_VB_Handle2PoolId(destination->block);
  out.u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(destination->block);
  out.u64PhyAddr[1] = out.u64PhyAddr[0] + ALIGN(calc.u32MainYSize, calc.u16AddrAlign);
  out.pu8VirAddr[0] = static_cast<CVI_U8*>(CVI_SYS_MmapCache(out.u64PhyAddr[0], out.u32Length[0]));
  out.pu8VirAddr[1] = static_cast<CVI_U8*>(CVI_SYS_MmapCache(out.u64PhyAddr[1], out.u32Length[1]));
  if (out.pu8VirAddr[0] == nullptr || out.pu8VirAddr[1] == nullptr) {
    setError(error, "CVI_SYS_MmapCache for RGB-to-NV12 snapshot failed");
    return false;
  }

  uint8_t* rgb[3] = {nullptr, nullptr, nullptr};
  for (int plane = 0; plane < 3; ++plane) {
    rgb[plane] = static_cast<uint8_t*>(CVI_SYS_Mmap(in.u64PhyAddr[plane], in.u32Length[plane]));
    if (rgb[plane] == nullptr) {
      for (int mapped = 0; mapped < plane; ++mapped) {
        CVI_SYS_Munmap(rgb[mapped], in.u32Length[mapped]);
      }
      setError(error, "CVI_SYS_Mmap for RGB planar source failed");
      return false;
    }
    CVI_SYS_IonInvalidateCache(in.u64PhyAddr[plane], rgb[plane], in.u32Length[plane]);
  }

  std::memset(out.pu8VirAddr[0], 0, out.u32Length[0]);
  std::memset(out.pu8VirAddr[1], 128, out.u32Length[1]);
  const bool bgr = in.enPixelFormat == PIXEL_FORMAT_BGR_888_PLANAR;
  const auto readRgb = [&rgb, &in, bgr](uint32_t x, uint32_t y, int* r, int* g, int* b) {
    const uint32_t offset0 = y * in.u32Stride[0] + x;
    const uint32_t offset1 = y * in.u32Stride[1] + x;
    const uint32_t offset2 = y * in.u32Stride[2] + x;
    if (bgr) {
      *b = rgb[0][offset0];
      *g = rgb[1][offset1];
      *r = rgb[2][offset2];
    } else {
      *r = rgb[0][offset0];
      *g = rgb[1][offset1];
      *b = rgb[2][offset2];
    }
  };

  for (uint32_t y = 0; y < in.u32Height; ++y) {
    uint8_t* y_row = out.pu8VirAddr[0] + y * out.u32Stride[0];
    for (uint32_t x = 0; x < in.u32Width; ++x) {
      int r, g, b;
      readRgb(x, y, &r, &g, &b);
      y_row[x] = clampToByte((77 * r + 150 * g + 29 * b + 128) >> 8);
    }
  }
  for (uint32_t y = 0; y < in.u32Height; y += 2) {
    uint8_t* uv_row = out.pu8VirAddr[1] + (y / 2) * out.u32Stride[1];
    for (uint32_t x = 0; x < in.u32Width; x += 2) {
      int r_sum = 0, g_sum = 0, b_sum = 0;
      for (uint32_t dy = 0; dy < 2; ++dy) {
        for (uint32_t dx = 0; dx < 2; ++dx) {
          int r, g, b;
          readRgb(x + dx, y + dy, &r, &g, &b);
          r_sum += r;
          g_sum += g;
          b_sum += b;
        }
      }
      const int r = (r_sum + 2) / 4;
      const int g = (g_sum + 2) / 4;
      const int b = (b_sum + 2) / 4;
      uv_row[x] = clampToByte(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
      uv_row[x + 1] = clampToByte(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
    }
  }

  for (int plane = 0; plane < 3; ++plane) {
    CVI_SYS_Munmap(rgb[plane], in.u32Length[plane]);
  }
  CVI_SYS_IonFlushCache(out.u64PhyAddr[0], out.pu8VirAddr[0], out.u32Length[0]);
  CVI_SYS_IonFlushCache(out.u64PhyAddr[1], out.pu8VirAddr[1], out.u32Length[1]);
  return true;
}

}  // namespace

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
  return forSource(source, 0, timeout_ms);
}

Camera::Config Camera::forSource(CameraSourceId source, int camera_device, int timeout_ms) {
  Config c;
  c.timeout_ms = timeout_ms;
  switch (source) {
    case CameraSourceId::Ai:
      c.group = camera_device == 1 ? DualOsLayout::kRearVpssGroup
                                   : DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kAiChannel;
      c.width = DualOsLayout::kAiWidth;
      c.height = DualOsLayout::kAiHeight;
      c.pixel_format = PixelFormat::RGB888_PLANAR;
      break;
    case CameraSourceId::SubRgb:
      c.group = camera_device == 1 ? DualOsLayout::kRearVpssGroup
                                   : DualOsLayout::kCaptureVpssGroup;
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
    case CameraSourceId::Rgb:
      c.group = camera_device == 1 ? DualOsLayout::kRearVpssGroup
                                   : DualOsLayout::kCaptureVpssGroup;
      c.channel = DualOsLayout::kRgbChannel;
      c.width = DualOsLayout::kRgbWidth;
      c.height = DualOsLayout::kRgbHeight;
      c.pixel_format = PixelFormat::RGB888_PLANAR;
      break;
    case CameraSourceId::Live:
    default:
      c.group = camera_device == 1 ? DualOsLayout::kRearVpssGroup
                                   : DualOsLayout::kCaptureVpssGroup;
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
  TemporaryNv12Frame converted;
  Frame encode_frame = frame;
  const auto* native = static_cast<const VIDEO_FRAME_INFO_S*>(frame.native);
  if (isRgbPlanar(native->stVFrame.enPixelFormat)) {
    if (!convertRgbPlanarToNv12(frame, &converted, error)) {
      releaseFrame();
      return false;
    }
    encode_frame.native = &converted.frame;
    encode_frame.format = PIXEL_FORMAT_NV12;
  }
  VencChannel venc(VencChannel::mjpeg(0, frame.width, frame.height, 0, 25, 25, 92));
  VencChannel::EncodedPacket packet;
  bool ok = venc.open(error) && venc.encode(encode_frame, &packet, error);
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

  int ret;
  if (config_.preserve_hardware_on_close) {
    // U-Boot owns panel/DSI startup. AliOS has no VO context after boot,
    // however, so register the configured full-screen output once before
    // enabling it. Without this, its inherited logo window remains 320x240
    // and rejects the 720x480 video layer rectangle.
    if (!CVI_VO_IsEnabled(static_cast<VO_DEV>(config_.device))) {
      VO_PUB_ATTR_S pub_attr;
      std::memset(&pub_attr, 0, sizeof(pub_attr));
      pub_attr.enIntfType = static_cast<VO_INTF_TYPE_E>(config_.interface_type);
      pub_attr.enIntfSync = static_cast<VO_INTF_SYNC_E>(config_.interface_sync);
      ret = CVI_VO_SetPubAttr(static_cast<VO_DEV>(config_.device), &pub_attr);
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VO_SetPubAttr for U-Boot handoff failed, ret=" +
                            std::to_string(ret));
        return false;
      }
      ret = CVI_VO_Enable(static_cast<VO_DEV>(config_.device));
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VO_Enable for U-Boot handoff failed, ret=" +
                            std::to_string(ret));
        return false;
      }
    }
  } else {
    VO_PUB_ATTR_S pub_attr;
    std::memset(&pub_attr, 0, sizeof(pub_attr));
    pub_attr.enIntfType = static_cast<VO_INTF_TYPE_E>(config_.interface_type);
    pub_attr.enIntfSync = static_cast<VO_INTF_SYNC_E>(config_.interface_sync);
    ret = CVI_VO_SetPubAttr(static_cast<VO_DEV>(config_.device), &pub_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VO_SetPubAttr failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_VO_Enable(static_cast<VO_DEV>(config_.device));
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VO_Enable failed, ret=" + std::to_string(ret));
      return false;
    }
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
  const Input snapshot_input = input_ == Input::None ? Input::Live : input_;
  Camera camera(Camera::forSource(toCameraSource(snapshot_input), timeout_ms));
  return camera.snapshot(path, error);
}
void Display::close() {
  hideLive(nullptr);
  if (config_.preserve_hardware_on_close) {
    return;
  }
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
