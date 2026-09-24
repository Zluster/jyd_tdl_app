// Python media bindings kept outside the small module entry point.
// Audio and local-video APIs share this translation unit, while vision stays
// in tdl_py_module.cpp.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "tdl_app/audio.hpp"
#include "tdl_app/audio_output.hpp"
#include "tdl_app/video_player.hpp"

// Declarations only; the implementation is compiled once in
// third_party/vendor/minimp3/minimp3_impl.c.
#include "minimp3_ex.h"

#if defined(TDL_PY_WITH_NPU) && !defined(TDL_PY_AUDIO_BASE_ONLY)
#include "tdl_app/direct_keyword_spotter.hpp"
#include "tdl_app/npu_asr_recognizer.hpp"
#include "tdl_app/speaker_recognizer.hpp"
#include "tdl_app/text_to_speech.hpp"
#endif

namespace nb = nanobind;

namespace {

#if defined(TDL_PY_WITH_NPU) && !defined(TDL_PY_AUDIO_BASE_ONLY)
bool pcm16FromBytes(const nb::bytes &pcm, std::vector<std::int16_t> *samples,
                    std::string *error) {
  if (!samples) {
    if (error) *error = "PCM output pointer is null";
    return false;
  }
  if (pcm.size() == 0 || pcm.size() % sizeof(std::int16_t) != 0) {
    if (error) *error = "PCM must be non-empty signed 16-bit mono bytes";
    return false;
  }
  samples->resize(pcm.size() / sizeof(std::int16_t));
  std::memcpy(samples->data(), pcm.c_str(), pcm.size());
  return true;
}

std::string defaultFirmwarePath() {
  const char *firmware = std::getenv("BMRUNTIME_USING_FIRMWARE");
  return (firmware && firmware[0]) ? std::string(firmware)
                                   : "/lib/firmware/libbm1688_kernel_module.so";
}

tdl_app::ModelSessionConfig modelConfig(const std::string &model_spec) {
  return tdl_app::ModelSessionConfig::fromSpec(model_spec,
                                                defaultFirmwarePath());
}

// This is private plumbing for the high-level algorithm classes below.  Each
// Python application owns one algorithm and therefore one microphone stream;
// PCM is never part of the app-facing real-time API.
class RealtimeMicrophone {
 public:
  bool start(int input_volume, int points_per_frame, int frame_count,
             int frame_depth, int timeout_ms, std::string *error) {
    tdl_app::AudioInputStreamConfig config;
    config.io.sample_rate = 16000;
    config.io.channels = 1;
    config.io.bit_depth = 16;
    config.io.ai_volume = input_volume;
    config.io.points_per_frame = points_per_frame;
    config.io.frame_count = frame_count;
    config.io.frame_depth = frame_depth;
    config.io.timeout_ms = timeout_ms;
    return audio_.openInputStream(config, error);
  }

  bool read(std::vector<std::int16_t> *samples,
            tdl_app::AudioPcmChunk *metadata, std::string *error) {
    if (!samples) {
      if (error) *error = "PCM sample output pointer is null";
      return false;
    }
    tdl_app::AudioPcmChunk chunk;
    if (!audio_.readInputChunk(&chunk, error)) return false;
    if (chunk.sample_rate != 16000 || chunk.channels != 1 ||
        chunk.bit_depth != 16 || chunk.data.empty() ||
        chunk.data.size() % sizeof(std::int16_t) != 0) {
      if (error) *error = "microphone did not return 16 kHz mono PCM16";
      return false;
    }
    samples->resize(chunk.data.size() / sizeof(std::int16_t));
    std::memcpy(samples->data(), chunk.data.data(), chunk.data.size());
    if (metadata) *metadata = std::move(chunk);
    return true;
  }

  bool stop(std::string *error) { return audio_.closeInputStream(error); }

  bool opened() const {
    tdl_app::AudioInputStreamStatus status;
    return audio_.inputStreamStatus(&status, nullptr) && status.opened;
  }

 private:
 tdl_app::Audio audio_;
};
#endif

// High-level AI/AO facade for Python. Audio operations return false and
// preserve the hardware error in last_error so an application can decide how
// to recover without an exception unwinding its UI loop.
#ifndef TDL_AUDIO_ONLY
class PyAudio {
 public:
  ~PyAudio() {
    audio_.closeInputStream(nullptr);
    audio_.closeOutputStream(nullptr);
  }

  bool recordWav(const std::string &path, double seconds, int sample_rate,
                 int channels, int input_volume, int points_per_frame,
                 int frame_count, int frame_depth, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        sample_rate, channels, input_volume, 24, points_per_frame,
        frame_count, frame_depth, timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.recordWav(path, seconds, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object capturePcm(double seconds, int sample_rate, int channels,
                        int input_volume, int points_per_frame,
                        int frame_count, int frame_depth, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        sample_rate, channels, input_volume, 24, points_per_frame,
        frame_count, frame_depth, timeout_ms);
    std::string error;
    std::vector<std::uint8_t> pcm;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      tdl_app::AudioInputStreamConfig input_config;
      input_config.io = config;
      ok = audio_.openInputStream(input_config, &error);
      if (ok) {
        const std::size_t bytes_per_sample =
            static_cast<std::size_t>(config.bit_depth / 8);
        const std::size_t target_bytes = static_cast<std::size_t>(
            seconds * config.sample_rate * config.channels * bytes_per_sample + 0.5);
        if (seconds <= 0.0 || bytes_per_sample == 0 || target_bytes == 0) {
          error = "capture seconds and audio format must be valid";
          ok = false;
        }
        while (ok && pcm.size() < target_bytes) {
          tdl_app::AudioPcmChunk chunk;
          if (!audio_.readInputChunk(&chunk, &error)) {
            ok = false;
            break;
          }
          if (chunk.data.empty()) {
            error = "audio input returned an empty PCM chunk";
            ok = false;
            break;
          }
          const std::size_t count = std::min(
              chunk.data.size(), target_bytes - pcm.size());
          pcm.insert(pcm.end(), chunk.data.begin(), chunk.data.begin() + count);
        }
        std::string close_error;
        if (!audio_.closeInputStream(&close_error) && ok) {
          error = close_error;
          ok = false;
        }
      }
    }
    last_error_ = error;
    if (!ok) {
      return nb::none();
    }
    return nb::bytes(reinterpret_cast<const char *>(pcm.data()), pcm.size());
  }

