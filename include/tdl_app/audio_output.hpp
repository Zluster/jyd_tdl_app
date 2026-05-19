#pragma once

#include <string>

#include "tdl_app/audio_types.hpp"

namespace tdl_app {

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

  struct ChannelState {
    unsigned int total = 0;
    unsigned int free = 0;
    unsigned int busy = 0;
  };

  AudioOutput();
  explicit AudioOutput(const Config &config);
  ~AudioOutput();

  AudioOutput(const AudioOutput &) = delete;
  AudioOutput &operator=(const AudioOutput &) = delete;

  static Config mono16k(int device = 0, int channel = 0, int card_id = -1);

  bool open(std::string *error = nullptr);
  bool writeFrame(const AudioFrame &frame, int timeout_ms = 1000,
                  std::string *error = nullptr);
  bool setVolume(int volume_db, std::string *error = nullptr);
  bool getVolume(int *volume_db, std::string *error = nullptr) const;
  bool setTrackMode(AudioTrackMode mode, std::string *error = nullptr);
  bool getTrackMode(AudioTrackMode *mode, std::string *error = nullptr) const;
  bool setMute(bool enabled, const AudioFade &fade = AudioFade{},
               std::string *error = nullptr);
  bool getMute(bool *enabled, AudioFade *fade = nullptr,
               std::string *error = nullptr) const;
  bool setVqeConfig(const AudioOutputVqeConfig &config,
                    std::string *error = nullptr);
  bool getVqeConfig(AudioOutputVqeConfig *config,
                    std::string *error = nullptr) const;
  bool enableVqe(std::string *error = nullptr);
  bool disableVqe(std::string *error = nullptr);
  bool enableResample(AudioSampleRate input_sample_rate,
                      std::string *error = nullptr);
  bool disableResample(std::string *error = nullptr);
  bool clearBuffer(std::string *error = nullptr);
  bool pause(std::string *error = nullptr);
  bool resume(std::string *error = nullptr);
  bool queryState(ChannelState *state, std::string *error = nullptr) const;
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace tdl_app
