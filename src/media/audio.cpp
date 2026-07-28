#include "tdl_app/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::size_t bytesPerSample(int bit_depth) {
  switch (bit_depth) {
    case 8: return 1;
    case 16: return 2;
    case 24: return 3;
    case 32: return 4;
    default: return 0;
  }
}

AudioSampleRate toSampleRate(int sample_rate, std::string *error) {
  switch (sample_rate) {
    case 8000: return AudioSampleRate::Hz8000;
    case 11025: return AudioSampleRate::Hz11025;
    case 16000: return AudioSampleRate::Hz16000;
    case 22050: return AudioSampleRate::Hz22050;
    case 24000: return AudioSampleRate::Hz24000;
    case 32000: return AudioSampleRate::Hz32000;
    case 44100: return AudioSampleRate::Hz44100;
    case 48000: return AudioSampleRate::Hz48000;
    case 64000: return AudioSampleRate::Hz64000;
    default:
      setError(error, "unsupported sample rate: " + std::to_string(sample_rate));
      return AudioSampleRate::Hz16000;
  }
}

AudioBitWidth toBitWidth(int bit_depth, std::string *error) {
  switch (bit_depth) {
    case 8: return AudioBitWidth::Bits8;
    case 16: return AudioBitWidth::Bits16;
    case 24: return AudioBitWidth::Bits24;
    case 32: return AudioBitWidth::Bits32;
    default:
      setError(error, "unsupported bit depth: " + std::to_string(bit_depth));
      return AudioBitWidth::Bits16;
  }
}

AudioSoundMode toSoundMode(int channels, std::string *error) {
  if (channels == 1) {
    return AudioSoundMode::Mono;
  }
  if (channels == 2) {
    return AudioSoundMode::Stereo;
  }
  setError(error, "channels must be 1 or 2");
  return AudioSoundMode::Mono;
}

bool validateIoConfig(const AudioIoConfig &config, std::string *error) {
  if (config.channels != 1 && config.channels != 2) {
    setError(error, "channels must be 1 or 2");
    return false;
  }
  if (bytesPerSample(config.bit_depth) == 0) {
    setError(error, "bit depth must be 8/16/24/32");
    return false;
  }
  std::string sample_rate_error;
  (void)toSampleRate(config.sample_rate, &sample_rate_error);
  if (!sample_rate_error.empty()) {
    setError(error, sample_rate_error);
    return false;
  }
  if (config.points_per_frame <= 0 || config.frame_count <= 0 || config.frame_depth <= 0) {
    setError(error, "frame parameters must be > 0");
    return false;
  }
  if (config.timeout_ms < 0) {
    setError(error, "timeout_ms must be >= 0");
    return false;
  }
  return true;
}

AudioInput::Config toInputConfig(const AudioIoConfig &config, std::string *error) {
  AudioInput::Config out;
  out.device = config.ai_device;
  out.channel = config.ai_channel;
  out.card_id = config.ai_card_id;
  out.sample_rate = toSampleRate(config.sample_rate, error);
  out.bit_width = toBitWidth(config.bit_depth, error);
  out.sound_mode = toSoundMode(config.channels, error);
  out.points_per_frame = config.points_per_frame;
  out.frame_count = config.frame_count;
  out.frame_depth = config.frame_depth;
  out.channel_count = config.channels;
  out.volume_step = config.ai_volume;
  return out;
}

AudioOutput::Config toOutputConfig(const AudioIoConfig &config, std::string *error) {
  AudioOutput::Config out;
  out.device = config.ao_device;
  out.channel = config.ao_channel;
  out.card_id = config.ao_card_id;
  out.sample_rate = toSampleRate(config.sample_rate, error);
  out.bit_width = toBitWidth(config.bit_depth, error);
  out.sound_mode = toSoundMode(config.channels, error);
  out.points_per_frame = config.points_per_frame;
  out.frame_count = config.frame_count;
  out.channel_count = config.channels;
  out.volume_db = config.ao_volume;
  return out;
}

AudioEncoder::Config toEncoderConfig(const AudioSessionConfig &config) {
  AudioEncoder::Config out = config.encoder;
  out.points_per_frame = config.io.points_per_frame;
  return out;
}

AudioDecoder::Config toDecoderConfig(const AudioSessionConfig &config) {
  AudioDecoder::Config out = config.decoder;
  out.sample_rate = config.io.sample_rate;
  out.channel_count = config.io.channels;
  out.bytes_per_sample = static_cast<int>(bytesPerSample(config.io.bit_depth));
  out.frame_size = config.io.points_per_frame;
  return out;
}

