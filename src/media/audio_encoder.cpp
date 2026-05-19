#include "tdl_app/audio_encoder.hpp"

#include <cstring>
#include <string>

#include "cvi_audio.h"
#include "cvi_comm_aenc.h"
#include "media/private/audio_runtime.hpp"
#include "media/private/audio_vendor_utils.hpp"

namespace tdl_app {
namespace {

constexpr int kAudioIoTimeoutMs = 1000;

}  // namespace

AudioEncoder::AudioEncoder() = default;

AudioEncoder::AudioEncoder(const Config &config) : config_(config) {}

AudioEncoder::~AudioEncoder() { close(); }

AudioEncoder::Config AudioEncoder::g711a(int channel, int points_per_frame) {
  Config config;
  config.channel = channel;
  config.payload_type = AudioPayloadType::G711A;
  config.points_per_frame = points_per_frame;
  return config;
}

AudioEncoder::Config AudioEncoder::g711u(int channel, int points_per_frame) {
  Config config = g711a(channel, points_per_frame);
  config.payload_type = AudioPayloadType::G711U;
  return config;
}

AudioEncoder::Config AudioEncoder::g726(int channel, int points_per_frame,
                                        AudioG726Bitrate bitrate) {
  Config config = g711a(channel, points_per_frame);
  config.payload_type = AudioPayloadType::G726;
  config.g726_bitrate = bitrate;
  return config;
}

bool AudioEncoder::open(std::string *error) {
  if (opened_) {
    return true;
  }
  if (!retainAudioRuntime(error)) {
    return false;
  }

  AENC_ATTR_G711_S g711_attr;
  AENC_ATTR_G726_S g726_attr;
  AENC_ATTR_ADPCM_S adpcm_attr;
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

  AENC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = private_audio::toVendor(config_.payload_type);
  attr.u32PtNumPerFrm = static_cast<CVI_U32>(config_.points_per_frame);
  attr.u32BufSize = static_cast<CVI_U32>(config_.buffer_size);
  attr.pValue = payload_attr;
  attr.bFileDbgMode = config_.file_debug_mode ? CVI_TRUE : CVI_FALSE;

  const int ret = CVI_AENC_CreateChn(config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(
        error, "CVI_AENC_CreateChn failed, ret=" + std::to_string(ret));
    return false;
  }

  opened_ = true;
  return true;
}

bool AudioEncoder::encodeFrame(const AudioFrame &frame, AudioEncodedStream *stream,
                               std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  if (!stream) {
    private_audio::setError(error, "audio encoder stream pointer is null");
    return false;
  }
  if (frame.channels.empty()) {
    private_audio::setError(error, "audio encoder frame is empty");
    return false;
  }

  AUDIO_FRAME_S vendor_frame = private_audio::toVendor(frame);
  int ret = CVI_AENC_SendFrame(config_.channel, &vendor_frame, nullptr);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AENC_SendFrame failed, ret=" + std::to_string(ret));
    return false;
  }

  AUDIO_STREAM_S vendor_stream;
  std::memset(&vendor_stream, 0, sizeof(vendor_stream));
  ret = CVI_AENC_GetStream(config_.channel, &vendor_stream, kAudioIoTimeoutMs);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AENC_GetStream failed, ret=" + std::to_string(ret));
    return false;
  }

  private_audio::fromVendor(vendor_stream, config_.payload_type, stream);
  CVI_AENC_ReleaseStream(config_.channel, &vendor_stream);
  return !stream->empty();
}

void AudioEncoder::close() {
  if (!opened_) {
    return;
  }
  CVI_AENC_DestroyChn(config_.channel);
  releaseAudioRuntime();
  opened_ = false;
}

bool AudioEncoder::isOpen() const { return opened_; }

}  // namespace tdl_app
