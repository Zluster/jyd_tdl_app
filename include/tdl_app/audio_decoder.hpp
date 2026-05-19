#pragma once

#include <string>

#include "tdl_app/audio_types.hpp"

namespace tdl_app {

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
  explicit AudioDecoder(const Config &config);
  ~AudioDecoder();

  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  static Config g711a(int channel = 0, int sample_rate = 16000);
  static Config g711u(int channel = 0, int sample_rate = 16000);
  static Config g726(int channel = 0, int sample_rate = 16000,
                     AudioG726Bitrate bitrate = AudioG726Bitrate::Kbps32);

  bool open(std::string *error = nullptr);
  bool decodeStream(const AudioEncodedStream &stream, AudioFrame *frame,
                    bool block = true, std::string *error = nullptr);
  bool clearBuffer(std::string *error = nullptr);
  bool sendEndOfStream(bool instant = true, std::string *error = nullptr);
  void close();
  bool isOpen() const;
  const Config &config() const { return config_; }

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace tdl_app
