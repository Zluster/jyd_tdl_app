#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tdl_app {

enum class AudioSampleRate {
  Hz8000 = 8000,
  Hz11025 = 11025,
  Hz16000 = 16000,
  Hz22050 = 22050,
  Hz24000 = 24000,
  Hz32000 = 32000,
  Hz44100 = 44100,
  Hz48000 = 48000,
  Hz64000 = 64000,
};

enum class AudioBitWidth {
  Bits8 = 8,
  Bits16 = 16,
  Bits24 = 24,
  Bits32 = 32,
};

enum class AudioWorkMode {
  I2sMaster = 0,
  I2sSlave = 1,
  PcmSlaveStd = 2,
  PcmSlaveNonStd = 3,
  PcmMasterStd = 4,
  PcmMasterNonStd = 5,
};

enum class AudioI2sType {
  InnerCodec = 0,
  InnerHdmi = 1,
  External = 2,
};

enum class AudioSoundMode {
  Mono = 0,
  Stereo = 1,
};

enum class AudioTrackMode {
  Normal = 0,
  BothLeft = 1,
  BothRight = 2,
  Exchange = 3,
  Mix = 4,
  LeftMute = 5,
  RightMute = 6,
  BothMute = 7,
};

enum class AudioFadeRate {
  None = 0,
  Rate10 = 10,
  Rate20 = 20,
  Rate30 = 30,
  Rate50 = 50,
  Rate100 = 100,
  Rate200 = 200,
};

enum class AudioPayloadType {
  G711A = 19,
  G711U = 20,
  G726 = 21,
  AdpcmA = 49,
};

enum class AudioG726Bitrate {
  Kbps16 = 0,
  Kbps24 = 1,
  Kbps32 = 2,
  Kbps40 = 3,
};

enum class AudioAdpcmType {
  Dvi4 = 0,
  Ima = 1,
  OriginalDvi4 = 2,
};

enum class AudioDecodeMode {
  Pack = 0,
  Stream = 1,
};

struct AudioFade {
  bool enabled = false;
  AudioFadeRate fade_in = AudioFadeRate::None;
  AudioFadeRate fade_out = AudioFadeRate::None;
};

struct AudioAecConfig {
  std::uint16_t filter_length = 13;
  std::uint16_t std_threshold = 37;
  std::uint16_t suppress_coeff = 60;
};

struct AudioDelayConfig {
  std::uint16_t initial_filter_length = 0;
  std::uint16_t digital_gain_target = 0;
  std::uint16_t delay_sample = 0;
};

struct AudioAgcConfig {
  std::int8_t max_gain = 0;
  std::int8_t target_high = 2;
  std::int8_t target_low = 72;
  bool vad_enabled = true;
};

struct AudioAnrConfig {
  std::uint16_t snr_coeff = 15;
  std::uint16_t initial_silence_time = 0;
};

struct AudioTalkVqeConfig {
  std::uint16_t client_config = 0;
  std::uint32_t open_mask = 0;
  int work_sample_rate = 16000;
  AudioAecConfig aec;
  AudioAnrConfig anr;
  AudioAgcConfig agc;
  AudioDelayConfig delay;
  int notch_frequency = 0;

  static AudioTalkVqeConfig talk3a(int sample_rate = 16000) {
    AudioTalkVqeConfig config;
    config.work_sample_rate = sample_rate;
    config.open_mask = 0x1 | 0x2 | 0x4 | 0x8;
    return config;
  }

  static AudioTalkVqeConfig agcAnr(int sample_rate = 16000) {
    AudioTalkVqeConfig config;
    config.work_sample_rate = sample_rate;
    config.open_mask = 0x4 | 0x8 | 0x20;
    return config;
  }
};

struct AudioHpfConfig {
  int type = 1;
  float f0 = 70.0f;
  float q = 0.707f;
  float gain_db = 0.0f;
};

struct AudioEqBandConfig {
  int band_index = 0;
  std::uint32_t frequency = 0;
  float q_value = 0.0f;
  float gain_db = 0.0f;
};

struct AudioDrcCompressorConfig {
  std::uint32_t attack_time_ms = 0;
  std::uint32_t release_time_ms = 0;
  std::uint16_t ratio = 0;
  float threshold_db = 0.0f;
};

struct AudioDrcLimiterConfig {
  std::uint32_t attack_time_ms = 0;
  std::uint32_t release_time_ms = 0;
  float threshold_db = 0.0f;
  float post_gain_db = 0.0f;
};

struct AudioDrcExpanderConfig {
  std::uint32_t attack_time_ms = 0;
  std::uint32_t release_time_ms = 0;
  std::uint32_t hold_time_ms = 0;
  std::uint16_t ratio = 0;
  float threshold_db = 0.0f;
  float min_db = 0.0f;
};

struct AudioOutputVqeConfig {
  std::uint32_t open_mask = 0;
  int work_sample_rate = 16000;
  int channels = 1;
  AudioHpfConfig hpf;
  AudioEqBandConfig eq;
  AudioDrcCompressorConfig drc_compressor;
  AudioDrcLimiterConfig drc_limiter;
  AudioDrcExpanderConfig drc_expander;

  static AudioOutputVqeConfig speakerEnhance(int sample_rate = 16000,
                                             int channel_count = 1) {
    AudioOutputVqeConfig config;
    config.open_mask = 0x1 | 0x2 | 0x4 | 0x8;
    config.work_sample_rate = sample_rate;
    config.channels = channel_count;
    config.hpf.f0 = 70.0f;
    config.eq.band_index = 5;
    config.eq.frequency = 150;
    config.eq.q_value = 0.707f;
    config.eq.gain_db = 6.0f;
    config.drc_limiter.attack_time_ms = 10;
    config.drc_limiter.release_time_ms = 100;
    config.drc_limiter.threshold_db = -2.0f;
    config.drc_limiter.post_gain_db = 4.0f;
    config.drc_compressor.attack_time_ms = 5;
    config.drc_compressor.release_time_ms = 200;
    config.drc_compressor.threshold_db = -2.0f;
    config.drc_compressor.ratio = 10;
    config.drc_expander.attack_time_ms = 10;
    config.drc_expander.release_time_ms = 200;
    config.drc_expander.hold_time_ms = 20;
    config.drc_expander.threshold_db = -20.0f;
    config.drc_expander.min_db = -60.0f;
    config.drc_expander.ratio = 5;
    return config;
  }
};

struct AudioFrame {
  AudioBitWidth bit_width = AudioBitWidth::Bits16;
  AudioSoundMode sound_mode = AudioSoundMode::Mono;
  std::uint64_t timestamp = 0;
  std::uint32_t sequence = 0;
  std::uint32_t bytes_per_channel = 0;
  std::vector<std::vector<std::uint8_t>> channels;

  bool empty() const { return channels.empty(); }
  std::size_t channelCount() const { return channels.size(); }
  std::size_t bytes() const {
    return channels.empty() ? 0 : channels.front().size();
  }
};

struct AudioEncodedStream {
  AudioPayloadType payload_type = AudioPayloadType::G711A;
  std::uint64_t timestamp = 0;
  std::uint32_t sequence = 0;
  std::vector<std::uint8_t> data;

  bool empty() const { return data.empty(); }
  std::size_t size() const { return data.size(); }
};

}  // namespace tdl_app
