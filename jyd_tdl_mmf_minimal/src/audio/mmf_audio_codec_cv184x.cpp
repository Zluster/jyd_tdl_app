#include "mmf_cv184x_common.hpp"

using namespace mmf_cv184x;

struct mmf_audio_encoder {
  explicit mmf_audio_encoder(const mmf_audio_codec_config_t& cfg)
      : config(cfg), encoder([&]() {
          mmf_cvi::AudioEncoder::Config out;
          out.channel = static_cast<int>(cfg.channel_id);
          out.payload_type = to_native_audio_payload(cfg.codec);
          out.points_per_frame = 160;
          return out;
        }()) {}
  mmf_audio_codec_config_t config;
  mmf_cvi::AudioEncoder encoder;
  mmf_cvi::AudioEncodedStream stream;
  std::vector<std::uint8_t> packet;
};

struct mmf_audio_decoder {
  explicit mmf_audio_decoder(const mmf_audio_codec_config_t& cfg)
      : config(cfg), decoder([&]() {
          mmf_cvi::AudioDecoder::Config out;
          out.channel = static_cast<int>(cfg.channel_id);
          out.payload_type = to_native_audio_payload(cfg.codec);
          out.sample_rate = static_cast<int>(cfg.sample_rate);
          out.channel_count = static_cast<int>(cfg.channels);
          return out;
        }()) {}
  mmf_audio_codec_config_t config;
  mmf_cvi::AudioDecoder decoder;
  mmf_cvi::AudioFrame frame;
  std::vector<std::uint8_t> interleaved;
};

extern "C" {

mmf_result_t mmf_audio_encoder_open(const mmf_audio_codec_config_t* config,
                                    mmf_audio_encoder_t** encoder) {
  if (config == nullptr || encoder == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_audio_encoder_t> ptr(new mmf_audio_encoder_t(*config));
  std::string error;
  if (!ptr->encoder.open(&error))
    return ok_or_error(false, error);
  *encoder = ptr.release();
  return MMF_OK;
}

void mmf_audio_encoder_close(mmf_audio_encoder_t* encoder) {
  if (encoder == nullptr)
    return;
  encoder->encoder.close();
  delete encoder;
}

mmf_result_t mmf_audio_encoder_encode(mmf_audio_encoder_t* encoder, const mmf_audio_frame_t* pcm,
                                      mmf_packet_t* packet) {
  if (encoder == nullptr || pcm == nullptr || packet == nullptr)
    return MMF_EINVAL;
  mmf_cvi::AudioFrame frame;
  const size_t sample_bytes = bytes_per_sample(pcm->format);
  const auto* data = static_cast<const uint8_t*>(pcm->data);
  const size_t samples = pcm->bytes / (sample_bytes * pcm->channels);
  frame.bit_width = to_native_bit_width(pcm->format);
  frame.sound_mode =
      pcm->channels > 1 ? mmf_cvi::AudioSoundMode::Stereo : mmf_cvi::AudioSoundMode::Mono;
  frame.channels.resize(pcm->channels);
  for (uint32_t ch = 0; ch < pcm->channels; ++ch)
    frame.channels[ch].resize(samples * sample_bytes);
  for (size_t sample = 0; sample < samples; ++sample) {
    for (uint32_t ch = 0; ch < pcm->channels; ++ch) {
      std::memcpy(frame.channels[ch].data() + sample * sample_bytes,
                  data + (sample * pcm->channels + ch) * sample_bytes, sample_bytes);
    }
  }
  std::string error;
  if (!encoder->encoder.encodeFrame(frame, &encoder->stream, &error)) {
    return ok_or_error(false, error);
  }
  packet->data = encoder->stream.data.data();
  packet->bytes = encoder->stream.data.size();
  packet->codec = from_native_audio_payload(encoder->stream.payload_type);
  packet->sequence = encoder->stream.sequence;
  packet->timestamp_us = encoder->stream.timestamp;
  return MMF_OK;
}

mmf_result_t mmf_audio_encoder_release(mmf_audio_encoder_t* encoder, mmf_packet_t* packet) {
  (void)encoder;
  if (packet != nullptr) {
    packet->data = nullptr;
    packet->bytes = 0;
  }
  return MMF_OK;
}

mmf_result_t mmf_audio_decoder_open(const mmf_audio_codec_config_t* config,
                                    mmf_audio_decoder_t** decoder) {
  if (config == nullptr || decoder == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_audio_decoder_t> ptr(new mmf_audio_decoder_t(*config));
  std::string error;
  if (!ptr->decoder.open(&error))
    return ok_or_error(false, error);
  *decoder = ptr.release();
  return MMF_OK;
}

void mmf_audio_decoder_close(mmf_audio_decoder_t* decoder) {
  if (decoder == nullptr)
    return;
  decoder->decoder.close();
  delete decoder;
}

mmf_result_t mmf_audio_decoder_decode(mmf_audio_decoder_t* decoder, const mmf_packet_t* packet,
                                      mmf_audio_frame_t* pcm) {
  if (decoder == nullptr || packet == nullptr || pcm == nullptr)
    return MMF_EINVAL;
  mmf_cvi::AudioEncodedStream stream;
  stream.payload_type = to_native_audio_payload(packet->codec);
  stream.sequence = static_cast<uint32_t>(packet->sequence);
  stream.timestamp = packet->timestamp_us;
  const auto* data = static_cast<const uint8_t*>(packet->data);
  stream.data.assign(data, data + packet->bytes);
  std::string error;
  if (!decoder->decoder.decodeStream(stream, &decoder->frame, true, &error)) {
    return ok_or_error(false, error);
  }
  if (decoder->frame.channels.empty())
    return MMF_EIO;
  const mmf_audio_format_t fmt = from_native_bit_width(decoder->frame.bit_width);
  const size_t sample_bytes = bytes_per_sample(fmt);
  const size_t channel_count = decoder->frame.channels.size();
  const size_t samples = decoder->frame.channels[0].size() / sample_bytes;
  decoder->interleaved.assign(samples * channel_count * sample_bytes, 0);
  for (size_t sample = 0; sample < samples; ++sample) {
    for (size_t ch = 0; ch < channel_count; ++ch) {
      std::memcpy(decoder->interleaved.data() + (sample * channel_count + ch) * sample_bytes,
                  decoder->frame.channels[ch].data() + sample * sample_bytes, sample_bytes);
    }
  }
  pcm->sample_rate = decoder->config.sample_rate;
  pcm->channels = static_cast<uint32_t>(channel_count);
  pcm->samples_per_frame = static_cast<uint32_t>(samples);
  pcm->format = fmt;
  pcm->data = decoder->interleaved.data();
  pcm->bytes = decoder->interleaved.size();
  return MMF_OK;
}

mmf_result_t mmf_audio_decoder_release(mmf_audio_decoder_t* decoder, mmf_audio_frame_t* pcm) {
  (void)decoder;
  if (pcm != nullptr) {
    pcm->data = nullptr;
    pcm->bytes = 0;
  }
  return MMF_OK;
}

}  // extern "C"
