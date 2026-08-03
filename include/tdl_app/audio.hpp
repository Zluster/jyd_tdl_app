#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/audio_decoder.hpp"
#include "tdl_app/audio_encoder.hpp"
#include "tdl_app/audio_input.hpp"
#include "tdl_app/audio_output.hpp"
#include "tdl_app/audio_types.hpp"

namespace tdl_app {

struct AudioStatus {
  bool runtime_ready = true;
  bool input_stream_open = false;
  bool output_stream_open = false;
  bool session_open = false;
  bool talk_vqe_supported = true;
  bool encoder_supported = true;
  bool decoder_supported = true;
  int ai_device = 0;
  int ai_channel = 0;
  int ao_device = 0;
  int ao_channel = 0;
  int sample_rate = 16000;
  int channels = 1;
  int bit_depth = 16;
  std::string note;
};

struct AudioIoConfig {
  int sample_rate = 16000;
  int channels = 1;
  int bit_depth = 16;
  int points_per_frame = 160;
  int frame_count = 8;
  int frame_depth = 8;
  int ai_device = 0;
  int ai_channel = 0;
  int ao_device = 0;
  int ao_channel = 0;
  int ai_card_id = -1;
  int ao_card_id = -1;
  int ai_volume = 24;
  int ao_volume = 24;
  int timeout_ms = 1000;
};

struct AudioPcmChunk {
  int sample_rate = 16000;
  int channels = 1;
  int bit_depth = 16;
  std::uint64_t timestamp = 0;
  std::uint32_t sequence = 0;
  std::vector<std::uint8_t> data;

  bool empty() const { return data.empty(); }
  std::size_t size() const { return data.size(); }
};

struct AudioInputStreamConfig {
  AudioIoConfig io;
  bool enable_talk_vqe = false;
  AudioTalkVqeConfig talk_vqe = AudioTalkVqeConfig::talk3a();
  int reference_output_device = 0;
  int reference_output_channel = 0;
};

struct AudioInputStreamStatus {
  bool opened = false;
  bool talk_vqe_enabled = false;
  std::uint32_t sequence = 0;
  AudioInputStreamConfig config;
};

struct AudioOutputStreamStatus {
  bool opened = false;
  std::uint32_t sequence = 0;
  AudioIoConfig config;
};

struct AudioSessionConfig {
  AudioIoConfig io;
  bool enable_input = true;
  bool enable_output = true;
  bool enable_encoder = false;
  bool enable_decoder = false;
  bool enable_talk_vqe = false;
  AudioTalkVqeConfig talk_vqe = AudioTalkVqeConfig::talk3a();
  int reference_output_device = 0;
  int reference_output_channel = 0;
  AudioEncoder::Config encoder = AudioEncoder::g711a();
  AudioDecoder::Config decoder = AudioDecoder::g711a();
};

struct AudioSessionStatus {
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
};

class Audio {
 public:
  AudioStatus status() const;

  bool recordWav(const std::string &path, double seconds = 3.0,
                 const AudioIoConfig &config = AudioIoConfig{},
                 std::string *error = nullptr) const;
  bool playWav(const std::string &path,
               const AudioIoConfig &config = AudioIoConfig{},
               std::string *error = nullptr) const;
  bool loopback(double seconds = 3.0,
                const AudioSessionConfig &config = AudioSessionConfig{},
                std::string *error = nullptr) const;

  bool getInputVolume(int *volume,
                      const AudioIoConfig &config = AudioIoConfig{},
                      std::string *error = nullptr) const;
  bool setInputVolume(int volume,
                      const AudioIoConfig &config = AudioIoConfig{},
                      std::string *error = nullptr) const;
  bool getOutputVolume(int *volume,
                       const AudioIoConfig &config = AudioIoConfig{},
                       std::string *error = nullptr) const;
  bool setOutputVolume(int volume,
                       const AudioIoConfig &config = AudioIoConfig{},
                       std::string *error = nullptr) const;

  bool openInputStream(const AudioInputStreamConfig &config,
                       std::string *error = nullptr) const;
  bool closeInputStream(std::string *error = nullptr) const;
  bool inputStreamStatus(AudioInputStreamStatus *status,
                         std::string *error = nullptr) const;
  bool readInputChunk(AudioPcmChunk *chunk,
                      std::string *error = nullptr) const;

  bool openOutputStream(const AudioIoConfig &config,
                        std::string *error = nullptr) const;
  bool closeOutputStream(std::string *error = nullptr) const;
  bool outputStreamStatus(AudioOutputStreamStatus *status,
                          std::string *error = nullptr) const;
  bool writeOutputChunk(const AudioPcmChunk &chunk,
                        std::string *error = nullptr) const;

  bool openSession(const AudioSessionConfig &config,
                   std::string *error = nullptr) const;
  bool closeSession(std::string *error = nullptr) const;
  bool sessionStatus(AudioSessionStatus *status,
                     std::string *error = nullptr) const;
  bool readEncodedChunk(AudioEncodedStream *chunk,
                        std::string *error = nullptr) const;
  bool writeDecodedChunk(const AudioEncodedStream &chunk,
                         std::string *error = nullptr) const;
};

}  // namespace tdl_app

