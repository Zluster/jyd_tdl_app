#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/audio_types.hpp"

namespace tdl_app {

struct SpeechRecognitionResult {
  std::string text;

  void clear() { text.clear(); }
  bool empty() const { return text.empty(); }
};

class SpeechRecognizer {
 public:
  using Config = ModelSessionConfig;

  SpeechRecognizer();
  ~SpeechRecognizer();

  SpeechRecognizer(const SpeechRecognizer &) = delete;
  SpeechRecognizer &operator=(const SpeechRecognizer &) = delete;
  SpeechRecognizer(SpeechRecognizer &&) noexcept;
  SpeechRecognizer &operator=(SpeechRecognizer &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::vector<std::uint8_t> &pcm16le_mono,
           SpeechRecognitionResult *result, std::string *error = nullptr);
  bool run(const AudioFrame &frame, SpeechRecognitionResult *result,
           std::string *error = nullptr);

  bool initialized() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