std::uint64_t targetSampleCount(double seconds, const AudioIoConfig &config) {
  return seconds <= 0.0 ? 0 : static_cast<std::uint64_t>(seconds * config.sample_rate + 0.5);
}

bool audioFrameToChunk(const AudioFrame &frame, std::uint32_t sample_limit,
                       AudioPcmChunk *chunk, std::string *error) {
  if (!chunk) {
    setError(error, "audio chunk pointer is null");
    return false;
  }
  if (frame.channels.empty()) {
    setError(error, "audio frame is empty");
    return false;
  }
  const std::size_t sample_bytes = bytesPerSample(static_cast<int>(frame.bit_width));
  if (sample_bytes == 0) {
    setError(error, "audio frame bit depth is invalid");
    return false;
  }
  const std::size_t channel_count = frame.channels.size();
  const std::size_t available_samples = frame.channels.front().size() / sample_bytes;
  const std::size_t sample_count = sample_limit > 0
      ? std::min<std::size_t>(available_samples, sample_limit)
      : available_samples;
  for (std::size_t ch = 1; ch < channel_count; ++ch) {
    if (frame.channels[ch].size() < sample_count * sample_bytes) {
      setError(error, "audio frame channels are inconsistent");
      return false;
    }
  }
  chunk->sample_rate = 16000;
  chunk->channels = static_cast<int>(channel_count);
  chunk->bit_depth = static_cast<int>(frame.bit_width);
  chunk->timestamp = frame.timestamp;
  chunk->sequence = frame.sequence;
  chunk->data.assign(sample_count * channel_count * sample_bytes, 0);
  if (channel_count == 1) {
    std::copy_n(frame.channels[0].data(), chunk->data.size(), chunk->data.data());
    return true;
  }
  for (std::size_t sample = 0; sample < sample_count; ++sample) {
    for (std::size_t ch = 0; ch < channel_count; ++ch) {
      const std::size_t src_offset = sample * sample_bytes;
      const std::size_t dst_offset = (sample * channel_count + ch) * sample_bytes;
      std::copy_n(frame.channels[ch].data() + src_offset, sample_bytes,
                  chunk->data.data() + dst_offset);
    }
  }
  return true;
}

bool chunkToAudioFrame(const AudioPcmChunk &chunk, const AudioIoConfig &config,
                       AudioFrame *frame, std::string *error) {
  if (!frame) {
    setError(error, "audio frame pointer is null");
    return false;
  }
  const int channels = chunk.channels > 0 ? chunk.channels : config.channels;
  const int bit_depth = chunk.bit_depth > 0 ? chunk.bit_depth : config.bit_depth;
  if (channels != 1 && channels != 2) {
    setError(error, "channels must be 1 or 2");
    return false;
  }
  const std::size_t sample_bytes = bytesPerSample(bit_depth);
  if (sample_bytes == 0) {
    setError(error, "bit depth must be 8/16/24/32");
    return false;
  }
  const std::size_t frame_align = sample_bytes * static_cast<std::size_t>(channels);
  if (chunk.data.size() % frame_align != 0) {
    setError(error, "pcm chunk size is not aligned to sample size");
    return false;
  }
  std::string bit_width_error;
  std::string sound_mode_error;
  frame->bit_width = toBitWidth(bit_depth, &bit_width_error);
  frame->sound_mode = toSoundMode(channels, &sound_mode_error);
  if (!bit_width_error.empty()) {
    setError(error, bit_width_error);
    return false;
  }
  if (!sound_mode_error.empty()) {
    setError(error, sound_mode_error);
    return false;
  }
  frame->timestamp = chunk.timestamp;
  frame->sequence = chunk.sequence;
  frame->bytes_per_channel = static_cast<std::uint32_t>(chunk.data.size() / static_cast<std::size_t>(channels));
  frame->channels.clear();
  if (channels == 1) {
    frame->channels.push_back(chunk.data);
    return true;
  }
  // CVI_AO_SendFrame expects interleaved stereo in u64VirAddr[0].
  frame->channels.push_back(chunk.data);
  return true;
}

void writeLe16(std::ofstream *out, std::uint16_t value) {
  out->put(static_cast<char>(value & 0xff));
  out->put(static_cast<char>((value >> 8) & 0xff));
}

void writeLe32(std::ofstream *out, std::uint32_t value) {
  out->put(static_cast<char>(value & 0xff));
  out->put(static_cast<char>((value >> 8) & 0xff));
  out->put(static_cast<char>((value >> 16) & 0xff));
  out->put(static_cast<char>((value >> 24) & 0xff));
}

std::uint16_t readLe16(std::ifstream *in) {
  const std::uint8_t b0 = static_cast<std::uint8_t>(in->get());
  const std::uint8_t b1 = static_cast<std::uint8_t>(in->get());
  return static_cast<std::uint16_t>(b0 | (b1 << 8));
}