  bool openInputStream(int sample_rate, int channels, int input_volume,
                       int points_per_frame, int frame_count, int frame_depth,
                       int timeout_ms) {
    tdl_app::AudioInputStreamConfig config;
    config.io = makeConfig(sample_rate, channels, input_volume, 24,
                           points_per_frame, frame_count, frame_depth,
                           timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.openInputStream(config, &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object readInputChunk() {
    tdl_app::AudioPcmChunk chunk;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.readInputChunk(&chunk, &error);
    }
    last_error_ = error;
    if (!ok || chunk.data.empty()) return nb::none();
    return nb::bytes(reinterpret_cast<const char *>(chunk.data.data()),
                     chunk.data.size());
  }

  bool closeInputStream() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.closeInputStream(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool openOutputStream(int sample_rate, int channels, int output_volume,
                        int points_per_frame, int frame_count, int frame_depth,
                        int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        sample_rate, channels, 24, output_volume, points_per_frame,
        frame_count, frame_depth, timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.openOutputStream(config, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool writeOutputChunk(const nb::bytes &pcm) {
    if (pcm.size() == 0 || pcm.size() % sizeof(std::int16_t) != 0) {
      last_error_ = "PCM must be non-empty signed 16-bit mono bytes";
      return false;
    }
    tdl_app::AudioPcmChunk chunk;
    chunk.data.resize(pcm.size());
    std::memcpy(chunk.data.data(), pcm.c_str(), pcm.size());
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.writeOutputChunk(chunk, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool closeOutputStream() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.closeOutputStream(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool playWav(const std::string &path, int output_volume, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        16000, 1, 24, output_volume, 160, 8, 8, timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.playWav(path, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool loopback(double seconds, int sample_rate, int channels,
                int input_volume, int output_volume, int points_per_frame,
                int frame_count, int frame_depth, int timeout_ms) {
    tdl_app::AudioSessionConfig config;
    config.io = makeConfig(sample_rate, channels, input_volume, output_volume,
                           points_per_frame, frame_count, frame_depth,
                           timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.loopback(seconds, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool setInputVolume(int volume) {
    std::string error;
    const bool ok = audio_.setInputVolume(volume, {}, &error);
    last_error_ = error;
    return ok;
  }

  bool setOutputVolume(int volume) {
    std::string error;
    const bool ok = audio_.setOutputVolume(volume, {}, &error);
    last_error_ = error;
    return ok;
  }

  nb::object inputVolume() {
    int volume = 0;
    std::string error;
    if (!audio_.getInputVolume(&volume, {}, &error)) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::int_(volume);
  }

  nb::object outputVolume() {
    int volume = 0;
    std::string error;
    if (!audio_.getOutputVolume(&volume, {}, &error)) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::int_(volume);
  }

  nb::dict status() const {
    const tdl_app::AudioStatus status = audio_.status();
    nb::dict out;
    out["runtime_ready"] = status.runtime_ready;
    out["input_stream_open"] = status.input_stream_open;
    out["output_stream_open"] = status.output_stream_open;
    out["session_open"] = status.session_open;
    out["sample_rate"] = status.sample_rate;
    out["channels"] = status.channels;
    out["bit_depth"] = status.bit_depth;
    out["ai_device"] = status.ai_device;
    out["ai_channel"] = status.ai_channel;
    out["ao_device"] = status.ao_device;
    out["ao_channel"] = status.ao_channel;
    out["note"] = status.note;
    return out;
  }

  const std::string &lastError() const { return last_error_; }

 private:
  static tdl_app::AudioIoConfig makeConfig(int sample_rate, int channels,
                                            int input_volume,
                                            int output_volume,
                                            int points_per_frame,
                                            int frame_count, int frame_depth,
                                            int timeout_ms) {
    tdl_app::AudioIoConfig config;
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.ai_volume = input_volume;
    config.ao_volume = output_volume;
    config.points_per_frame = points_per_frame;
    config.frame_count = frame_count;
    config.frame_depth = frame_depth;
    config.timeout_ms = timeout_ms;
    return config;
  }

  tdl_app::Audio audio_;
  std::string last_error_;
};

class PyAudioOutputStream {
 public:
  PyAudioOutputStream() = default;
  ~PyAudioOutputStream() {
    // No other thread can be inside a method here: they would still hold a
    // reference to the Python object.  Plain lock is therefore deadlock-free.
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
  }

  PyAudioOutputStream(const PyAudioOutputStream &) = delete;
  PyAudioOutputStream &operator=(const PyAudioOutputStream &) = delete;

  bool open(int sample_rate, int channels, int bit_depth, int output_volume,
            int points_per_frame, int frame_count, int timeout_ms,
            int ao_device, int ao_channel, int ao_card_id) {
    std::string error;
    tdl_app::AudioOutput::Config config;
    if (!buildConfig(sample_rate, channels, bit_depth, output_volume,
                     points_per_frame, frame_count, ao_device, ao_channel,
                     ao_card_id, &config, &error)) {
      last_error_ = error;
      return false;
    }
    if (timeout_ms < 0) {
      last_error_ = "timeout_ms must be >= 0";
      return false;
    }
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (output_) {
        error = "audio output stream is already open";
      } else {
        std::unique_ptr<tdl_app::AudioOutput> output(
            new tdl_app::AudioOutput(config));
        ok = output->open(&error);
        if (ok) {
          output_ = std::move(output);
          bit_width_ = config.bit_width;
          sound_mode_ = config.sound_mode;
          channels_ = channels;
          frame_samples_ = output_->periodFrames();
          frame_bytes_ = static_cast<std::size_t>(frame_samples_) *
                         static_cast<std::size_t>(channels) *
                         static_cast<std::size_t>(bit_depth / 8);
          timeout_ms_ = timeout_ms;
          sequence_ = 0;
          paused_ = false;
        }
      }
    }
    last_error_ = ok ? std::string() : error;
    return ok;
  }

  // Send exactly one period of interleaved PCM: frame_bytes bytes, i.e.
  // frame_samples samples per channel (pad the tail of a file with zeros, as
  // playWav does).  Blocks for at most timeout_ms while the driver queue is full.
  bool write(const nb::bytes &pcm) {
    std::vector<std::uint8_t> data(pcm.size());
    if (!data.empty()) {
      std::memcpy(data.data(), pcm.c_str(), pcm.size());
    }
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!output_) {
        error = "audio output stream is not open";
      } else if (data.size() != frame_bytes_) {
        error = "PCM block must be exactly frame_bytes (" +
                std::to_string(frame_bytes_) + ") bytes, got " +
                std::to_string(data.size());
      } else {
        tdl_app::AudioFrame frame;
        frame.bit_width = bit_width_;
        frame.sound_mode = sound_mode_;
        frame.sequence = ++sequence_;
        frame.bytes_per_channel =
            static_cast<std::uint32_t>(data.size() / static_cast<std::size_t>(channels_));
        // Interleaved stereo travels in plane 0, same as Audio::writeOutputChunk.
        frame.channels.push_back(std::move(data));
        ok = output_->writeFrame(frame, timeout_ms_, &error);
      }
    }
    last_error_ = ok ? std::string() : error;
    return ok;
  }

  bool pause() {
    return control([this](std::string *error) {
      if (!output_->pause(error)) return false;
      paused_ = true;
      return true;
    });
  }

  bool resume() {
    return control([this](std::string *error) {
      if (!output_->resume(error)) return false;
      paused_ = false;
      return true;
    });
  }

  bool setVolume(int volume) {
    return control([this, volume](std::string *error) {
      return output_->setVolume(volume, error);
    });
  }

  // {"total", "free", "busy"} in bytes of the channel's queue (the vendor
  // header says "number of channel buffer", but CVI_AO_QueryChnStat reports the
  // share buffer size and its fill level in bytes), or None on failure.  busy
  // is how much submitted audio the library has not yet handed to the hardware;
  // all three are 0 before the first write creates the queue.
  nb::object bufferState() {
    tdl_app::AudioOutput::ChannelState state;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!output_) {
        error = "audio output stream is not open";
      } else {
        ok = output_->queryState(&state, &error);
      }
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    nb::dict out;
    out["total"] = state.total;
    out["free"] = state.free;
    out["busy"] = state.busy;
    return out;
  }

  void close() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
  }

  bool opened() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return output_ != nullptr;
  }

  bool paused() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return output_ != nullptr && paused_;
  }

  // Effective period per write(): samples per channel, and bytes.  0 while closed.
  int frameSamples() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return output_ ? frame_samples_ : 0;
  }

  int frameBytes() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return output_ ? static_cast<int>(frame_bytes_) : 0;
  }

  const std::string &lastError() const { return last_error_; }

 private:
  template <typename Fn>
  bool control(Fn fn) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!output_) {
        error = "audio output stream is not open";
      } else {
        ok = fn(&error);
      }
    }
    last_error_ = ok ? std::string() : error;
    return ok;
  }

  void closeLocked() {
    if (output_) {
      output_->close();
      output_.reset();
    }
    paused_ = false;
  }

  static bool buildConfig(int sample_rate, int channels, int bit_depth,
                          int output_volume, int points_per_frame,
                          int frame_count, int ao_device, int ao_channel,
                          int ao_card_id, tdl_app::AudioOutput::Config *config,
                          std::string *error) {
    switch (sample_rate) {
      case 8000: config->sample_rate = tdl_app::AudioSampleRate::Hz8000; break;
      case 11025: config->sample_rate = tdl_app::AudioSampleRate::Hz11025; break;
      case 16000: config->sample_rate = tdl_app::AudioSampleRate::Hz16000; break;
      case 22050: config->sample_rate = tdl_app::AudioSampleRate::Hz22050; break;
      case 24000: config->sample_rate = tdl_app::AudioSampleRate::Hz24000; break;
      case 32000: config->sample_rate = tdl_app::AudioSampleRate::Hz32000; break;
      case 44100: config->sample_rate = tdl_app::AudioSampleRate::Hz44100; break;
      case 48000: config->sample_rate = tdl_app::AudioSampleRate::Hz48000; break;
      case 64000: config->sample_rate = tdl_app::AudioSampleRate::Hz64000; break;
      default:
        *error = "unsupported sample rate: " + std::to_string(sample_rate);
        return false;
    }
    switch (bit_depth) {
      case 8: config->bit_width = tdl_app::AudioBitWidth::Bits8; break;
      case 16: config->bit_width = tdl_app::AudioBitWidth::Bits16; break;
      case 24: config->bit_width = tdl_app::AudioBitWidth::Bits24; break;
      case 32: config->bit_width = tdl_app::AudioBitWidth::Bits32; break;
      default:
        *error = "bit depth must be 8/16/24/32";
        return false;
    }
    if (channels == 1) {
      config->sound_mode = tdl_app::AudioSoundMode::Mono;
    } else if (channels == 2) {
      config->sound_mode = tdl_app::AudioSoundMode::Stereo;
    } else {
      *error = "channels must be 1 or 2";
      return false;
    }
    if (points_per_frame <= 0 || frame_count <= 0) {
      *error = "points_per_frame and frame_count must be > 0";
      return false;
    }
    config->device = ao_device;
    config->channel = ao_channel;
    config->card_id = ao_card_id;
    config->points_per_frame = points_per_frame;
    config->frame_count = frame_count;
    config->channel_count = channels;
    config->volume_db = output_volume;
    return true;
  }

  std::mutex mutex_;
  std::unique_ptr<tdl_app::AudioOutput> output_;
  tdl_app::AudioBitWidth bit_width_ = tdl_app::AudioBitWidth::Bits16;
  tdl_app::AudioSoundMode sound_mode_ = tdl_app::AudioSoundMode::Mono;
  int channels_ = 1;
  int frame_samples_ = 0;
  std::size_t frame_bytes_ = 0;
  int timeout_ms_ = 100;
  std::uint32_t sequence_ = 0;
  bool paused_ = false;
  std::string last_error_;   // only touched with the GIL held
};

// Streaming MP3 decoder for Python: the PCM source half of a music player,
// the AO half being AudioOutputStream.  minimp3_ex maps the file, scans the
// frame headers once at open() (or trusts the Xing/VBR tag) to learn the
// exact length and build a sample index, and then decodes on demand:
//
//     dec = tdl_py.Mp3Decoder()
//     dec.open(path)                      # False + last_error on failure
//     dec.sample_rate, dec.channels, dec.frames, dec.duration_ms
//     pcm = dec.read(960)                 # up to 960 frames of interleaved S16LE;
//                                         # b"" at the end, None on a decode error
//     dec.seek(frame)                     # sample-accurate, any time
//     dec.close()
//
// "frame" here always means one sample per channel (the WAV convention the
// player already uses), never an MP3 packet.  Decoding releases the GIL; a
// mutex serialises the methods so a controller thread can close() or seek()
// while a worker is blocked in read() without corrupting the decoder.
class PyMp3Decoder {
 public:
  PyMp3Decoder() = default;
  ~PyMp3Decoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
  }

