#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

// Streaming Chinese Zipformer ASR backed directly by CV184X BM Runtime.
// Input is signed 16-bit, mono, 16 kHz PCM.
class NpuStreamingAsr {
 public:
  using Config = ModelSessionConfig;

  NpuStreamingAsr();
  ~NpuStreamingAsr();

  NpuStreamingAsr(const NpuStreamingAsr &) = delete;
  NpuStreamingAsr &operator=(const NpuStreamingAsr &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);

  // Appends PCM to the recognizer. newly_decoded contains only text generated
  // by this call; text() contains the full result since load().
  bool acceptPcm(const std::vector<std::int16_t> &pcm16le_mono,
                 std::string *newly_decoded = nullptr,
                 std::string *error = nullptr);

  // Flushes the final encoder chunk by appending model-required silence.
  bool finish(std::string *newly_decoded = nullptr,
              std::string *error = nullptr);

  bool initialized() const;
  const std::string &text() const;
  void reset();

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
