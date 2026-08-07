#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

struct SpeakerEmbedding {
  std::vector<float> values;

  bool empty() const { return values.empty(); }
};

struct SpeakerMatch {
  std::string label;
  float score = 0.0f;
  bool matched = false;
};

// Persistent, label-to-embedding store used by speaker enrollment and lookup.
class SpeakerDatabase {
 public:
  bool load(const std::string &path, std::string *error = nullptr);
  bool save(const std::string &path, std::string *error = nullptr) const;
  bool upsert(const std::string &label, const SpeakerEmbedding &embedding,
              std::string *error = nullptr);
  bool find(const std::string &label, SpeakerEmbedding *embedding) const;
  std::vector<std::string> labels() const;
  std::size_t size() const;
  void clear();

 private:
  struct Entry {
    std::string label;
    SpeakerEmbedding embedding;
  };
  std::vector<Entry> entries_;
};

class SpeakerRecognizer {
 public:
  using Config = ModelSessionConfig;

  SpeakerRecognizer();
  ~SpeakerRecognizer();

  SpeakerRecognizer(const SpeakerRecognizer &) = delete;
  SpeakerRecognizer &operator=(const SpeakerRecognizer &) = delete;
  SpeakerRecognizer(SpeakerRecognizer &&) noexcept;
  SpeakerRecognizer &operator=(SpeakerRecognizer &&) noexcept;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool extract(const std::vector<std::int16_t> &pcm16le_mono,
               SpeakerEmbedding *embedding, std::string *error = nullptr);
  bool extract(const std::vector<std::uint8_t> &pcm16le_mono,
               SpeakerEmbedding *embedding, std::string *error = nullptr);
  SpeakerMatch identify(const SpeakerEmbedding &embedding,
                        const SpeakerDatabase &database, float threshold) const;
  SpeakerMatch verify(const std::string &label, const SpeakerEmbedding &embedding,
                      const SpeakerDatabase &database, float threshold) const;

  bool initialized() const;
  int inputFrames() const;
  int embeddingDim() const;
  const Config &config() const { return config_; }
  void reset();

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