  PyMp3Decoder(const PyMp3Decoder &) = delete;
  PyMp3Decoder &operator=(const PyMp3Decoder &) = delete;

  bool open(const std::string &path) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (opened_) {
        error = "mp3 decoder is already open";
      } else {
        ok = openLocked(path, &error);
      }
    }
    last_error_ = ok ? std::string() : error;
    return ok;
  }

  // Decode up to frame_count frames.  Returns b"" at the end of the track and
  // None (with last_error set) on a decode error; a short block is normal
  // near the end of the file.
  nb::object read(int frame_count) {
    if (frame_count <= 0) {
      last_error_ = "frame_count must be > 0";
      return nb::none();
    }
    std::vector<mp3d_sample_t> pcm;
    std::string error;
    std::size_t got = 0;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!opened_) {
        error = "mp3 decoder is not open";
      } else {
        pcm.resize(static_cast<std::size_t>(frame_count) *
                   static_cast<std::size_t>(channels_));
        got = mp3dec_ex_read(&dec_, pcm.data(), pcm.size());
        if (got == 0 && dec_.last_error != 0) {
          error = describe("decode", dec_.last_error);
        } else {
          got -= got % static_cast<std::size_t>(channels_);   // whole frames only
          ok = true;
        }
      }
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::bytes(reinterpret_cast<const char *>(pcm.data()),
                     got * sizeof(mp3d_sample_t));
  }

  // Sample-accurate seek to a frame index (clamped to the track).
  bool seek(std::int64_t frame_index) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!opened_) {
        error = "mp3 decoder is not open";
      } else {
        const std::int64_t target =
            std::max<std::int64_t>(0, std::min(frame_index, frames_));
        const int ret = mp3dec_ex_seek(
            &dec_, static_cast<std::uint64_t>(target) *
                       static_cast<std::uint64_t>(channels_));
        if (ret != 0) {
          error = describe("seek", ret);
        } else {
          ok = true;
        }
      }
    }
    last_error_ = ok ? std::string() : error;
    return ok;
  }

  void close() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
  }

  bool opened() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_;
  }

  int sampleRate() { return locked(sample_rate_); }
  int channels() { return locked(channels_); }
  std::int64_t frames() { return locked(frames_); }

  std::int64_t durationMs() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return (opened_ && sample_rate_ > 0) ? frames_ * 1000 / sample_rate_ : 0;
  }

  // Next frame read() will return, i.e. frames decoded so far from the
  // current seek position.
  std::int64_t position() {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || channels_ <= 0) return 0;
    return static_cast<std::int64_t>(dec_.cur_sample /
                                     static_cast<std::uint64_t>(channels_));
  }

  const std::string &lastError() const { return last_error_; }

 private:
  template <typename T>
  T locked(const T &field) {
    nb::gil_scoped_release guard;
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_ ? field : T();
  }

  static std::string describe(const char *what, int code) {
    const char *reason = "unknown error";
    switch (code) {
      case MP3D_E_PARAM: reason = "invalid parameter"; break;
      case MP3D_E_MEMORY: reason = "out of memory"; break;
      case MP3D_E_IOERROR: reason = "cannot read file"; break;
      case MP3D_E_USER: reason = "not an MPEG audio file"; break;
      case MP3D_E_DECODE: reason = "stream parameters changed mid-file"; break;
      default: break;
    }
    return std::string("mp3 ") + what + " failed: " + reason;
  }

  bool openLocked(const std::string &path, std::string *error) {
    // MP3D_SEEK_TO_SAMPLE: open() walks the frame headers (cheap, no decoding)
    // so frames_ is exact and seek() lands on a sample, not a byte offset.
    const int ret = mp3dec_ex_open(&dec_, path.c_str(), MP3D_SEEK_TO_SAMPLE);
    if (ret != 0) {
      *error = describe("open", ret);
      return false;
    }
    if (dec_.samples == 0 || dec_.info.channels <= 0 || dec_.info.hz <= 0) {
      mp3dec_ex_close(&dec_);
      *error = "mp3 open failed: no MPEG audio frames found";
      return false;
    }
    if (dec_.info.channels > 2) {
      mp3dec_ex_close(&dec_);
      *error = "mp3 open failed: only mono and stereo are supported";
      return false;
    }
    sample_rate_ = dec_.info.hz;
    channels_ = dec_.info.channels;
    frames_ = static_cast<std::int64_t>(dec_.samples /
                                        static_cast<std::uint64_t>(channels_));
    opened_ = true;
    return true;
  }

  void closeLocked() {
    if (opened_) {
      mp3dec_ex_close(&dec_);
      opened_ = false;
    }
    sample_rate_ = 0;
    channels_ = 0;
    frames_ = 0;
  }

  std::mutex mutex_;
  mp3dec_ex_t dec_{};
  bool opened_ = false;
  int sample_rate_ = 0;
  int channels_ = 0;
  std::int64_t frames_ = 0;   // per-channel sample frames in the track
  std::string last_error_;    // only touched with the GIL held
};
#endif

