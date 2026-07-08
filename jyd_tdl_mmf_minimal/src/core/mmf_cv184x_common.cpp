#include "mmf_cv184x_common.hpp"

#include <algorithm>
namespace mmf_cv184x {

std::mutex g_error_mutex;
std::string g_last_error;

void set_last_error(const std::string& error) {
  std::lock_guard<std::mutex> lock(g_error_mutex);
  g_last_error = error;
}

mmf_result_t ok_or_error(bool ok, const std::string& error) {
  if (ok) {
    return MMF_OK;
  }
  set_last_error(error.empty() ? "operation failed" : error);
  return MMF_EIO;
}

mmf_cvi::CameraSourceId to_native_source(mmf_camera_source_t source) {
  switch (source) {
    case MMF_CAMERA_SRC_AI:
      return mmf_cvi::CameraSourceId::Ai;
    case MMF_CAMERA_SRC_LIVE:
      return mmf_cvi::CameraSourceId::Live;
    case MMF_CAMERA_SRC_MAIN:
      return mmf_cvi::CameraSourceId::Main;
    case MMF_CAMERA_SRC_SUBRGB:
      return mmf_cvi::CameraSourceId::SubRgb;
    case MMF_CAMERA_SRC_SCREEN:
      return mmf_cvi::CameraSourceId::Screen;
  }
  return mmf_cvi::CameraSourceId::Live;
}

bool source_to_vpss(mmf_camera_source_t source, int* group, int* channel) {
  if (group == nullptr || channel == nullptr) {
    return false;
  }
  switch (source) {
    case MMF_CAMERA_SRC_MAIN:
      *group = mmf_cvi::DualOsLayout::kCaptureVpssGroup;
      *channel = mmf_cvi::DualOsLayout::kMainChannel;
      return true;
    case MMF_CAMERA_SRC_AI:
      *group = mmf_cvi::DualOsLayout::kCaptureVpssGroup;
      *channel = mmf_cvi::DualOsLayout::kAiChannel;
      return true;
    case MMF_CAMERA_SRC_LIVE:
      *group = mmf_cvi::DualOsLayout::kCaptureVpssGroup;
      *channel = mmf_cvi::DualOsLayout::kLiveChannel;
      return true;
    case MMF_CAMERA_SRC_SUBRGB:
      *group = mmf_cvi::DualOsLayout::kCaptureVpssGroup;
      *channel = mmf_cvi::DualOsLayout::kSubRgbChannel;
      return true;
    case MMF_CAMERA_SRC_SCREEN:
      *group = mmf_cvi::DualOsLayout::kDisplayVpssGroup;
      *channel = mmf_cvi::DualOsLayout::kDisplayChannel;
      return true;
  }
  return false;
}

mmf_scale_mode_t from_vpss_scale(const VPSS_CHN_ATTR_S& attr) {
  if (attr.stAspectRatio.enMode == ASPECT_RATIO_AUTO) {
    return MMF_SCALE_FIT_BLACK;
  }
  if (attr.stAspectRatio.enMode == ASPECT_RATIO_MANUAL) {
    return MMF_SCALE_CENTER_CROP;
  }
  return MMF_SCALE_STRETCH;
}

void apply_vpss_scale(mmf_scale_mode_t mode, VPSS_CHN_ATTR_S* attr) {
  if (attr == nullptr) {
    return;
  }
  if (mode == MMF_SCALE_FIT_BLACK) {
    attr->stAspectRatio.enMode = ASPECT_RATIO_AUTO;
    attr->stAspectRatio.bEnableBgColor = CVI_TRUE;
    attr->stAspectRatio.u32BgColor = 0;
    std::memset(&attr->stAspectRatio.stVideoRect, 0, sizeof(attr->stAspectRatio.stVideoRect));
  } else if (mode == MMF_SCALE_STRETCH) {
    attr->stAspectRatio.enMode = ASPECT_RATIO_NONE;
    attr->stAspectRatio.bEnableBgColor = CVI_FALSE;
    attr->stAspectRatio.u32BgColor = 0;
    std::memset(&attr->stAspectRatio.stVideoRect, 0, sizeof(attr->stAspectRatio.stVideoRect));
  }
}

mmf_pixel_format_t from_native_pixfmt(int format) {
  switch (format) {
    case mmf_cvi::PixelFormat::NV12:
      return MMF_PIXFMT_NV12;
    case mmf_cvi::PixelFormat::NV21:
      return MMF_PIXFMT_NV21;
    case mmf_cvi::PixelFormat::RGB888:
      return MMF_PIXFMT_RGB888;
    case mmf_cvi::PixelFormat::BGR888:
      return MMF_PIXFMT_BGR888;
    case mmf_cvi::PixelFormat::RGB888_PLANAR:
      return MMF_PIXFMT_RGB888_PLANAR;
    case mmf_cvi::PixelFormat::ARGB8888:
      return MMF_PIXFMT_ARGB8888;
    case mmf_cvi::PixelFormat::YUV400:
      return MMF_PIXFMT_GRAY8;
    default:
      return MMF_PIXFMT_UNKNOWN;
  }
}

int to_native_pixfmt(mmf_pixel_format_t format) {
  switch (format) {
    case MMF_PIXFMT_NV12:
      return mmf_cvi::PixelFormat::NV12;
    case MMF_PIXFMT_NV21:
      return mmf_cvi::PixelFormat::NV21;
    case MMF_PIXFMT_RGB888:
      return mmf_cvi::PixelFormat::RGB888;
    case MMF_PIXFMT_BGR888:
      return mmf_cvi::PixelFormat::BGR888;
    case MMF_PIXFMT_RGB888_PLANAR:
      return mmf_cvi::PixelFormat::RGB888_PLANAR;
    case MMF_PIXFMT_ARGB8888:
      return mmf_cvi::PixelFormat::ARGB8888;
    case MMF_PIXFMT_GRAY8:
      return mmf_cvi::PixelFormat::YUV400;
    default:
      return mmf_cvi::PixelFormat::NV21;
  }
}

mmf_cvi::AudioBitWidth to_native_bit_width(mmf_audio_format_t format) {
  switch (format) {
    case MMF_AUDIO_FMT_S24_LE:
      return mmf_cvi::AudioBitWidth::Bits24;
    case MMF_AUDIO_FMT_S32_LE:
      return mmf_cvi::AudioBitWidth::Bits32;
    case MMF_AUDIO_FMT_S16_LE:
    default:
      return mmf_cvi::AudioBitWidth::Bits16;
  }
}

mmf_audio_format_t from_native_bit_width(mmf_cvi::AudioBitWidth bit_width) {
  switch (bit_width) {
    case mmf_cvi::AudioBitWidth::Bits24:
      return MMF_AUDIO_FMT_S24_LE;
    case mmf_cvi::AudioBitWidth::Bits32:
      return MMF_AUDIO_FMT_S32_LE;
    case mmf_cvi::AudioBitWidth::Bits16:
    default:
      return MMF_AUDIO_FMT_S16_LE;
  }
}

size_t bytes_per_sample(mmf_audio_format_t format) {
  switch (format) {
    case MMF_AUDIO_FMT_S24_LE:
      return 3;
    case MMF_AUDIO_FMT_S32_LE:
      return 4;
    case MMF_AUDIO_FMT_S16_LE:
      return 2;
    default:
      return 0;
  }
}

mmf_cvi::AudioSampleRate to_native_sample_rate(uint32_t rate) {
  switch (rate) {
    case 8000:
      return mmf_cvi::AudioSampleRate::Hz8000;
    case 11025:
      return mmf_cvi::AudioSampleRate::Hz11025;
    case 22050:
      return mmf_cvi::AudioSampleRate::Hz22050;
    case 24000:
      return mmf_cvi::AudioSampleRate::Hz24000;
    case 32000:
      return mmf_cvi::AudioSampleRate::Hz32000;
    case 44100:
      return mmf_cvi::AudioSampleRate::Hz44100;
    case 48000:
      return mmf_cvi::AudioSampleRate::Hz48000;
    case 64000:
      return mmf_cvi::AudioSampleRate::Hz64000;
    case 16000:
    default:
      return mmf_cvi::AudioSampleRate::Hz16000;
  }
}

mmf_cvi::AudioPayloadType to_native_audio_payload(mmf_codec_t codec) {
  switch (codec) {
    case MMF_CODEC_G711U:
      return mmf_cvi::AudioPayloadType::G711U;
    case MMF_CODEC_G711A:
    default:
      return mmf_cvi::AudioPayloadType::G711A;
  }
}

mmf_codec_t from_native_audio_payload(mmf_cvi::AudioPayloadType codec) {
  switch (codec) {
    case mmf_cvi::AudioPayloadType::G711U:
      return MMF_CODEC_G711U;
    case mmf_cvi::AudioPayloadType::G711A:
    default:
      return MMF_CODEC_G711A;
  }
}

mmf_cvi::AudioInput::Config to_input_config(const mmf_audio_input_config_t& cfg) {
  mmf_cvi::AudioInput::Config out;
  out.device = cfg.io.ai_device;
  out.channel = cfg.io.ai_channel;
  out.sample_rate = to_native_sample_rate(cfg.io.sample_rate);
  out.bit_width = to_native_bit_width(cfg.io.format);
  out.sound_mode =
      cfg.io.channels > 1 ? mmf_cvi::AudioSoundMode::Stereo : mmf_cvi::AudioSoundMode::Mono;
  out.channel_count = static_cast<int>(cfg.io.channels);
  out.points_per_frame = static_cast<int>(cfg.io.samples_per_frame);
  out.frame_count = static_cast<int>(cfg.io.frame_count);
  out.frame_depth = static_cast<int>(cfg.io.frame_count);
  out.volume_step = cfg.io.input_volume;
  return out;
}

mmf_cvi::AudioOutput::Config to_output_config(const mmf_audio_output_config_t& cfg) {
  mmf_cvi::AudioOutput::Config out;
  out.device = cfg.io.ao_device;
  out.channel = cfg.io.ao_channel;
  out.sample_rate = to_native_sample_rate(cfg.io.sample_rate);
  out.bit_width = to_native_bit_width(cfg.io.format);
  out.sound_mode =
      cfg.io.channels > 1 ? mmf_cvi::AudioSoundMode::Stereo : mmf_cvi::AudioSoundMode::Mono;
  out.channel_count = static_cast<int>(cfg.io.channels);
  out.points_per_frame = static_cast<int>(cfg.io.samples_per_frame);
  out.frame_count = static_cast<int>(cfg.io.frame_count);
  out.volume_db = cfg.io.output_volume;
  return out;
}

mmf_cvi::AudioTalkVqeConfig to_talk_vqe(const mmf_audio_3a_config_t& cfg, uint32_t sample_rate) {
  mmf_cvi::AudioTalkVqeConfig out =
      mmf_cvi::AudioTalkVqeConfig::talk3a(static_cast<int>(sample_rate));
  out.open_mask = 0;
  if (cfg.aec_enable)
    out.open_mask |= 0x1;
  if (cfg.ns_enable)
    out.open_mask |= 0x4;
  if (cfg.agc_enable)
    out.open_mask |= 0x8;
  out.aec.filter_length = static_cast<uint16_t>(cfg.vendor_filter_len);
  out.aec.std_threshold = static_cast<uint16_t>(cfg.vendor_std_thrd);
  out.aec.suppress_coeff = static_cast<uint16_t>(cfg.vendor_supp_coeff);
  out.anr.snr_coeff = static_cast<uint16_t>(cfg.vendor_snr_coeff);
  out.agc.max_gain = static_cast<int8_t>(std::min<uint32_t>(cfg.agc_max_gain, 3));
  out.agc.target_high =
      static_cast<int8_t>(std::min<uint32_t>(static_cast<uint32_t>(
                                                cfg.agc_target_db < 0 ? 0 : cfg.agc_target_db),
                                            36));
  out.agc.target_low = static_cast<int8_t>(std::min<uint32_t>(cfg.agc_compress, 36));
  out.agc.vad_enabled = cfg.vendor_vad_enable == MMF_TRUE;
  out.delay.delay_sample = static_cast<uint16_t>(cfg.aec_delay_ms > 0 ? cfg.aec_delay_ms : 0);
  return out;
}

void fill_default_3a(mmf_audio_3a_config_t* config) {
  if (config == nullptr) {
    return;
  }
  std::memset(config, 0, sizeof(*config));
  config->aec_enable = MMF_TRUE;
  config->ns_enable = MMF_TRUE;
  config->agc_enable = MMF_TRUE;
  config->aec_level = 5;
  config->aec_delay_ms = 0;
  config->ns_level = 5;
  config->agc_target_db = 2;
  config->agc_max_gain = 1;
  config->agc_compress = 6;
  config->vendor_filter_len = 13;
  config->vendor_std_thrd = 37;
  config->vendor_supp_coeff = 60;
  config->vendor_snr_coeff = 15;
  config->vendor_vad_enable = MMF_TRUE;
}

mmf_result_t copy_native_video_frame(const mmf_video_frame_t& src, mmf_video_frame_t* dst,
                                     std::vector<uint8_t>* storage) {
  if (dst == nullptr || storage == nullptr || src.priv == nullptr) {
    return MMF_EINVAL;
  }

  const auto* native = static_cast<const VIDEO_FRAME_INFO_S*>(src.priv);
  const VIDEO_FRAME_S& vf = native->stVFrame;
  size_t total = 0;
  for (int i = 0; i < 3; ++i) {
    total += static_cast<size_t>(vf.u32Length[i]);
  }
  if (total == 0 || vf.u64PhyAddr[0] == 0) {
    set_last_error("decoded frame has no readable buffer");
    return MMF_EIO;
  }

  auto* mapped = static_cast<uint8_t*>(CVI_SYS_Mmap(vf.u64PhyAddr[0], total));
  if (mapped == nullptr) {
    set_last_error("CVI_SYS_Mmap decoded frame failed");
    return MMF_EIO;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, total);

  storage->assign(mapped, mapped + total);
  CVI_SYS_Munmap(mapped, total);

  std::memset(dst, 0, sizeof(*dst));
  dst->width = src.width;
  dst->height = src.height;
  dst->pixel_format = src.pixel_format;
  dst->sequence = src.sequence;
  dst->timestamp_us = src.timestamp_us;
  dst->priv = nullptr;
  size_t offset = 0;
  for (int i = 0; i < 3; ++i) {
    dst->stride[i] = vf.u32Stride[i];
    dst->plane_bytes[i] = vf.u32Length[i];
    if (vf.u32Length[i] > 0) {
      dst->plane[i] = storage->data() + offset;
      offset += vf.u32Length[i];
    }
  }
  return MMF_OK;
}

}  // namespace mmf_cv184x