std::uint32_t readLe32(std::ifstream *in) {
  const std::uint8_t b0 = static_cast<std::uint8_t>(in->get());
  const std::uint8_t b1 = static_cast<std::uint8_t>(in->get());
  const std::uint8_t b2 = static_cast<std::uint8_t>(in->get());
  const std::uint8_t b3 = static_cast<std::uint8_t>(in->get());
  return static_cast<std::uint32_t>(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}

bool writeWavHeader(std::ofstream *out, const AudioIoConfig &config,
                    std::uint32_t data_size, std::string *error) {
  if (!out || !(*out)) {
    setError(error, "wav output file is not open");
    return false;
  }
  const std::uint16_t block_align = static_cast<std::uint16_t>(config.channels * bytesPerSample(config.bit_depth));
  const std::uint32_t byte_rate = static_cast<std::uint32_t>(config.sample_rate * block_align);
  out->write("RIFF", 4);
  writeLe32(out, 36 + data_size);
  out->write("WAVE", 4);
  out->write("fmt ", 4);
  writeLe32(out, 16);
  writeLe16(out, 1);
  writeLe16(out, static_cast<std::uint16_t>(config.channels));
  writeLe32(out, static_cast<std::uint32_t>(config.sample_rate));
  writeLe32(out, byte_rate);
  writeLe16(out, block_align);
  writeLe16(out, static_cast<std::uint16_t>(config.bit_depth));
  out->write("data", 4);
  writeLe32(out, data_size);
  if (!(*out)) {
    setError(error, "failed to write wav header");
    return false;
  }
  return true;
}

bool readWavHeader(std::ifstream *in, AudioIoConfig *config,
                   std::uint32_t *data_size, std::string *error) {
  if (!in || !(*in)) {
    setError(error, "wav input file is not open");
    return false;
  }
  char riff[4] = {0, 0, 0, 0};
  char wave[4] = {0, 0, 0, 0};
  in->read(riff, 4);
  if (!(*in) || std::strncmp(riff, "RIFF", 4) != 0) {
    setError(error, "invalid wav header: missing RIFF");
    return false;
  }
  (void)readLe32(in);
  in->read(wave, 4);
  if (!(*in) || std::strncmp(wave, "WAVE", 4) != 0) {
    setError(error, "invalid wav header: missing WAVE");
    return false;
  }
  bool fmt_found = false;
  bool data_found = false;
  std::uint16_t audio_format = 1;
  std::uint16_t channels = 1;
  std::uint32_t sample_rate = 16000;
  std::uint16_t bits_per_sample = 16;
  std::uint32_t payload_size = 0;
  while (*in && (!fmt_found || !data_found)) {
    char chunk_id[4] = {0, 0, 0, 0};
    in->read(chunk_id, 4);
    if (!(*in)) {
      break;
    }
    const std::uint32_t chunk_size = readLe32(in);
    if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
      audio_format = readLe16(in);
      channels = readLe16(in);
      sample_rate = readLe32(in);
      (void)readLe32(in);
      (void)readLe16(in);
      bits_per_sample = readLe16(in);
      if (chunk_size > 16) {
        in->seekg(chunk_size - 16, std::ios::cur);
      }
      fmt_found = true;
    } else if (std::strncmp(chunk_id, "data", 4) == 0) {
      payload_size = chunk_size;
      data_found = true;
      break;
    } else {
      in->seekg(chunk_size, std::ios::cur);
    }
  }
  if (!fmt_found || !data_found) {
    setError(error, "wav header is incomplete");
    return false;
  }
  if (audio_format != 1) {
    setError(error, "only PCM wav is supported");
    return false;
  }
  config->sample_rate = static_cast<int>(sample_rate);
  config->channels = static_cast<int>(channels);
  config->bit_depth = static_cast<int>(bits_per_sample);
  if (data_size) {
    *data_size = payload_size;
  }
  return validateIoConfig(*config, error);
}