#if defined(TDL_PY_WITH_NPU) && !defined(TDL_PY_AUDIO_BASE_ONLY)
class PySpeakerRecognizer {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(modelConfig(model_spec), &error);
    }
    last_error_ = error;
    return ok;
  }

  bool enroll(const std::string &label, const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return false;
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) return false;
    if (!database_.upsert(label, embedding, &last_error_)) return false;
    last_error_.clear();
    return true;
  }

  nb::object recognize(const nb::bytes &pcm, float threshold) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) {
      return nb::none();
    }
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) {
      return nb::none();
    }
    const tdl_app::SpeakerMatch match =
        recognizer_.identify(embedding, database_, threshold);
    nb::dict result;
    result["label"] = match.label;
    result["score"] = match.score;
    result["matched"] = match.matched;
    last_error_.clear();
    return result;
  }

  nb::object verify(const std::string &label, const nb::bytes &pcm,
                    float threshold) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) return nb::none();
    return result(recognizer_.verify(label, embedding, database_, threshold));
  }

  bool beginEnroll(const std::string &label, double seconds, int input_volume,
                   int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Enroll, label, seconds, 0.60f,
                        input_volume, points_per_frame, timeout_ms);
  }

  bool beginVerify(const std::string &label, double seconds, float threshold,
                   int input_volume, int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Verify, label, seconds, threshold,
                        input_volume, points_per_frame, timeout_ms);
  }

  bool beginIdentify(double seconds, float threshold, int input_volume,
                     int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Identify, "", seconds, threshold,
                        input_volume, points_per_frame, timeout_ms);
  }

  // Read one live audio frame.  Applications call poll() in their regular UI
  // loop; it returns progress until the requested voice sample is complete.
  nb::object poll() {
    if (mode_ == CaptureMode::None) {
      last_error_ = "no speaker capture is active";
      return nb::none();
    }
    std::vector<std::int16_t> chunk;
    tdl_app::AudioPcmChunk metadata;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&chunk, &metadata, &error);
    }
    if (!ok) {
      microphone_.stop(nullptr);
      mode_ = CaptureMode::None;
      last_error_ = error;
      return nb::none();
    }
    const std::size_t remaining = target_samples_ - samples_.size();
    const std::size_t count = std::min(remaining, chunk.size());
    samples_.insert(samples_.end(), chunk.begin(), chunk.begin() + count);
    if (samples_.size() < target_samples_) {
      return progress(false, metadata);
    }

    microphone_.stop(nullptr);
    tdl_app::SpeakerEmbedding embedding;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples_, &embedding, &error);
    }
    if (!ok) {
      resetCapture();
      last_error_ = error;
      return nb::none();
    }

    nb::dict out = progress(true, metadata);
    if (mode_ == CaptureMode::Enroll) {
      ok = database_.upsert(label_, embedding, &error);
      out["label"] = label_;
      out["score"] = 1.0f;
      out["matched"] = ok;
    } else if (mode_ == CaptureMode::Verify) {
      appendMatch(&out, recognizer_.verify(label_, embedding, database_, threshold_));
    } else {
      appendMatch(&out, recognizer_.identify(embedding, database_, threshold_));
    }
    resetCapture();
    last_error_ = error;
    return ok ? nb::object(out) : nb::none();
  }

  void cancel() {
    microphone_.stop(nullptr);
    resetCapture();
    last_error_.clear();
  }

  bool capturing() const { return mode_ != CaptureMode::None; }

  bool saveDatabase(const std::string &path) {
    const bool ok = database_.save(path, &last_error_);
    return ok;
  }

  bool loadDatabase(const std::string &path) {
    const bool ok = database_.load(path, &last_error_);
    return ok;
  }

  void clear() {
    database_.clear();
    last_error_.clear();
  }

  std::vector<std::string> labels() const { return database_.labels(); }
  bool initialized() const { return recognizer_.initialized(); }
  const std::string &lastError() const { return last_error_; }

 private:
  enum class CaptureMode { None, Enroll, Verify, Identify };

  static nb::dict result(const tdl_app::SpeakerMatch &match) {
    nb::dict out;
    appendMatch(&out, match);
    return out;
  }

  static void appendMatch(nb::dict *out, const tdl_app::SpeakerMatch &match) {
    (*out)["label"] = match.label;
    (*out)["score"] = match.score;
    (*out)["matched"] = match.matched;
  }

  bool beginCapture(CaptureMode mode, const std::string &label, double seconds,
                    float threshold, int input_volume, int points_per_frame,
                    int timeout_ms) {
    if (!recognizer_.initialized()) {
      last_error_ = "speaker model is not loaded";
      return false;
    }
    if (mode_ != CaptureMode::None) {
      last_error_ = "speaker capture is already active";
      return false;
    }
    if (seconds <= 0.0) {
      last_error_ = "capture seconds must be > 0";
      return false;
    }
    const std::size_t target = static_cast<std::size_t>(seconds * 16000.0 + 0.5);
    if (target == 0) {
      last_error_ = "capture duration is too short";
      return false;
    }
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    if (!ok) {
      last_error_ = error;
      return false;
    }
    mode_ = mode;
    label_ = label;
    threshold_ = threshold;
    target_samples_ = target;
    samples_.clear();
    samples_.reserve(target);
    last_error_.clear();
    return true;
  }

  nb::dict progress(bool done, const tdl_app::AudioPcmChunk &chunk) const {
    nb::dict out;
    out["done"] = done;
    out["progress"] = static_cast<double>(samples_.size()) /
                      static_cast<double>(target_samples_);
    out["timestamp"] = chunk.timestamp;
    out["sequence"] = chunk.sequence;
    return out;
  }

  void resetCapture() {
    mode_ = CaptureMode::None;
    label_.clear();
    threshold_ = 0.60f;
    target_samples_ = 0;
    samples_.clear();
  }

  tdl_app::SpeakerRecognizer recognizer_;
  tdl_app::SpeakerDatabase database_;
  RealtimeMicrophone microphone_;
  CaptureMode mode_ = CaptureMode::None;
  std::string label_;
  float threshold_ = 0.60f;
  std::size_t target_samples_ = 0;
  std::vector<std::int16_t> samples_;
  std::string last_error_;
};

