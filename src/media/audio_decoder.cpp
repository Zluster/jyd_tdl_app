#include "tdl_app/audio_decoder.hpp"

#include <cstring>
#include <string>

#include "cvi_audio.h"
#include "cvi_comm_adec.h"
#include "media/private/audio_runtime.hpp"
#include "media/private/audio_vendor_utils.hpp"

namespace tdl_app {
namespace {

constexpr int kAudioIoTimeoutMs = 1000;

}  // namespace

AudioDecoder::AudioDecoder() = default;

AudioDecoder::AudioDecoder(const Config &config) : config_(config) {}

AudioDecoder::~AudioDecoder() { close(); }

AudioDecoder::Config AudioDecoder::g711a(int channel, int sample_rate) {
  Config config;
  config.channel = channel;
  config.payload_type = AudioPayloadType::G711A;
  config.sample_rate = sample_rate;
  return config;
}

AudioDecoder::Config AudioDecoder::g711u(int channel, int sample_rate) {
  Config config = g711a(channel, sample_rate);
  config.payload_type = AudioPayloadType::G711U;
  return config;
}

AudioDecoder::Config AudioDecoder::g726(int channel, int sample_rate,
                                        AudioG726Bitrate bitrate) {
  Config config = g711a(channel, sample_rate);
  config.payload_type = AudioPayloadType::G726;
  config.g726_bitrate = bitrate;
  return config;
}

bool AudioDecoder::open(std::string *error) {
  if (opened_) {
    return true;
  }
  if (!retainAudioRuntime(error)) {
    return false;
  }

  ADEC_ATTR_G711_S g711_attr;
  ADEC_ATTR_G726_S g726_attr;
  ADEC_ATTR_ADPCM_S adpcm_attr;
  std::memset(&g711_attr, 0, sizeof(g711_attr));
  std::memset(&g726_attr, 0, sizeof(g726_attr));
  std::memset(&adpcm_attr, 0, sizeof(adpcm_attr));

  void *payload_attr = nullptr;
  switch (config_.payload_type) {
    case AudioPayloadType::G711A:
    case AudioPayloadType::G711U:
      payload_attr = &g711_attr;
      break;
    case AudioPayloadType::G726:
      g726_attr.enG726bps = private_audio::toVendor(config_.g726_bitrate);
      payload_attr = &g726_attr;
      break;
    case AudioPayloadType::AdpcmA:
      adpcm_attr.enADPCMType = private_audio::toVendor(config_.adpcm_type);
      payload_attr = &adpcm_attr;
      break;
  }

  ADEC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = private_audio::toVendor(config_.payload_type);
  attr.u32BufSize = static_cast<CVI_U32>(config_.buffer_size);
  attr.enMode = private_audio::toVendor(config_.decode_mode);
  attr.pValue = payload_attr;
  attr.bFileDbgMode = config_.file_debug_mode ? CVI_TRUE : CVI_FALSE;
  attr.s32BytesPerSample = config_.bytes_per_sample;
  attr.s32frame_size = config_.frame_size;
  attr.s32ChannelNums = config_.channel_count;
  attr.s32Sample_rate = config_.sample_rate;

  const int ret = CVI_ADEC_CreateChn(config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(
        error, "CVI_ADEC_CreateChn failed, ret=" + std::to_string(ret));
    return false;
  }

  opened_ = true;
  return true;
}

bool AudioDecoder::decodeStream(const AudioEncodedStream &stream,
                                AudioFrame *frame, bool block,
                                std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  if (!frame) {
    private_audio::setError(error, "audio decoder frame pointer is null");
    return false;
  }
  if (stream.empty()) {
    private_audio::setError(error, "audio decoder stream is empty");
    return false;
  }

  AUDIO_STREAM_S vendor_stream = private_audio::toVendor(stream);
  const int timeout_ms = block ? kAudioIoTimeoutMs : 0;
  int ret = CVI_ADEC_SendStream(config_.channel, &vendor_stream,
                                block ? CVI_TRUE : CVI_FALSE);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_ADEC_SendStream failed, ret=" + std::to_string(ret));
    return false;
  }

  AUDIO_FRAME_INFO_S vendor_frame_info;
  std::memset(&vendor_frame_info, 0, sizeof(vendor_frame_info));
  ret = CVI_ADEC_GetFrame(config_.channel, &vendor_frame_info, timeout_ms);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_ADEC_GetFrame failed, ret=" + std::to_string(ret));
    return false;
  }

  if (!vendor_frame_info.pstFrame) {
    private_audio::setError(error, "audio decoder returned null frame");
    return false;
  }

  const AudioBitWidth bit_width = config_.bytes_per_sample >= 4
                                      ? AudioBitWidth::Bits32
                                      : config_.bytes_per_sample >= 3
                                            ? AudioBitWidth::Bits24
                                            : config_.bytes_per_sample >= 2
                                                  ? AudioBitWidth::Bits16
                                                  : AudioBitWidth::Bits8;
  const AudioSoundMode sound_mode =
      config_.channel_count > 1 ? AudioSoundMode::Stereo : AudioSoundMode::Mono;
  private_audio::fromVendor(*vendor_frame_info.pstFrame, bit_width, sound_mode,
                            frame);
  CVI_ADEC_ReleaseFrame(config_.channel, &vendor_frame_info);
  if (frame->empty()) {
    private_audio::setError(error, "audio decoder returned empty frame");
    return false;
  }
  return true;
}

bool AudioDecoder::clearBuffer(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio decoder is not open");
    return false;
  }
  const int ret = CVI_ADEC_ClearChnBuf(config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_ADEC_ClearChnBuf failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioDecoder::sendEndOfStream(bool instant, std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio decoder is not open");
    return false;
  }
  const int ret =
      CVI_ADEC_SendEndOfStream(config_.channel, instant ? CVI_TRUE : CVI_FALSE);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_ADEC_SendEndOfStream failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

void AudioDecoder::close() {
  if (!opened_) {
    return;
  }
  CVI_ADEC_DestroyChn(config_.channel);
  releaseAudioRuntime();
  opened_ = false;
}

bool AudioDecoder::isOpen() const { return opened_; }

}  // namespace tdl_app