bool drainOutput(AudioOutput *output, int wait_ms) {
  if (!output) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    AudioOutput::ChannelState state;
    if (output->queryState(&state, nullptr) && state.busy == 0) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

struct InputStreamState {
  bool opened = false;
  bool talk_vqe_enabled = false;
  std::uint32_t sequence = 0;
  AudioInputStreamConfig config;
  std::unique_ptr<AudioInput> input;
  void close() {
    if (input) {
      input->close();
      input.reset();
    }
    opened = false;
    talk_vqe_enabled = false;
    sequence = 0;
  }
};

struct OutputStreamState {
  bool opened = false;
  std::uint32_t sequence = 0;
  AudioIoConfig config;
  std::unique_ptr<AudioOutput> output;
  void close() {
    if (output) {
      drainOutput(output.get(), 300);
      output->close();
      output.reset();
    }
    opened = false;
    sequence = 0;
  }
};

struct SessionState {
  bool opened = false;
  bool input_opened = false;
  bool output_opened = false;
  bool encoder_opened = false;
  bool decoder_opened = false;
  bool talk_vqe_enabled = false;
  std::uint32_t input_sequence = 0;
  std::uint32_t output_sequence = 0;
  std::uint32_t encoded_sequence = 0;
  AudioSessionConfig config;
  std::unique_ptr<AudioInput> input;
  std::unique_ptr<AudioOutput> output;
  std::unique_ptr<AudioEncoder> encoder;
  std::unique_ptr<AudioDecoder> decoder;
  void close() {
    if (decoder) {
      decoder->close();
      decoder.reset();
    }
    if (encoder) {
      encoder->close();
      encoder.reset();
    }
    if (output) {
      drainOutput(output.get(), 300);
      output->close();
      output.reset();
    }
    if (input) {
      input->close();
      input.reset();
    }
    opened = false;
    input_opened = false;
    output_opened = false;
    encoder_opened = false;
    decoder_opened = false;
    talk_vqe_enabled = false;
    input_sequence = 0;
    output_sequence = 0;
    encoded_sequence = 0;
  }
};

InputStreamState &inputStreamState() { static InputStreamState state; return state; }
OutputStreamState &outputStreamState() { static OutputStreamState state; return state; }
SessionState &sessionState() { static SessionState state; return state; }
std::mutex &controlMutex() { static std::mutex mutex; return mutex; }
std::mutex &inputMutex() { static std::mutex mutex; return mutex; }
std::mutex &outputMutex() { static std::mutex mutex; return mutex; }
std::mutex &sessionMutex() { static std::mutex mutex; return mutex; }

bool openInputState(InputStreamState *state, const AudioInputStreamConfig &config,
                    std::string *error) {
  if (!state) {
    setError(error, "audio input state is null");
    return false;
  }
  if (!validateIoConfig(config.io, error)) {
    return false;
  }
  std::string build_error;
  state->close();
  state->config = config;
  state->input.reset(new AudioInput(toInputConfig(config.io, &build_error)));
  if (!build_error.empty()) {
    setError(error, build_error);
    state->close();
    return false;
  }
  if (!state->input->open(error)) {
    state->close();
    return false;
  }
  if (config.enable_talk_vqe) {
    if (!state->input->configureTalkVqe(config.talk_vqe,
                                        config.reference_output_device,
                                        config.reference_output_channel,
                                        error) ||
        !state->input->enableVqe(error)) {
      state->close();
      return false;
    }
    state->talk_vqe_enabled = true;
  }
  state->opened = true;
  return true;
}

bool openOutputState(OutputStreamState *state, const AudioIoConfig &config,
                     std::string *error) {
  if (!state) {
    setError(error, "audio output state is null");
    return false;
  }
  if (!validateIoConfig(config, error)) {
    return false;
  }
  std::string build_error;
  state->close();
  state->config = config;
  state->output.reset(new AudioOutput(toOutputConfig(config, &build_error)));
  if (!build_error.empty()) {
    setError(error, build_error);
    state->close();
    return false;
  }
  if (!state->output->open(error)) {
    state->close();
    return false;
  }
  state->opened = true;
  return true;
}

bool openSessionState(SessionState *state, const AudioSessionConfig &config,
                      std::string *error) {
  if (!state) {
    setError(error, "audio session state is null");
    return false;
  }
  if (!validateIoConfig(config.io, error)) {
    return false;
  }
  if (config.enable_encoder && !config.enable_input) {
    setError(error, "audio encoder requires input to be enabled");
    return false;
  }
  if (config.enable_decoder && !config.enable_output) {
    setError(error, "audio decoder requires output to be enabled");
    return false;
  }
  if (config.enable_talk_vqe && (!config.enable_input || !config.enable_output)) {
    setError(error, "talk vqe requires both input and output to be enabled");
    return false;
  }
  state->close();
  state->config = config;
  if (config.enable_output) {
    std::string build_error;
    state->output.reset(new AudioOutput(toOutputConfig(config.io, &build_error)));
    if (!build_error.empty()) {
      setError(error, build_error);
      state->close();
      return false;
    }
    if (!state->output->open(error)) {
      state->close();
      return false;
    }
    state->output_opened = true;
  }
  if (config.enable_input) {
    std::string build_error;
    state->input.reset(new AudioInput(toInputConfig(config.io, &build_error)));
    if (!build_error.empty()) {
      setError(error, build_error);
      state->close();
      return false;
    }
    if (!state->input->open(error)) {
      state->close();
      return false;
    }
    state->input_opened = true;
    if (config.enable_talk_vqe) {
      if (!state->input->configureTalkVqe(config.talk_vqe,
                                          config.reference_output_device,
                                          config.reference_output_channel,
                                          error) ||
          !state->input->enableVqe(error)) {
        state->close();
        return false;
      }
      state->talk_vqe_enabled = true;
    }
  }
  if (config.enable_encoder) {
    state->encoder.reset(new AudioEncoder(toEncoderConfig(config)));
    if (!state->encoder->open(error)) {
      state->close();
      return false;
    }
    state->encoder_opened = true;
  }
  if (config.enable_decoder) {
    state->decoder.reset(new AudioDecoder(toDecoderConfig(config)));
    if (!state->decoder->open(error)) {
      state->close();
      return false;
    }
    state->decoder_opened = true;
  }
  state->opened = true;
  return true;
}

}  // namespace