class PyStreamingAsr {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(modelConfig(model_spec), &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object accept(const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    std::string text;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.acceptPcm(samples, &text, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return nb::str(text.c_str());
  }

  nb::object finish() {
    std::string text;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.finish(&text, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return nb::str(text.c_str());
  }

  bool start(int input_volume, int points_per_frame, int timeout_ms) {
    if (!recognizer_.initialized()) {
      last_error_ = "ASR model is not loaded";
      return false;
    }
    recognizer_.resetStream();
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    last_error_ = error;
    return ok;
  }

  // One real-time microphone frame in, one incremental recognition result
  // out.  "text" is only what appeared in this call; "full_text" is the
  // utterance accumulated since start()/reset().
  nb::object read() {
    std::vector<std::int16_t> samples;
    tdl_app::AudioPcmChunk chunk;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&samples, &chunk, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    std::string text;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.acceptPcm(samples, &text, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    nb::dict out;
    out["text"] = text;
    out["full_text"] = recognizer_.text();
    out["timestamp"] = chunk.timestamp;
    out["sequence"] = chunk.sequence;
    last_error_.clear();
    return out;
  }

  bool stop() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.stop(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool listening() const { return microphone_.opened(); }

  void reset() {
    recognizer_.resetStream();
    last_error_.clear();
  }

  bool initialized() const { return recognizer_.initialized(); }
  const std::string &text() const { return recognizer_.text(); }
  const std::string &lastError() const { return last_error_; }

 private:
  tdl_app::NpuStreamingAsr recognizer_;
  RealtimeMicrophone microphone_;
  std::string last_error_;
};

class PyKeywordSpotter {
 public:
  bool load(const std::string &model_spec, const std::string &keywords_path,
            float threshold, int beam_width) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.load(model_spec, keywords_path, defaultFirmwarePath(), threshold,
                         beam_width, &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object accept(const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    std::vector<tdl_app::DirectKeywordResult> hits;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.accept(samples, &hits, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return result(hits);
  }

  nb::object finish() {
    std::vector<tdl_app::DirectKeywordResult> hits;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.finish(&hits, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return result(hits);
  }

  bool start(int input_volume, int points_per_frame, int timeout_ms) {
    if (!spotter_.initialized()) {
      last_error_ = "KWS model is not loaded";
      return false;
    }
    spotter_.resetStream();
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    last_error_ = error;
    return ok;
  }

  // Read and evaluate exactly one microphone frame.  An empty list is the
  // normal no-keyword result; None means a microphone or inference error.
  nb::object read() {
    std::vector<std::int16_t> samples;
    tdl_app::AudioPcmChunk chunk;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&samples, &chunk, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    std::vector<tdl_app::DirectKeywordResult> hits;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.accept(samples, &hits, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return result(hits);
  }

  bool stop() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.stop(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool listening() const { return microphone_.opened(); }

  nb::list scores() const { return result(spotter_.scores()); }

  void reset() {
    spotter_.resetStream();
    last_error_.clear();
  }

  bool initialized() const { return spotter_.initialized(); }
  const std::string &lastError() const { return last_error_; }

 private:
  static nb::dict one(const tdl_app::DirectKeywordResult &source) {
    nb::dict out;
    out["name"] = source.name;
    out["confidence"] = source.confidence;
    out["threshold"] = source.threshold;
    out["matched_tokens"] = source.matched_tokens;
    out["total_tokens"] = source.total_tokens;
    out["matched_text"] = source.matched_text;
    out["complete"] = source.complete;
    out["triggered"] = source.triggered;
    return out;
  }

  static nb::list result(const std::vector<tdl_app::DirectKeywordResult> &hits) {
    nb::list out;
    for (const tdl_app::DirectKeywordResult &hit : hits) out.append(one(hit));
    return out;
  }

  tdl_app::DirectKeywordSpotter spotter_;
  RealtimeMicrophone microphone_;
  std::string last_error_;
};

// 中文文字转语音。文本前处理、音素切分、NPU 推理与重采样都在 C++ 内完成，
// Python 侧只提交文本、取回 16 kHz 单声道 PCM，接口形状与 KWS/ASR 一致。
class PyTextToSpeech {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = speech_.load(model_spec, defaultFirmwarePath(), &error);
    }
    last_error_ = error;
    return ok;
  }

  // 返回 16 kHz 单声道有符号 16 位小端 PCM；失败返回 None 并写入 last_error。
  nb::object run(const std::string &text) {
    std::vector<std::int16_t> pcm;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = speech_.run(text, &pcm, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::bytes(reinterpret_cast<const char *>(pcm.data()),
                     pcm.size() * sizeof(std::int16_t));
  }

  bool synthesize(const std::string &text, const std::string &wav_path) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = speech_.synthesize(text, wav_path, &error);
    }
    last_error_ = error;
    return ok;
  }

  // 只做文本前处理，返回每段的 token 列表；失败返回 None。
  nb::object tokens(const std::string &text) {
    std::vector<std::vector<int>> chunks;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = speech_.tokens(text, &chunks, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    nb::list out;
    for (const std::vector<int> &chunk : chunks) {
      nb::list one;
      for (int token : chunk) one.append(token);
      out.append(one);
    }
    return out;
  }

  void reset() {
    speech_.reset();
    last_error_.clear();
  }

  bool initialized() const { return speech_.initialized(); }
  const std::string &lastError() const { return last_error_; }

  nb::dict stats() const {
    const tdl_app::TtsSynthesisStats &s = speech_.lastStats();
    nb::dict out;
    out["chunks"] = s.chunks;
    out["rendered"] = s.rendered;
    out["frames"] = s.frames;
    out["samples"] = s.samples;
    out["duration"] = s.duration;
    return out;
  }

 private:
  tdl_app::TextToSpeech speech_;
  std::string last_error_;
};

#endif






}  // namespace

void registerAudioBindings(nb::module_ &m) {
#ifndef TDL_AUDIO_ONLY
  // --- Audio ---------------------------------------------------------------
  nb::class_<PyAudio>(m, "Audio",
      "Basic AI/AO audio control. Methods return False on a hardware error; "
      "inspect last_error to handle it in the application.")
      .def(nb::init<>())
      .def("record_wav", &PyAudio::recordWav,
           nb::arg("path"), nb::arg("seconds") = 3.0,
           nb::arg("sample_rate") = 16000, nb::arg("channels") = 1,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Record signed PCM into a standard WAV file.")
      .def("capture_pcm", &PyAudio::capturePcm,
           nb::arg("seconds") = 3.0, nb::arg("sample_rate") = 16000,
           nb::arg("channels") = 1, nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Capture signed interleaved PCM bytes for inference.")
      .def("open_input_stream", &PyAudio::openInputStream,
           nb::arg("sample_rate") = 16000, nb::arg("channels") = 1,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Open a continuous PCM microphone stream.")
      .def("read_input_chunk", &PyAudio::readInputChunk,
           "Read one PCM chunk from an open microphone stream.")
      .def("close_input_stream", &PyAudio::closeInputStream,
           "Close a continuous PCM microphone stream.")
      .def("open_output_stream", &PyAudio::openOutputStream,
           nb::arg("sample_rate") = 16000, nb::arg("channels") = 1,
           nb::arg("output_volume") = 24,
           nb::arg("points_per_frame") = 960,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Open a continuous PCM speaker stream.")
      .def("write_output_chunk", &PyAudio::writeOutputChunk,
           nb::arg("pcm"), "Write signed PCM16 bytes to an open speaker stream.")
      .def("close_output_stream", &PyAudio::closeOutputStream,
           "Close a continuous PCM speaker stream.")
      .def("play_wav", &PyAudio::playWav,
           nb::arg("path"), nb::arg("output_volume") = 24,
           nb::arg("timeout_ms") = 1000,
           "Play a standard PCM WAV file through AO.")
      .def("loopback", &PyAudio::loopback,
           nb::arg("seconds") = 3.0, nb::arg("sample_rate") = 16000,
           nb::arg("channels") = 1, nb::arg("input_volume") = 24,
           nb::arg("output_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Route microphone input directly to speaker output for a fixed time.")
      .def("set_input_volume", &PyAudio::setInputVolume, nb::arg("volume"))
      .def("set_output_volume", &PyAudio::setOutputVolume, nb::arg("volume"))
      .def("input_volume", &PyAudio::inputVolume,
           "Return current input volume, or None on failure.")
      .def("output_volume", &PyAudio::outputVolume,
           "Return current output volume, or None on failure.")
      .def("status", &PyAudio::status)
      .def_prop_ro("last_error", &PyAudio::lastError);

  // --- Streaming AO output -------------------------------------------------
  nb::class_<PyAudioOutputStream>(m, "AudioOutputStream",
      "One AO output channel owned by this object. open() with the PCM format, "
      "then write() interleaved PCM blocks of exactly frame_bytes bytes "
      "(frame_samples samples per channel -- the driver rounds points_per_frame "
      "to whole milliseconds); pause/resume/buffer_state map onto the driver. "
      "The channel is released by close() or when the object is garbage "
      "collected. Methods return False on a hardware error; inspect last_error.")
      .def(nb::init<>())
      .def("open", &PyAudioOutputStream::open,
           nb::arg("sample_rate") = 16000, nb::arg("channels") = 1,
           nb::arg("bit_depth") = 16, nb::arg("output_volume") = 16,
           nb::arg("points_per_frame") = 320, nb::arg("frame_count") = 8,
           nb::arg("timeout_ms") = 100, nb::arg("ao_device") = 0,
           nb::arg("ao_channel") = 0, nb::arg("ao_card_id") = -1,
           "Open the AO channel for the given PCM format; read frame_samples "
           "afterwards for the period the driver settled on.")
      .def("write", &PyAudioOutputStream::write, nb::arg("pcm"),
           "Send exactly one period (frame_bytes bytes) of interleaved PCM; "
           "blocks at most timeout_ms while the driver queue is full.")
      .def("pause", &PyAudioOutputStream::pause,
           "Stop the driver from consuming queued audio (CVI_AO_PauseChn).")
      .def("resume", &PyAudioOutputStream::resume,
           "Continue consuming queued audio (CVI_AO_ResumeChn).")
      .def("set_volume", &PyAudioOutputStream::setVolume, nb::arg("volume"),
           "Change the output volume while open.")
      .def("buffer_state", &PyAudioOutputStream::bufferState,
           "Return {total, free, busy} bytes of the channel queue (busy = written "
           "but not yet handed to the hardware), or None on failure.")
      .def("close", &PyAudioOutputStream::close,
           "Release the AO channel (idempotent).")
      .def_prop_ro("opened", &PyAudioOutputStream::opened)
      .def_prop_ro("paused", &PyAudioOutputStream::paused)
      .def_prop_ro("frame_samples", &PyAudioOutputStream::frameSamples,
                   "Samples per channel the driver accepts per write(); 0 when closed.")
      .def_prop_ro("frame_bytes", &PyAudioOutputStream::frameBytes,
                   "Bytes per write(); 0 when closed.")
      .def_prop_ro("last_error", &PyAudioOutputStream::lastError);

  // --- MP3 file decoding ---------------------------------------------------
  nb::class_<PyMp3Decoder>(m, "Mp3Decoder",
      "Streaming MP3 decoder (minimp3) with sample-accurate seek: the PCM "
      "source for a music player, feeding AudioOutputStream. open(path), then "
      "read(frame_count) returns interleaved signed 16-bit PCM bytes (b\"\" at "
      "the end, None on a decode error); seek(frame) jumps to a per-channel "
      "sample frame. Methods return False/None on failure; inspect last_error.")
      .def(nb::init<>())
      .def("open", &PyMp3Decoder::open, nb::arg("path"),
           "Map the file, scan the frame headers for the exact length and "
           "build the seek index.")
      .def("read", &PyMp3Decoder::read, nb::arg("frame_count"),
           "Decode up to frame_count frames (samples per channel) of S16LE PCM.")
      .def("seek", &PyMp3Decoder::seek, nb::arg("frame_index"),
           "Seek to a per-channel sample frame; clamped to the track.")
      .def("close", &PyMp3Decoder::close, "Release the file (idempotent).")
      .def_prop_ro("opened", &PyMp3Decoder::opened)
      .def_prop_ro("sample_rate", &PyMp3Decoder::sampleRate)
      .def_prop_ro("channels", &PyMp3Decoder::channels)
      .def_prop_ro("frames", &PyMp3Decoder::frames,
                   "Total per-channel sample frames in the track.")
      .def_prop_ro("duration_ms", &PyMp3Decoder::durationMs)
      .def_prop_ro("position", &PyMp3Decoder::position,
                   "Frame index the next read() starts at.")
      .def_prop_ro("last_error", &PyMp3Decoder::lastError);
#endif

#if defined(TDL_PY_WITH_NPU) && !defined(TDL_PY_AUDIO_BASE_ONLY)
  // --- Speaker recognition -------------------------------------------------
  nb::class_<PySpeakerRecognizer>(m, "SpeakerRecognizer",
      "CAMPPlus speaker enrollment and recognition over 16 kHz mono PCM.")
      .def(nb::init<>())
      .def("load", &PySpeakerRecognizer::load, nb::arg("model_spec"),
           "Load the CAMPPlus speaker model.")
      .def("enroll", &PySpeakerRecognizer::enroll,
           nb::arg("label"), nb::arg("pcm"),
           "Extract a voice embedding and add or replace this label.")
      .def("recognize", &PySpeakerRecognizer::recognize,
           nb::arg("pcm"), nb::arg("threshold") = 0.60f,
           "Return {label, score, matched}, or None when extraction fails.")
      .def("verify", &PySpeakerRecognizer::verify,
           nb::arg("label"), nb::arg("pcm"), nb::arg("threshold") = 0.60f,
           "Verify a PCM sample against one enrolled label.")
      .def("begin_enroll", &PySpeakerRecognizer::beginEnroll,
           nb::arg("label"), nb::arg("seconds") = 3.0,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone enrollment; use poll() until done.")
      .def("begin_verify", &PySpeakerRecognizer::beginVerify,
           nb::arg("label"), nb::arg("seconds") = 3.0,
           nb::arg("threshold") = 0.60f, nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone verification; use poll() until done.")
      .def("begin_identify", &PySpeakerRecognizer::beginIdentify,
           nb::arg("seconds") = 3.0, nb::arg("threshold") = 0.60f,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone identification; use poll() until done.")
      .def("poll", &PySpeakerRecognizer::poll,
           "Consume one live microphone frame and return enrollment progress/result.")
      .def("cancel", &PySpeakerRecognizer::cancel)
      .def("save_database", &PySpeakerRecognizer::saveDatabase,
           nb::arg("path"))
      .def("load_database", &PySpeakerRecognizer::loadDatabase,
           nb::arg("path"))
      .def("clear", &PySpeakerRecognizer::clear)
      .def("labels", &PySpeakerRecognizer::labels)
      .def_prop_ro("initialized", &PySpeakerRecognizer::initialized)
      .def_prop_ro("capturing", &PySpeakerRecognizer::capturing)
      .def_prop_ro("last_error", &PySpeakerRecognizer::lastError);

  // --- Streaming ASR -------------------------------------------------------
  nb::class_<PyStreamingAsr>(m, "StreamingAsr",
      "Direct BMRT Zipformer ASR over signed 16-bit mono 16 kHz PCM."
      " No Sherpa or ONNX Runtime is used.")
      .def(nb::init<>())
      .def("load", &PyStreamingAsr::load, nb::arg("model_spec"),
           "Load the CV184X encoder/decoder/joiner ASR model set.")
      .def("accept", &PyStreamingAsr::accept, nb::arg("pcm"),
           "Accept PCM and return only text decoded by this call, or None on failure.")
      .def("finish", &PyStreamingAsr::finish,
           "Flush the final ASR chunk and return only final text, or None on failure.")
      .def("start", &PyStreamingAsr::start,
           nb::arg("input_volume") = 24, nb::arg("points_per_frame") = 160,
           nb::arg("timeout_ms") = 1000,
           "Open the microphone and reset this real-time recognition session.")
      .def("read", &PyStreamingAsr::read,
           "Recognize one real-time microphone frame; return text and metadata.")
      .def("stop", &PyStreamingAsr::stop,
           "Close the microphone; call finish() separately to flush ASR.")
      .def("reset", &PyStreamingAsr::reset)
      .def_prop_ro("initialized", &PyStreamingAsr::initialized)
      .def_prop_ro("listening", &PyStreamingAsr::listening)
      .def_prop_ro("text", &PyStreamingAsr::text)
      .def_prop_ro("last_error", &PyStreamingAsr::lastError);

  // --- Keyword spotting ----------------------------------------------------
  nb::class_<PyKeywordSpotter>(m, "KeywordSpotter",
      "Direct BMRT RNNT keyword spotter over signed 16-bit mono 16 kHz PCM."
      " No Sherpa or ONNX Runtime is used.")
      .def(nb::init<>())
      .def("load", &PyKeywordSpotter::load,
           nb::arg("model_spec"), nb::arg("keywords_path"),
           nb::arg("threshold") = -1.0f, nb::arg("beam_width") = 2,
           "Load CV184X KWS bmodels and a keyword token file.")
      .def("accept", &PyKeywordSpotter::accept, nb::arg("pcm"),
           "Accept PCM; return newly triggered keyword dictionaries, or None on failure.")
      .def("finish", &PyKeywordSpotter::finish)
      .def("start", &PyKeywordSpotter::start,
           nb::arg("input_volume") = 24, nb::arg("points_per_frame") = 160,
           nb::arg("timeout_ms") = 1000,
           "Open the microphone and reset this real-time keyword session.")
      .def("read", &PyKeywordSpotter::read,
           "Evaluate one real-time microphone frame and return keyword hits.")
      .def("stop", &PyKeywordSpotter::stop,
           "Close the microphone; call finish() separately to flush KWS.")
      .def("scores", &PyKeywordSpotter::scores,
           "Return current score dictionaries for every configured keyword.")
      .def("reset", &PyKeywordSpotter::reset)
      .def_prop_ro("initialized", &PyKeywordSpotter::initialized)
      .def_prop_ro("listening", &PyKeywordSpotter::listening)
      .def_prop_ro("last_error", &PyKeywordSpotter::lastError);

  // --- 中文文字转语音 ------------------------------------------------------
  nb::class_<PyTextToSpeech>(m, "TextToSpeech",
      "CV184X 中文文字转语音（Piper 中文前端 + 解码器，BMRT NPU）。"
      " 不使用 Sherpa 或 ONNX Runtime。")
      .def(nb::init<>())
      .def("load", &PyTextToSpeech::load, nb::arg("model_spec"),
           "加载 .mud 描述的 TTS 前端与解码 bmodel。")
      .def("run", &PyTextToSpeech::run, nb::arg("text"),
           "合成整段文本，返回 16 kHz 单声道 PCM16 字节，失败返回 None。")
      .def("synthesize", &PyTextToSpeech::synthesize,
           nb::arg("text"), nb::arg("wav_path"),
           "合成文本并写出标准 PCM WAV 文件。")
      .def("tokens", &PyTextToSpeech::tokens, nb::arg("text"),
           "只做文本前处理，返回每段 token 列表，失败返回 None。")
      .def("reset", &PyTextToSpeech::reset,
           "卸载 NPU 模型并释放设备（幂等）。")
      .def_prop_ro("initialized", &PyTextToSpeech::initialized)
      .def_prop_ro("stats", &PyTextToSpeech::stats,
                   "上一次合成的统计信息：段数、帧数、采样点数与时长。")
      .def_prop_ro("last_error", &PyTextToSpeech::lastError);
#endif

}

void registerVideoBindings(nb::module_ &m) {
  nb::class_<tdl_app::VideoPlayer>(m, "VideoPlayer",
      "Local H.264 player using FFmpeg decode and hardware VPSS/VO display; "
      "LVGL remains an overlay.")
      .def(nb::init<>())
      .def("play", [](tdl_app::VideoPlayer &self, const std::string &path,
                      bool loop) {
        std::string error;
        bool ok = false;
        {
          nb::gil_scoped_release release;
          ok = self.play(path, loop, &error);
        }
        if (!ok) throw std::runtime_error("video player start failed: " + error);
      }, nb::arg("path"), nb::arg("loop") = false,
      "Play a local H.264 MP4/H.264 file.")
      .def("pause", &tdl_app::VideoPlayer::pause)
      .def("resume", &tdl_app::VideoPlayer::resume)
      .def("stop", &tdl_app::VideoPlayer::stop)
      .def("close", &tdl_app::VideoPlayer::close)
      .def("set_volume", [](tdl_app::VideoPlayer &self, int volume_level) {
        std::string error;
        if (!self.setVolume(volume_level, &error)) {
          throw std::runtime_error("video player volume failed: " + error);
        }
      }, nb::arg("volume_level"),
      "Set CV184X speaker level. Values are clamped to [0, 32].")
      .def_prop_ro("state", &tdl_app::VideoPlayer::state)
      .def_prop_ro("last_error", &tdl_app::VideoPlayer::lastError)
      .def_prop_ro("playing", &tdl_app::VideoPlayer::isPlaying)
      .def_prop_ro("width", &tdl_app::VideoPlayer::width)
      .def_prop_ro("height", &tdl_app::VideoPlayer::height)
      .def_prop_ro("duration_ms", &tdl_app::VideoPlayer::durationMs)
      .def_prop_ro("position_ms", &tdl_app::VideoPlayer::positionMs)
      .def_prop_ro("has_audio", &tdl_app::VideoPlayer::hasAudio)
      .def_prop_ro("volume", &tdl_app::VideoPlayer::volume);
}

void registerMediaBindings(nb::module_ &m) {
  registerAudioBindings(m);
  registerVideoBindings(m);
}
