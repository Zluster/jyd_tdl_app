#include "media/private/audio_file_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace tdl_app {
namespace private_audio {
namespace {

struct DecoderState {
  AVFormatContext *format = nullptr;
  AVCodecContext *codec = nullptr;
  SwrContext *resampler = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;

  ~DecoderState() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
};

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string ffError(const char *operation, int code) {
  char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, detail, sizeof(detail));
  return std::string(operation) + " failed: " + detail;
}

void mixStereoToDualMono(std::vector<std::uint8_t> *pcm,
                         std::size_t sample_count) {
  for (std::size_t i = 0; i < sample_count; ++i) {
    std::int16_t left = 0;
    std::int16_t right = 0;
    std::memcpy(&left, pcm->data() + (i * 2) * sizeof(left), sizeof(left));
    std::memcpy(&right, pcm->data() + (i * 2 + 1) * sizeof(right),
                sizeof(right));
    const std::int16_t mixed = static_cast<std::int16_t>(
        (static_cast<std::int32_t>(left) +
         static_cast<std::int32_t>(right)) /
        2);
    std::memcpy(pcm->data() + (i * 2) * sizeof(mixed), &mixed,
                sizeof(mixed));
    std::memcpy(pcm->data() + (i * 2 + 1) * sizeof(mixed), &mixed,
                sizeof(mixed));
  }
}

class FrameEmitter {
 public:
  explicit FrameEmitter(const Pcm16StereoSink &sink) : sink_(sink) {}

  bool append(const std::uint8_t *data, std::size_t size,
              std::string *error) {
    pending_.insert(pending_.end(), data, data + size);
    while (pending_.size() - offset_ >= frameBytes()) {
      std::vector<std::uint8_t> frame(
          pending_.begin() + static_cast<std::ptrdiff_t>(offset_),
          pending_.begin() +
              static_cast<std::ptrdiff_t>(offset_ + frameBytes()));
      if (!sink_(frame, error)) {
        return false;
      }
      offset_ += frameBytes();
    }
    compact();
    return true;
  }

  bool finish(std::string *error) {
    if (pending_.size() == offset_) {
      return true;
    }
    std::vector<std::uint8_t> frame(frameBytes(), 0);
    std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(offset_),
              pending_.end(), frame.begin());
    offset_ = pending_.size();
    return sink_(frame, error);
  }

 private:
  static std::size_t frameBytes() {
    return static_cast<std::size_t>(kPlaybackSamplesPerFrame) *
           kPlaybackChannels * (kPlaybackBitDepth / 8);
  }

  void compact() {
    if (offset_ == pending_.size()) {
      pending_.clear();
      offset_ = 0;
    } else if (offset_ >= frameBytes() * 16) {
      pending_.erase(
          pending_.begin(),
          pending_.begin() + static_cast<std::ptrdiff_t>(offset_));
      offset_ = 0;
    }
  }

  const Pcm16StereoSink &sink_;
  std::vector<std::uint8_t> pending_;
  std::size_t offset_ = 0;
};

bool convertFrame(DecoderState *state, const AVFrame *frame,
                  FrameEmitter *emitter, std::string *error) {
  const int output_capacity = static_cast<int>(av_rescale_rnd(
      swr_get_delay(state->resampler, state->codec->sample_rate) +
          frame->nb_samples,
      kPlaybackSampleRate, state->codec->sample_rate, AV_ROUND_UP));
  std::vector<std::uint8_t> converted(
      static_cast<std::size_t>(output_capacity) * kPlaybackChannels *
      sizeof(std::int16_t));
  std::uint8_t *output[] = {converted.data()};
  const int output_samples = swr_convert(
      state->resampler, output, output_capacity,
      const_cast<const std::uint8_t **>(frame->extended_data),
      frame->nb_samples);
  if (output_samples < 0) {
    setError(error, ffError("swr_convert", output_samples));
    return false;
  }
  mixStereoToDualMono(&converted, static_cast<std::size_t>(output_samples));
  return emitter->append(
      converted.data(),
      static_cast<std::size_t>(output_samples) * kPlaybackChannels *
          sizeof(std::int16_t),
      error);
}

bool receiveFrames(DecoderState *state, FrameEmitter *emitter,
                   std::string *error) {
  while (true) {
    const int ret = avcodec_receive_frame(state->codec, state->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      setError(error, ffError("avcodec_receive_frame", ret));
      return false;
    }
    if (!convertFrame(state, state->frame, emitter, error)) {
      av_frame_unref(state->frame);
      return false;
    }
    av_frame_unref(state->frame);
  }
}

}  // namespace