AudioStatus Audio::status() const {
  AudioStatus status;
  std::lock_guard<std::mutex> lock(controlMutex());
  status.input_stream_open = inputStreamState().opened;
  status.output_stream_open = outputStreamState().opened;
  status.session_open = sessionState().opened;
  if (sessionState().opened) {
    status.ai_device = sessionState().config.io.ai_device;
    status.ai_channel = sessionState().config.io.ai_channel;
    status.ao_device = sessionState().config.io.ao_device;
    status.ao_channel = sessionState().config.io.ao_channel;
    status.sample_rate = sessionState().config.io.sample_rate;
    status.channels = sessionState().config.io.channels;
    status.bit_depth = sessionState().config.io.bit_depth;
  } else if (inputStreamState().opened) {
    status.ai_device = inputStreamState().config.io.ai_device;
    status.ai_channel = inputStreamState().config.io.ai_channel;
    status.sample_rate = inputStreamState().config.io.sample_rate;
    status.channels = inputStreamState().config.io.channels;
    status.bit_depth = inputStreamState().config.io.bit_depth;
  } else if (outputStreamState().opened) {
    status.ao_device = outputStreamState().config.ao_device;
    status.ao_channel = outputStreamState().config.ao_channel;
    status.sample_rate = outputStreamState().config.sample_rate;
    status.channels = outputStreamState().config.channels;
    status.bit_depth = outputStreamState().config.bit_depth;
  }
  status.note = "high-level audio wrapper uses CVI AI/AO/AENC/ADEC for wav, pcm stream, talk VQE, and codec bridging";
  return status;
}

bool Audio::recordWav(const std::string &path, double seconds,
                      const AudioIoConfig &config,
                      std::string *error) const {
  if (!validateIoConfig(config, error)) {
    return false;
  }
  if (seconds <= 0.0) {
    setError(error, "record seconds must be > 0");
    return false;
  }
  std::string build_error;
  AudioInput input(toInputConfig(config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!input.open(error)) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    setError(error, "failed to open wav output: " + path);
    return false;
  }
  if (!writeWavHeader(&out, config, 0, error)) {
    return false;
  }
  std::uint32_t data_size = 0;
  std::uint64_t captured_samples = 0;
  const std::uint64_t target_samples = targetSampleCount(seconds, config);
  while (captured_samples < target_samples) {
    AudioFrame frame;
    if (!input.readFrame(&frame, config.timeout_ms, error)) {
      return false;
    }
    const std::size_t frame_samples = frame.channels.empty()
        ? 0
        : frame.channels.front().size() / bytesPerSample(static_cast<int>(frame.bit_width));
    const std::uint32_t sample_limit = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(target_samples - captured_samples, frame_samples));
    AudioPcmChunk chunk;
    if (!audioFrameToChunk(frame, sample_limit, &chunk, error)) {
      return false;
    }
    chunk.sample_rate = config.sample_rate;
    chunk.channels = config.channels;
    chunk.bit_depth = config.bit_depth;
    out.write(reinterpret_cast<const char *>(chunk.data.data()),
              static_cast<std::streamsize>(chunk.data.size()));
    if (!out.good()) {
      setError(error, "failed to write wav payload: " + path);
      return false;
    }
    data_size += static_cast<std::uint32_t>(chunk.data.size());
    captured_samples += sample_limit;
  }
  out.seekp(0, std::ios::beg);
  return writeWavHeader(&out, config, data_size, error);
}

