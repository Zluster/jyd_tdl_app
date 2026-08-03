#ifndef MMF_CVI_AUDIO_HPP
#define MMF_CVI_AUDIO_HPP

#include "mmf_cvi_base.hpp"

namespace mmf_cvi {

enum class AudioSampleRate {
  Hz8000 = 8000,
  Hz11025 = 11025,
  Hz16000 = 16000,
  Hz22050 = 22050,
  Hz24000 = 24000,
  Hz32000 = 32000,
  Hz44100 = 44100,
  Hz48000 = 48000,
  Hz64000 = 64000
};
enum class AudioBitWidth { Bits8 = 8, Bits16 = 16, Bits24 = 24, Bits32 = 32 };
enum class AudioWorkMode { I2sMaster = 0 };
enum class AudioI2sType { InnerCodec = 0 };
enum class AudioSoundMode { Mono = 0, Stereo = 1 };
enum class AudioPayloadType { G711A = 19, G711U = 20, G726 = 21, AdpcmA = 49 };
enum class AudioG726Bitrate { Kbps16 = 0, Kbps24 = 1, Kbps32 = 2, Kbps40 = 3 };
enum class AudioAdpcmType { Dvi4 = 0, Ima = 1, OriginalDvi4 = 2 };
enum class AudioDecodeMode { Pack = 0, Stream = 1 };
struct AudioAecConfig {
  uint16_t filter_length = 13;
  uint16_t std_threshold = 37;
  uint16_t suppress_coeff = 60;
};
struct AudioDelayConfig {
  uint16_t initial_filter_length = 0;
  uint16_t digital_gain_target = 0;
  uint16_t delay_sample = 0;
};
struct AudioAgcConfig {
  int8_t max_gain = 0;
  int8_t target_high = 2;
  int8_t target_low = 72;
  bool vad_enabled = true;
};
struct AudioAnrConfig {
  uint16_t snr_coeff = 15;
  uint16_t initial_silence_time = 0;
};
struct AudioTalkVqeConfig {
  uint16_t client_config = 0;
  uint32_t open_mask = 0;
  int work_sample_rate = 16000;
  AudioAecConfig aec;
  AudioAnrConfig anr;
  AudioAgcConfig agc;
  AudioDelayConfig delay;
  int notch_frequency = 0;
  static AudioTalkVqeConfig talk3a(int sample_rate = 16000) {
    AudioTalkVqeConfig c;
    c.work_sample_rate = sample_rate;
    c.open_mask = 0x1 | 0x2 | 0x4 | 0x8;
    return c;
  }
};
struct AudioFrame {
  AudioBitWidth bit_width = AudioBitWidth::Bits16;
  AudioSoundMode sound_mode = AudioSoundMode::Mono;
  uint64_t timestamp = 0;
  uint32_t sequence = 0;
  uint32_t bytes_per_channel = 0;
  std::vector<std::vector<uint8_t>> channels;
  bool empty() const {
    return channels.empty();
  }
};
struct AudioEncodedStream {
  AudioPayloadType payload_type = AudioPayloadType::G711A;
  uint64_t timestamp = 0;
  uint32_t sequence = 0;
  std::vector<uint8_t> data;
  bool empty() const {
    return data.empty();
  }
};
bool retainAudioRuntime(std::string* error = nullptr);
void releaseAudioRuntime();
class AudioInput {
 public:
  struct Config {
    int device = 0;
    int channel = 0;
    int card_id = -1;
    AudioSampleRate sample_rate = AudioSampleRate::Hz16000;
    AudioBitWidth bit_width = AudioBitWidth::Bits16;
    AudioWorkMode work_mode = AudioWorkMode::I2sMaster;
    AudioI2sType i2s_type = AudioI2sType::InnerCodec;
    AudioSoundMode sound_mode = AudioSoundMode::Mono;
    int frame_count = 8;
    int points_per_frame = 160;
    int channel_count = 1;
    int frame_depth = 8;
    int clock_select = 0;
    int volume_step = 0;
  };
  AudioInput();
  explicit AudioInput(const Config& config);
  ~AudioInput();
  bool open(std::string* error = nullptr);
  bool readFrame(AudioFrame* frame, int timeout_ms = 1000, std::string* error = nullptr);
  bool setVolume(int volume_step, std::string* error = nullptr);
  bool getVolume(int* volume_step, std::string* error = nullptr) const;
  bool configureTalkVqe(const AudioTalkVqeConfig& config, int output_device = 0,
                        int output_channel = 0, std::string* error = nullptr);
  void close();

 private:
  Config config_;
  bool opened_ = false;
};
class AudioOutput {
 public:
  struct Config {
    int device = 0;
    int channel = 0;
    int card_id = -1;
    AudioSampleRate sample_rate = AudioSampleRate::Hz16000;
    AudioBitWidth bit_width = AudioBitWidth::Bits16;
    AudioWorkMode work_mode = AudioWorkMode::I2sMaster;
    AudioI2sType i2s_type = AudioI2sType::InnerCodec;
    AudioSoundMode sound_mode = AudioSoundMode::Mono;
    int frame_count = 8;
    int points_per_frame = 160;
    int channel_count = 1;
    int clock_select = 0;
    int volume_db = 0;
  };
  AudioOutput();
  explicit AudioOutput(const Config& config);
  ~AudioOutput();
  bool open(std::string* error = nullptr);
  bool writeFrame(const AudioFrame& frame, int timeout_ms = 1000, std::string* error = nullptr);
  bool setVolume(int volume_db, std::string* error = nullptr);
  bool getVolume(int* volume_db, std::string* error = nullptr) const;
  void close();

 private:
  Config config_;
  bool opened_ = false;
};
class AudioEncoder {
 public:
  struct Config {
    int channel = 0;
    AudioPayloadType payload_type = AudioPayloadType::G711A;
    int points_per_frame = 160;
    int buffer_size = 30;
    AudioG726Bitrate g726_bitrate = AudioG726Bitrate::Kbps32;
    AudioAdpcmType adpcm_type = AudioAdpcmType::Dvi4;
    bool file_debug_mode = false;
  };
  AudioEncoder();
  explicit AudioEncoder(const Config& config);
  ~AudioEncoder();
  bool open(std::string* error = nullptr);
  bool encodeFrame(const AudioFrame& frame, AudioEncodedStream* stream,
                   std::string* error = nullptr);
  void close();

 private:
  Config config_;
  bool opened_ = false;
};
class AudioDecoder {
 public:
  struct Config {
    int channel = 0;
    AudioPayloadType payload_type = AudioPayloadType::G711A;
    AudioDecodeMode decode_mode = AudioDecodeMode::Pack;
    int buffer_size = 20;
    int bytes_per_sample = 2;
    int frame_size = 160;
    int channel_count = 1;
    int sample_rate = 16000;
    AudioG726Bitrate g726_bitrate = AudioG726Bitrate::Kbps32;
    AudioAdpcmType adpcm_type = AudioAdpcmType::Dvi4;
    bool file_debug_mode = false;
  };
  AudioDecoder();
  explicit AudioDecoder(const Config& config);
  ~AudioDecoder();
  bool open(std::string* error = nullptr);
  bool decodeStream(const AudioEncodedStream& stream, AudioFrame* frame, bool block = true,
                    std::string* error = nullptr);
  void close();

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace mmf_cvi

#endif  // MMF_CVI_AUDIO_HPP
