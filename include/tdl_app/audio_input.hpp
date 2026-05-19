#pragma once

#include <cstdint>
#include <string>

#include "tdl_app/audio_types.hpp"

namespace tdl_app {

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
  explicit AudioInput(const Config &config);
  ~AudioInput();

  AudioInput(const AudioInput &) = delete;
  AudioInput &operator=(const AudioInput &) = delete;

  static Config mono16k(int device = 0, int channel = 0, int card_id = -1);

  bool open(std::string *error = nullptr);
  bool readFrame(AudioFrame *frame, int timeout_ms = 1000,
                 std::string *error = nullptr);
  bool setVolume(int volume_step, std::string *error = nullptr);
  bool getVolume(int *volume_step, std::string *error = nullptr) const;
  bool setTrackMode(AudioTrackMode mode, std::string *error = nullptr);
  bool getTrackMode(AudioTrackMode *mode, std::string *error = nullptr) const;
  bool enableResample(AudioSampleRate output_sample_rate,
                      std::string *error = nullptr);
  bool disableResample(std::string *error = nullptr);
  bool enableVqe(std::string *error = nullptr);
  bool disableVqe(std::string *error = nullptr);
  bool setVqeMask(std::uint32_t mask, std::string *error = nullptr);
  bool configureTalkVqe(const AudioTalkVqeConfig &config, int output_device = 0,
                        int output_channel = 0,
                        std::string *error = nullptr);
  bool queryTalkVqe(AudioTalkVqeConfig *config,
                    std::string *error = nullptr) const;
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace tdl_app