bool Audio::playWav(const std::string &path, const AudioIoConfig &config,
                    std::string *error) const {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    setError(error, "failed to open wav input: " + path);
    return false;
  }
  AudioIoConfig wav_config = config;
  std::uint32_t data_size = 0;
  if (!readWavHeader(&in, &wav_config, &data_size, error)) {
    return false;
  }
  wav_config.ao_device = config.ao_device;
  wav_config.ao_channel = config.ao_channel;
  wav_config.ao_card_id = config.ao_card_id;
  wav_config.ao_volume = config.ao_volume;
  wav_config.frame_count = config.frame_count;
  wav_config.frame_depth = config.frame_depth;
  wav_config.timeout_ms = config.timeout_ms;
  wav_config.points_per_frame = config.points_per_frame > 0
      ? config.points_per_frame
      : std::max(80, wav_config.sample_rate / 100);
  const int source_channels = wav_config.channels;
  const bool duplicate_mono = source_channels == 1;
  if (duplicate_mono) {
    wav_config.channels = 2;
  }
  std::string build_error;
  AudioOutput output(toOutputConfig(wav_config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!output.open(error)) {
    return false;
  }
  const std::size_t sample_bytes = bytesPerSample(wav_config.bit_depth);
  const std::size_t source_frame_bytes =
      static_cast<std::size_t>(wav_config.points_per_frame) *
      source_channels * sample_bytes;
  const std::size_t output_frame_bytes =
      static_cast<std::size_t>(wav_config.points_per_frame) *
      wav_config.channels * sample_bytes;
  std::vector<std::uint8_t> input_buffer(source_frame_bytes, 0);
  std::vector<std::uint8_t> output_buffer(output_frame_bytes, 0);
  std::uint32_t sequence = 0;
  std::uint32_t remaining = data_size;
  while (remaining > 0) {
    const std::size_t want = std::min<std::size_t>(input_buffer.size(), remaining);
    std::fill(input_buffer.begin(), input_buffer.end(), 0);
    in.read(reinterpret_cast<char *>(input_buffer.data()), static_cast<std::streamsize>(want));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
      break;
    }
    std::fill(output_buffer.begin(), output_buffer.end(), 0);
    if (duplicate_mono) {
      const std::size_t samples = static_cast<std::size_t>(got) / sample_bytes;
      for (std::size_t i = 0; i < samples; ++i) {
        const std::uint8_t *sample = input_buffer.data() + i * sample_bytes;
        std::copy_n(sample, sample_bytes,
                    output_buffer.data() + (i * 2) * sample_bytes);
        std::copy_n(sample, sample_bytes,
                    output_buffer.data() + (i * 2 + 1) * sample_bytes);
      }
    } else {
      std::copy_n(input_buffer.data(), static_cast<std::size_t>(got),
                  output_buffer.data());
    }
    AudioPcmChunk chunk;
    chunk.sample_rate = wav_config.sample_rate;
    chunk.channels = wav_config.channels;
    chunk.bit_depth = wav_config.bit_depth;
    chunk.sequence = sequence++;
    chunk.data.assign(output_buffer.begin(), output_buffer.end());
    AudioFrame frame;
    if (!chunkToAudioFrame(chunk, wav_config, &frame, error)) {
      return false;
    }
    if (!output.writeFrame(frame, wav_config.timeout_ms, error)) {
      return false;
    }
    remaining -= static_cast<std::uint32_t>(got);
  }
  drainOutput(&output, 800);
  return true;
}

bool Audio::loopback(double seconds, const AudioSessionConfig &config,
                     std::string *error) const {
  if (!validateIoConfig(config.io, error)) {
    return false;
  }
  if (seconds <= 0.0) {
    setError(error, "loopback seconds must be > 0");
    return false;
  }
  AudioSessionConfig local = config;
  local.enable_input = true;
  local.enable_output = true;
  local.enable_encoder = false;
  local.enable_decoder = false;
  SessionState state;
  if (!openSessionState(&state, local, error)) {
    return false;
  }
  const std::uint64_t target_samples = targetSampleCount(seconds, local.io);
  std::uint64_t looped_samples = 0;
  while (looped_samples < target_samples) {
    AudioFrame frame;
    if (!state.input->readFrame(&frame, local.io.timeout_ms, error)) {
      state.close();
      return false;
    }
    frame.sequence = state.output_sequence++;
    if (!state.output->writeFrame(frame, local.io.timeout_ms, error)) {
      state.close();
      return false;
    }
    if (!frame.channels.empty()) {
      looped_samples += frame.channels.front().size() /
                        bytesPerSample(static_cast<int>(frame.bit_width));
    }
  }
  state.close();
  return true;
}

