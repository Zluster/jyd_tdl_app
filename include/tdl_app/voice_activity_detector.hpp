#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/audio_types.hpp"

namespace tdl_app {

struct VoiceSegment {
  std::int32_t start_ms = 0;
  std::int32_t end_ms = -1;
};

struct VoiceActivityResult {
  std::vector<VoiceSegment> segments;
  bool has_speech = false;
  bool start_event = false;
  bool end_event = false;

  void clear() {
    segments.clear();
    has_speech = false;
    start_event = false;
    end_event = false;
  }

  bool empty() const { return segments.empty() && !has_speech; }
  int segmentCount() const { return static_cast<int>(segments.size()); }
};

class VoiceActivityDetector {
 public:
  using Config = ModelSessionConfig;

  VoiceActivityDetector();
  ~VoiceActivityDetector();

  VoiceActivityDetector(const VoiceActivityDetector &) = delete;
  VoiceActivityDetector &operator=(const VoiceActivityDetector &) = delete;
  VoiceActivityDetector(VoiceActivityDetector &&) noexcept;
  VoiceActivityDetector &operator=(VoiceActivityDetector &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error);
  bool load(const std::string &model_spec, const std::string &firmware,
            const std::string &model_dir, std::string *error);

  bool run(const std::vector<std::uint8_t> &pcm16le_mono, bool is_final,
           VoiceActivityResult *result, std::string *error = nullptr);
  bool run(const AudioFrame &frame, bool is_final, VoiceActivityResult *result,
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
