#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

struct KeywordEvent {
  std::string text;
  std::vector<std::string> tokens;
  float confidence = 0.0f;
};

struct KeywordScore {
  std::string text;
  float probability = 0.0f;
};

// Streaming Chinese keyword spotter backed by three CV184X BM Runtime bmodels.
// Input is signed 16-bit, mono, 16 kHz PCM.
class NpuKeywordSpotter {
 public:
  using Config = ModelSessionConfig;

  NpuKeywordSpotter();
  ~NpuKeywordSpotter();
  NpuKeywordSpotter(const NpuKeywordSpotter &) = delete;
  NpuKeywordSpotter &operator=(const NpuKeywordSpotter &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool loadKeywords(const std::string &path, std::string *error = nullptr);
  bool acceptPcm(const std::vector<std::int16_t> &pcm16le_mono,
                 std::vector<KeywordEvent> *events = nullptr,
                 std::string *error = nullptr);
  bool acceptPcm(const std::vector<std::int16_t> &pcm16le_mono,
                 std::vector<KeywordEvent> *events,
                 std::vector<KeywordScore> *scores,
                 std::string *error = nullptr);
  // Flushes the final 0.8 seconds required by the streaming decoder and
  // starts a fresh stream. Realtime callers normally do not need this.
  bool finish(std::vector<KeywordEvent> *events = nullptr,
              std::string *error = nullptr);
  bool initialized() const;
  void reset();

 private:
  class Impl;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
