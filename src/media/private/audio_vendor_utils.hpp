#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "cvi_audio.h"
#include "tdl_app/audio_types.hpp"

namespace tdl_app {
namespace private_audio {

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

inline AUDIO_SAMPLE_RATE_E toVendor(AudioSampleRate value) {
  return static_cast<AUDIO_SAMPLE_RATE_E>(static_cast<int>(value));
}

inline AUDIO_BIT_WIDTH_E toVendor(AudioBitWidth value) {
  switch (value) {
    case AudioBitWidth::Bits8:
      return AUDIO_BIT_WIDTH_8;
    case AudioBitWidth::Bits16:
      return AUDIO_BIT_WIDTH_16;
    case AudioBitWidth::Bits24:
      return AUDIO_BIT_WIDTH_24;
    case AudioBitWidth::Bits32:
      return AUDIO_BIT_WIDTH_32;
  }
  return AUDIO_BIT_WIDTH_16;
}

inline AIO_MODE_E toVendor(AudioWorkMode value) {
  return static_cast<AIO_MODE_E>(static_cast<int>(value));
}

inline AIO_I2STYPE_E toVendor(AudioI2sType value) {
  return static_cast<AIO_I2STYPE_E>(static_cast<int>(value));
}

inline AUDIO_SOUND_MODE_E toVendor(AudioSoundMode value) {
  return value == AudioSoundMode::Stereo ? AUDIO_SOUND_MODE_STEREO
                                         : AUDIO_SOUND_MODE_MONO;
}

inline AUDIO_TRACK_MODE_E toVendor(AudioTrackMode value) {
  return static_cast<AUDIO_TRACK_MODE_E>(static_cast<int>(value));
}

inline AudioTrackMode fromVendor(AUDIO_TRACK_MODE_E value) {
  return static_cast<AudioTrackMode>(static_cast<int>(value));
}

inline AUDIO_FADE_RATE_E toVendor(AudioFadeRate value) {
  return static_cast<AUDIO_FADE_RATE_E>(static_cast<int>(value));
}

inline AudioFadeRate fromVendor(AUDIO_FADE_RATE_E value) {
  return static_cast<AudioFadeRate>(static_cast<int>(value));
}

inline PAYLOAD_TYPE_E toVendor(AudioPayloadType value) {
  return static_cast<PAYLOAD_TYPE_E>(static_cast<int>(value));
}

inline G726_BPS_E toVendor(AudioG726Bitrate value) {
  return static_cast<G726_BPS_E>(static_cast<int>(value));
}

inline ADPCM_TYPE_E toVendor(AudioAdpcmType value) {
  return static_cast<ADPCM_TYPE_E>(static_cast<int>(value));
}

inline ADEC_MODE_E toVendor(AudioDecodeMode value) {
  return static_cast<ADEC_MODE_E>(static_cast<int>(value));
}

inline AUDIO_FADE_S toVendor(const AudioFade &fade) {
  AUDIO_FADE_S out;
  std::memset(&out, 0, sizeof(out));
  out.bFade = fade.enabled ? CVI_TRUE : CVI_FALSE;
  out.enFadeInRate = toVendor(fade.fade_in);
  out.enFadeOutRate = toVendor(fade.fade_out);
  return out;
}

inline AudioFade fromVendor(const AUDIO_FADE_S &fade) {
  AudioFade out;
  out.enabled = fade.bFade == CVI_TRUE;
  out.fade_in = fromVendor(fade.enFadeInRate);
  out.fade_out = fromVendor(fade.enFadeOutRate);
  return out;
}

inline AUDIO_FRAME_S toVendor(const AudioFrame &frame) {
  AUDIO_FRAME_S out;
  std::memset(&out, 0, sizeof(out));
  out.enBitwidth = toVendor(frame.bit_width);
  out.enSoundmode = toVendor(frame.sound_mode);
  out.u64TimeStamp = frame.timestamp;
  out.u32Seq = frame.sequence;
  out.u32Len = frame.bytes_per_channel != 0
                   ? frame.bytes_per_channel
                   : static_cast<CVI_U32>(frame.channels.empty()
                                              ? 0
                                              : frame.channels.front().size());
  for (std::size_t i = 0; i < frame.channels.size() && i < 2; ++i) {
    out.u64VirAddr[i] =
        const_cast<CVI_U8 *>(reinterpret_cast<const CVI_U8 *>(
            frame.channels[i].data()));
  }
  return out;
}

inline void fromVendor(const AUDIO_FRAME_S &vendor_frame, AudioBitWidth bit_width,
                       AudioSoundMode sound_mode, AudioFrame *frame) {
  if (!frame) {
    return;
  }
  frame->bit_width = bit_width;
  frame->sound_mode = sound_mode;
  frame->timestamp = vendor_frame.u64TimeStamp;
  frame->sequence = vendor_frame.u32Seq;
  frame->bytes_per_channel = vendor_frame.u32Len;
  frame->channels.clear();
  for (int i = 0; i < 2; ++i) {
    if (!vendor_frame.u64VirAddr[i] || vendor_frame.u32Len == 0) {
      continue;
    }
    const std::uint8_t *begin =
        reinterpret_cast<const std::uint8_t *>(vendor_frame.u64VirAddr[i]);
    frame->channels.emplace_back(begin, begin + vendor_frame.u32Len);
  }
}

inline void fromVendor(const AUDIO_STREAM_S &vendor_stream,
                       AudioPayloadType payload_type,
                       AudioEncodedStream *stream) {
  if (!stream) {
    return;
  }
  stream->payload_type = payload_type;
  stream->timestamp = vendor_stream.u64TimeStamp;
  stream->sequence = vendor_stream.u32Seq;
  stream->data.clear();
  if (vendor_stream.pStream && vendor_stream.u32Len > 0) {
    stream->data.assign(vendor_stream.pStream,
                        vendor_stream.pStream + vendor_stream.u32Len);
  }
}

inline AUDIO_STREAM_S toVendor(const AudioEncodedStream &stream) {
  AUDIO_STREAM_S out;
  std::memset(&out, 0, sizeof(out));
  out.pStream = const_cast<CVI_U8 *>(reinterpret_cast<const CVI_U8 *>(
      stream.data.empty() ? nullptr : stream.data.data()));
  out.u32Len = static_cast<CVI_U32>(stream.data.size());
  out.u64TimeStamp = stream.timestamp;
  out.u32Seq = stream.sequence;
  return out;
}

}  // namespace private_audio
}  // namespace tdl_app