bool decodeAudioFileToPcm16Stereo(const std::string &path,
                                  const Pcm16StereoSink &sink,
                                  std::string *error) {
  DecoderState state;
  int ret = avformat_open_input(&state.format, path.c_str(), nullptr, nullptr);
  if (ret < 0) {
    setError(error, ffError("avformat_open_input", ret));
    return false;
  }
  ret = avformat_find_stream_info(state.format, nullptr);
  if (ret < 0) {
    setError(error, ffError("avformat_find_stream_info", ret));
    return false;
  }

  const AVCodec *decoder = nullptr;
  const int stream_index = av_find_best_stream(
      state.format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (stream_index < 0 || !decoder) {
    setError(error, ffError("av_find_best_stream", stream_index));
    return false;
  }
  state.codec = avcodec_alloc_context3(decoder);
  if (!state.codec) {
    setError(error, "avcodec_alloc_context3 failed");
    return false;
  }
  ret = avcodec_parameters_to_context(
      state.codec, state.format->streams[stream_index]->codecpar);
  if (ret < 0) {
    setError(error, ffError("avcodec_parameters_to_context", ret));
    return false;
  }
  ret = avcodec_open2(state.codec, decoder, nullptr);
  if (ret < 0) {
    setError(error, ffError("avcodec_open2", ret));
    return false;
  }

  std::int64_t input_layout = state.codec->channel_layout;
  if (input_layout == 0) {
    input_layout = av_get_default_channel_layout(state.codec->channels);
  }
  state.resampler = swr_alloc_set_opts(
      nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
      kPlaybackSampleRate, input_layout, state.codec->sample_fmt,
      state.codec->sample_rate, 0, nullptr);
  if (!state.resampler) {
    setError(error, "swr_alloc_set_opts failed");
    return false;
  }
  ret = swr_init(state.resampler);
  if (ret < 0) {
    setError(error, ffError("swr_init", ret));
    return false;
  }

  state.packet = av_packet_alloc();
  state.frame = av_frame_alloc();
  if (!state.packet || !state.frame) {
    setError(error, "failed to allocate FFmpeg packet/frame");
    return false;
  }

  FrameEmitter emitter(sink);
  while ((ret = av_read_frame(state.format, state.packet)) >= 0) {
    if (state.packet->stream_index == stream_index) {
      ret = avcodec_send_packet(state.codec, state.packet);
      if (ret < 0) {
        av_packet_unref(state.packet);
        setError(error, ffError("avcodec_send_packet", ret));
        return false;
      }
      if (!receiveFrames(&state, &emitter, error)) {
        av_packet_unref(state.packet);
        return false;
      }
    }
    av_packet_unref(state.packet);
  }
  if (ret != AVERROR_EOF) {
    setError(error, ffError("av_read_frame", ret));
    return false;
  }
  ret = avcodec_send_packet(state.codec, nullptr);
  if (ret < 0) {
    setError(error, ffError("avcodec_send_packet(flush)", ret));
    return false;
  }
  if (!receiveFrames(&state, &emitter, error)) {
    return false;
  }

  while (swr_get_delay(state.resampler, state.codec->sample_rate) > 0) {
    const int output_capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(state.resampler, state.codec->sample_rate),
        kPlaybackSampleRate, state.codec->sample_rate, AV_ROUND_UP));
    std::vector<std::uint8_t> converted(
        static_cast<std::size_t>(output_capacity) * kPlaybackChannels *
        sizeof(std::int16_t));
    std::uint8_t *output[] = {converted.data()};
    const int output_samples = swr_convert(
        state.resampler, output, output_capacity, nullptr, 0);
    if (output_samples < 0) {
      setError(error, ffError("swr_convert(flush)", output_samples));
      return false;
    }
    if (output_samples == 0) {
      break;
    }
    mixStereoToDualMono(&converted,
                        static_cast<std::size_t>(output_samples));
    if (!emitter.append(
            converted.data(),
            static_cast<std::size_t>(output_samples) * kPlaybackChannels *
                sizeof(std::int16_t),
            error)) {
      return false;
    }
  }
  return emitter.finish(error);
}

}  // namespace private_audio
}  // namespace tdl_app
