#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tdl_app {

struct DirectKeywordResult {
  std::string name;
  float confidence = 0.0f;
  float threshold = 0.0f;
  int matched_tokens = 0;
  int total_tokens = 0;
  std::string matched_text;
  bool complete = false;
  bool triggered = false;
};

// Streaming KWS over signed 16-bit, mono, 16 kHz PCM. The implementation is
// CPU Fbank plus direct CV184X BMRT encoder/decoder/joiner bmodels only.
class DirectKeywordSpotter {
 public:
  DirectKeywordSpotter();
  ~DirectKeywordSpotter();

  DirectKeywordSpotter(const DirectKeywordSpotter &) = delete;
  DirectKeywordSpotter &operator=(const DirectKeywordSpotter &) = delete;

  bool load(const std::string &model_spec, const std::string &keywords_path,
            const std::string &firmware = "", float threshold_override = -1.0f,
            int beam_width = 6, std::string *error = nullptr);
  bool accept(const std::vector<std::int16_t> &pcm16le_mono,
              std::vector<DirectKeywordResult> *hits,
              std::string *error = nullptr);
  bool finish(std::vector<DirectKeywordResult> *hits,
              std::string *error = nullptr);
  std::vector<DirectKeywordResult> scores() const;
  bool initialized() const;
  void reset();

 private:
  class Impl;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
