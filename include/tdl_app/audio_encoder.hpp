#pragma once

#include <string>

#include "tdl_app/audio_types.hpp"

namespace tdl_app {

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
  explicit AudioEncoder(const Config &config);
  ~AudioEncoder();

  AudioEncoder(const AudioEncoder &) = delete;
  AudioEncoder &operator=(const AudioEncoder &) = delete;

  static Config g711a(int channel = 0, int points_per_frame = 160);
  static Config g711u(int channel = 0, int points_per_frame = 160);
  static Config g726(int channel = 0, int points_per_frame = 160,
                     AudioG726Bitrate bitrate = AudioG726Bitrate::Kbps32);

  bool open(std::string *error = nullptr);
  bool encodeFrame(const AudioFrame &frame, AudioEncodedStream *stream,
                   std::string *error = nullptr);
  void close();
  bool isOpen() const;
  const Config &config() const { return config_; }

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace tdl_app