bool Audio::getInputVolume(int *volume, const AudioIoConfig &config,
                           std::string *error) const {
  if (!volume) {
    setError(error, "input volume pointer is null");
    return false;
  }
  {
    std::lock_guard<std::mutex> session_lock(sessionMutex());
    if (sessionState().opened && sessionState().input) {
      return sessionState().input->getVolume(volume, error);
    }
  }
  {
    std::lock_guard<std::mutex> input_lock(inputMutex());
    if (inputStreamState().opened && inputStreamState().input) {
      return inputStreamState().input->getVolume(volume, error);
    }
  }
  std::string build_error;
  AudioInput input(toInputConfig(config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!input.open(error)) {
    return false;
  }
  return input.getVolume(volume, error);
}

bool Audio::setInputVolume(int volume, const AudioIoConfig &config,
                           std::string *error) const {
  {
    std::lock_guard<std::mutex> session_lock(sessionMutex());
    if (sessionState().opened && sessionState().input) {
      return sessionState().input->setVolume(volume, error);
    }
  }
  {
    std::lock_guard<std::mutex> input_lock(inputMutex());
    if (inputStreamState().opened && inputStreamState().input) {
      return inputStreamState().input->setVolume(volume, error);
    }
  }
  std::string build_error;
  AudioInput input(toInputConfig(config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!input.open(error)) {
    return false;
  }
  return input.setVolume(volume, error);
}

bool Audio::getOutputVolume(int *volume, const AudioIoConfig &config,
                            std::string *error) const {
  if (!volume) {
    setError(error, "output volume pointer is null");
    return false;
  }
  {
    std::lock_guard<std::mutex> session_lock(sessionMutex());
    if (sessionState().opened && sessionState().output) {
      return sessionState().output->getVolume(volume, error);
    }
  }
  {
    std::lock_guard<std::mutex> output_lock(outputMutex());
    if (outputStreamState().opened && outputStreamState().output) {
      return outputStreamState().output->getVolume(volume, error);
    }
  }
  std::string build_error;
  AudioOutput output(toOutputConfig(config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!output.open(error)) {
    return false;
  }
  return output.getVolume(volume, error);
}

bool Audio::setOutputVolume(int volume, const AudioIoConfig &config,
                            std::string *error) const {
  {
    std::lock_guard<std::mutex> session_lock(sessionMutex());
    if (sessionState().opened && sessionState().output) {
      return sessionState().output->setVolume(volume, error);
    }
  }
  {
    std::lock_guard<std::mutex> output_lock(outputMutex());
    if (outputStreamState().opened && outputStreamState().output) {
      return outputStreamState().output->setVolume(volume, error);
    }
  }
  std::string build_error;
  AudioOutput output(toOutputConfig(config, &build_error));
  if (!build_error.empty()) {
    setError(error, build_error);
    return false;
  }
  if (!output.open(error)) {
    return false;
  }
  return output.setVolume(volume, error);
}

bool Audio::openInputStream(const AudioInputStreamConfig &config,
                            std::string *error) const {
  std::lock_guard<std::mutex> lock(controlMutex());
  if (sessionState().opened) {
    setError(error, "audio session is open; close it before opening input stream");
    return false;
  }
  std::lock_guard<std::mutex> input_lock(inputMutex());
  if (inputStreamState().opened) {
    setError(error, "audio input stream is already open");
    return false;
  }
  return openInputState(&inputStreamState(), config, error);
}

bool Audio::closeInputStream(std::string *error) const {
  (void)error;
  std::lock_guard<std::mutex> input_lock(inputMutex());
  inputStreamState().close();
  return true;
}

bool Audio::inputStreamStatus(AudioInputStreamStatus *status,
                              std::string *error) const {
  if (!status) {
    setError(error, "audio input stream status pointer is null");
    return false;
  }
  std::lock_guard<std::mutex> input_lock(inputMutex());
  status->opened = inputStreamState().opened;
  status->talk_vqe_enabled = inputStreamState().talk_vqe_enabled;
  status->sequence = inputStreamState().sequence;
  status->config = inputStreamState().config;
  return true;
}

bool Audio::readInputChunk(AudioPcmChunk *chunk, std::string *error) const {
  std::lock_guard<std::mutex> input_lock(inputMutex());
  if (!inputStreamState().opened || !inputStreamState().input) {
    setError(error, "audio input stream is not open");
    return false;
  }
  AudioFrame frame;
  if (!inputStreamState().input->readFrame(&frame, inputStreamState().config.io.timeout_ms, error)) {
    return false;
  }
  frame.sequence = ++inputStreamState().sequence;
  if (!audioFrameToChunk(frame, 0, chunk, error)) {
    return false;
  }
  chunk->sample_rate = inputStreamState().config.io.sample_rate;
  chunk->channels = inputStreamState().config.io.channels;
  chunk->bit_depth = inputStreamState().config.io.bit_depth;
  return true;
}

bool Audio::openOutputStream(const AudioIoConfig &config,
                             std::string *error) const {
  std::lock_guard<std::mutex> lock(controlMutex());
  if (sessionState().opened) {
    setError(error, "audio session is open; close it before opening output stream");
    return false;
  }
  std::lock_guard<std::mutex> output_lock(outputMutex());
  if (outputStreamState().opened) {
    setError(error, "audio output stream is already open");
    return false;
  }
  return openOutputState(&outputStreamState(), config, error);
}

bool Audio::closeOutputStream(std::string *error) const {
  (void)error;
  std::lock_guard<std::mutex> output_lock(outputMutex());
  outputStreamState().close();
  return true;
}

bool Audio::outputStreamStatus(AudioOutputStreamStatus *status,
                               std::string *error) const {
  if (!status) {
    setError(error, "audio output stream status pointer is null");
    return false;
  }
  std::lock_guard<std::mutex> output_lock(outputMutex());
  status->opened = outputStreamState().opened;
  status->sequence = outputStreamState().sequence;
  status->config = outputStreamState().config;
  return true;
}

bool Audio::writeOutputChunk(const AudioPcmChunk &chunk,
                             std::string *error) const {
  std::lock_guard<std::mutex> output_lock(outputMutex());
  if (!outputStreamState().opened || !outputStreamState().output) {
    setError(error, "audio output stream is not open");
    return false;
  }
  AudioFrame frame;
  const AudioIoConfig &config = outputStreamState().config;
  if (!chunkToAudioFrame(chunk, config, &frame, error)) {
    return false;
  }
  if (frame.sequence == 0) {
    frame.sequence = ++outputStreamState().sequence;
  }
  return outputStreamState().output->writeFrame(frame, config.timeout_ms, error);
}

bool Audio::openSession(const AudioSessionConfig &config,
                        std::string *error) const {
  std::lock_guard<std::mutex> lock(controlMutex());
  if (inputStreamState().opened || outputStreamState().opened) {
    setError(error, "close standalone input/output streams before opening a session");
    return false;
  }
  std::lock_guard<std::mutex> session_lock(sessionMutex());
  if (sessionState().opened) {
    setError(error, "audio session is already open");
    return false;
  }
  return openSessionState(&sessionState(), config, error);
}

bool Audio::closeSession(std::string *error) const {
  (void)error;
  std::lock_guard<std::mutex> session_lock(sessionMutex());
  sessionState().close();
  return true;
}

bool Audio::sessionStatus(AudioSessionStatus *status,
                          std::string *error) const {
  if (!status) {
    setError(error, "audio session status pointer is null");
    return false;
  }
  std::lock_guard<std::mutex> session_lock(sessionMutex());
  status->opened = sessionState().opened;
  status->input_opened = sessionState().input_opened;
  status->output_opened = sessionState().output_opened;
  status->encoder_opened = sessionState().encoder_opened;
  status->decoder_opened = sessionState().decoder_opened;
  status->talk_vqe_enabled = sessionState().talk_vqe_enabled;
  status->input_sequence = sessionState().input_sequence;
  status->output_sequence = sessionState().output_sequence;
  status->encoded_sequence = sessionState().encoded_sequence;
  status->config = sessionState().config;
  return true;
}

bool Audio::readEncodedChunk(AudioEncodedStream *chunk,
                             std::string *error) const {
  if (!chunk) {
    setError(error, "audio encoded chunk pointer is null");
    return false;
  }
  std::lock_guard<std::mutex> session_lock(sessionMutex());
  if (!sessionState().opened || !sessionState().input || !sessionState().encoder) {
    setError(error, "audio session with input+encoder is not open");
    return false;
  }
  AudioFrame frame;
  if (!sessionState().input->readFrame(&frame, sessionState().config.io.timeout_ms, error)) {
    return false;
  }
  frame.sequence = ++sessionState().input_sequence;
  if (!sessionState().encoder->encodeFrame(frame, chunk, error)) {
    return false;
  }
  chunk->sequence = ++sessionState().encoded_sequence;
  return true;
}

bool Audio::writeDecodedChunk(const AudioEncodedStream &chunk,
                              std::string *error) const {
  std::lock_guard<std::mutex> session_lock(sessionMutex());
  if (!sessionState().opened || !sessionState().decoder || !sessionState().output) {
    setError(error, "audio session with decoder+output is not open");
    return false;
  }
  AudioFrame frame;
  if (!sessionState().decoder->decodeStream(chunk, &frame, true, error)) {
    return false;
  }
  frame.sequence = ++sessionState().output_sequence;
  return sessionState().output->writeFrame(frame, sessionState().config.io.timeout_ms, error);
}

}  // namespace tdl_app
