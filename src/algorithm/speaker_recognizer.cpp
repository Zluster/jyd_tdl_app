#include "tdl_app/speaker_recognizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr char kDatabaseMagic[] = "JYDSPK01";
constexpr std::uint32_t kEmbeddingDim = 192;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

template <typename T>
bool readValue(std::istream *input, T *value) {
  input->read(reinterpret_cast<char *>(value), sizeof(*value));
  return static_cast<bool>(*input);
}

template <typename T>
bool writeValue(std::ostream *output, const T &value) {
  output->write(reinterpret_cast<const char *>(&value), sizeof(value));
  return static_cast<bool>(*output);
}

float cosineSimilarity(const SpeakerEmbedding &lhs, const SpeakerEmbedding &rhs) {
  if (lhs.values.size() != rhs.values.size() || lhs.values.empty()) {
    return -1.0f;
  }
  double dot = 0.0;
  double lhs_norm = 0.0;
  double rhs_norm = 0.0;
  for (std::size_t i = 0; i < lhs.values.size(); ++i) {
    dot += static_cast<double>(lhs.values[i]) * rhs.values[i];
    lhs_norm += static_cast<double>(lhs.values[i]) * lhs.values[i];
    rhs_norm += static_cast<double>(rhs.values[i]) * rhs.values[i];
  }
  if (lhs_norm <= std::numeric_limits<double>::epsilon() ||
      rhs_norm <= std::numeric_limits<double>::epsilon()) {
    return -1.0f;
  }
  return static_cast<float>(dot / std::sqrt(lhs_norm * rhs_norm));
}

std::size_t shapeElements(const bm_shape_t &shape) {
  std::size_t count = 1;
  for (int i = 0; i < shape.num_dims; ++i) {
    if (shape.dims[i] <= 0) {
      return 0;
    }
    count *= static_cast<std::size_t>(shape.dims[i]);
  }
  return count;
}

}  // namespace

bool SpeakerDatabase::load(const std::string &path, std::string *error) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    setError(error, "cannot open speaker database: " + path);
    return false;
  }

  char magic[sizeof(kDatabaseMagic)] = {};
  input.read(magic, sizeof(magic));
  if (!input || std::memcmp(magic, kDatabaseMagic, sizeof(magic)) != 0) {
    setError(error, "invalid speaker database format: " + path);
    return false;
  }

  std::uint32_t count = 0;
  if (!readValue(&input, &count) || count > 10000) {
    setError(error, "invalid speaker database entry count");
    return false;
  }

  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t label_size = 0;
    std::uint32_t embedding_dim = 0;
    if (!readValue(&input, &label_size) || label_size == 0 || label_size > 1024 ||
        !readValue(&input, &embedding_dim) || embedding_dim != kEmbeddingDim) {
      setError(error, "invalid speaker database entry");
      return false;
    }
    Entry entry;
    entry.label.resize(label_size);
    input.read(&entry.label[0], label_size);
    entry.embedding.values.resize(embedding_dim);
    input.read(reinterpret_cast<char *>(entry.embedding.values.data()),
               embedding_dim * sizeof(float));
    if (!input) {
      setError(error, "truncated speaker database");
      return false;
    }
    entries.push_back(std::move(entry));
  }
  entries_ = std::move(entries);
  return true;
}

bool SpeakerDatabase::save(const std::string &path, std::string *error) const {
  std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "cannot create speaker database: " + path);
    return false;
  }
  output.write(kDatabaseMagic, sizeof(kDatabaseMagic));
  const std::uint32_t count = static_cast<std::uint32_t>(entries_.size());
  if (!writeValue(&output, count)) {
    setError(error, "cannot write speaker database header");
    return false;
  }
  for (const Entry &entry : entries_) {
    const std::uint32_t label_size = static_cast<std::uint32_t>(entry.label.size());
    const std::uint32_t embedding_dim =
        static_cast<std::uint32_t>(entry.embedding.values.size());
    if (!writeValue(&output, label_size) || !writeValue(&output, embedding_dim)) {
      setError(error, "cannot write speaker database entry header");
      return false;
    }
    output.write(entry.label.data(), label_size);
    output.write(reinterpret_cast<const char *>(entry.embedding.values.data()),
                 embedding_dim * sizeof(float));
    if (!output) {
      setError(error, "cannot write speaker database entry");
      return false;
    }
  }
  return true;
}

bool SpeakerDatabase::upsert(const std::string &label,
                             const SpeakerEmbedding &embedding,
                             std::string *error) {
  if (label.empty() || label.size() > 1024) {
    setError(error, "speaker label must contain 1 to 1024 bytes");
    return false;
  }
  if (embedding.values.size() != kEmbeddingDim) {
    setError(error, "speaker embedding dimension must be 192");
    return false;
  }
  for (Entry &entry : entries_) {
    if (entry.label == label) {
      entry.embedding = embedding;
      return true;
    }
  }
  Entry entry;
  entry.label = label;
  entry.embedding = embedding;
  entries_.push_back(std::move(entry));
  return true;
}

bool SpeakerDatabase::find(const std::string &label, SpeakerEmbedding *embedding) const {
  if (!embedding) {
    return false;
  }
  for (const Entry &entry : entries_) {
    if (entry.label == label) {
      *embedding = entry.embedding;
      return true;
    }
  }
  return false;
}

std::vector<std::string> SpeakerDatabase::labels() const {
  std::vector<std::string> result;
  result.reserve(entries_.size());
  for (const Entry &entry : entries_) {
    result.push_back(entry.label);
  }
  return result;
}

std::size_t SpeakerDatabase::size() const { return entries_.size(); }

void SpeakerDatabase::clear() { entries_.clear(); }

class SpeakerRecognizer::Impl {
 public:
  ~Impl() { reset(); }

  bool load(const Config &config, std::string *error) {
    reset();
    if (config.model_spec.empty()) {
      setError(error, "speaker recognizer model_spec is empty");
      return false;
    }
    ModelDescriptor descriptor;
    if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
      return false;
    }
    if (!config.firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.firmware.c_str(), 0);
    }
    if (bm_dev_request(&handle_, 0) != BM_SUCCESS) {
      setError(error, "bm_dev_request failed");
      return false;
    }
    runtime_ = bmrt_create(handle_);
    const std::string model_path = resolveModelPath(descriptor);
    if (!runtime_ || !bmrt_load_bmodel(runtime_, model_path.c_str())) {
      setError(error, "failed to load speaker bmodel: " + model_path);
      reset();
      return false;
    }
    const char **names = nullptr;
    bmrt_get_network_names(runtime_, &names);
    if (!names || bmrt_get_network_number(runtime_) != 1) {
      if (names) {
        std::free(names);
      }
      setError(error, "speaker bmodel must contain exactly one network");
      reset();
      return false;
    }
    network_name_ = names[0];
    std::free(names);
    network_ = bmrt_get_network_info(runtime_, network_name_.c_str());
    if (!network_ || network_->is_dynamic || network_->stage_num != 1 ||
        network_->input_num != 1 || network_->output_num != 1 ||
        network_->input_dtypes[0] != BM_FLOAT32 ||
        network_->output_dtypes[0] != BM_FLOAT32) {
      setError(error, "speaker bmodel must be static float32 [1,T,80] -> [1,192]");
      reset();
      return false;
    }
    const bm_stage_info_t &stage = network_->stages[0];
    if (stage.input_shapes[0].num_dims != 3 || stage.input_shapes[0].dims[0] != 1 ||
        stage.input_shapes[0].dims[1] <= 0 || stage.input_shapes[0].dims[2] != 80 ||
        stage.output_shapes[0].num_dims != 2 || stage.output_shapes[0].dims[0] != 1 ||
        stage.output_shapes[0].dims[1] != static_cast<int>(kEmbeddingDim)) {
      setError(error, "speaker bmodel shape is not [1,T,80] -> [1,192]");
      reset();
      return false;
    }
    input_frames_ = stage.input_shapes[0].dims[1];
    return true;
  }

  bool extract(const std::vector<std::int16_t> &pcm, SpeakerEmbedding *embedding,
               std::string *error) const {
    if (!runtime_ || !network_) {
      setError(error, "speaker recognizer is not initialized");
      return false;
    }
    if (!embedding) {
      setError(error, "speaker embedding pointer is null");
      return false;
    }
    if (pcm.size() < 16000) {
      setError(error, "speaker audio must contain at least 1 second at 16 kHz");
      return false;
    }

    std::vector<float> waveform(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      waveform[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    knf::FbankOptions options;
    options.frame_opts.samp_freq = 16000;
    options.frame_opts.dither = 0.0f;
    options.frame_opts.snip_edges = false;
    options.frame_opts.frame_shift_ms = 10.0f;
    options.frame_opts.frame_length_ms = 25.0f;
    options.frame_opts.remove_dc_offset = true;
    options.frame_opts.preemph_coeff = 0.97f;
    options.frame_opts.window_type = "povey";
    options.frame_opts.round_to_power_of_two = true;
    options.mel_opts.num_bins = 80;
    options.mel_opts.low_freq = 20.0f;
    options.mel_opts.high_freq = -400.0f;
    options.mel_opts.is_librosa = false;
    knf::OnlineFbank fbank(options);
    fbank.AcceptWaveform(16000.0f, waveform.data(),
                         static_cast<std::int32_t>(waveform.size()));
    fbank.InputFinished();
    const int frame_count = fbank.NumFramesReady();
    if (frame_count < 1) {
      setError(error, "speaker feature extraction produced no frames");
      return false;
    }
    std::vector<float> features(static_cast<std::size_t>(frame_count) * 80);
    for (int frame = 0; frame < frame_count; ++frame) {
      std::memcpy(features.data() + static_cast<std::size_t>(frame) * 80,
                  fbank.GetFrame(frame), 80 * sizeof(float));
    }
    for (int dim = 0; dim < 80; ++dim) {
      double mean = 0.0;
      for (int frame = 0; frame < frame_count; ++frame) {
        mean += features[static_cast<std::size_t>(frame) * 80 + dim];
      }
      mean /= frame_count;
      for (int frame = 0; frame < frame_count; ++frame) {
        features[static_cast<std::size_t>(frame) * 80 + dim] -=
            static_cast<float>(mean);
      }
    }

    std::vector<int> starts(1, 0);
    if (frame_count > input_frames_) {
      starts.clear();
      for (int start = 0; start + input_frames_ < frame_count;
           start += input_frames_) {
        starts.push_back(start);
      }
      if (starts.back() != frame_count - input_frames_) {
        starts.push_back(frame_count - input_frames_);
      }
    }

    SpeakerEmbedding combined;
    combined.values.assign(kEmbeddingDim, 0.0f);
    const bm_stage_info_t &stage = network_->stages[0];
    for (int start : starts) {
      std::vector<float> input(static_cast<std::size_t>(input_frames_) * 80);
      const int available = std::min(input_frames_, frame_count - start);
      std::memcpy(input.data(), features.data() + static_cast<std::size_t>(start) * 80,
                  static_cast<std::size_t>(available) * 80 * sizeof(float));
      if (available < input_frames_) {
        const float *last = input.data() + static_cast<std::size_t>(available - 1) * 80;
        for (int frame = available; frame < input_frames_; ++frame) {
          std::memcpy(input.data() + static_cast<std::size_t>(frame) * 80,
                      last, 80 * sizeof(float));
        }
      }
      void *input_ptr = input.data();
      bm_shape_t input_shape = stage.input_shapes[0];
      std::vector<std::uint8_t> output_bytes(network_->max_output_bytes[0]);
      void *output_ptr = output_bytes.data();
      bm_shape_t output_shape{};
      if (!bmrt_launch_data(runtime_, network_name_.c_str(), &input_ptr,
                            &input_shape, 1, &output_ptr, &output_shape, 1,
                            true) ||
          shapeElements(output_shape) != kEmbeddingDim) {
        setError(error, "bmrt_launch_data failed for speaker bmodel");
        return false;
      }
      const float *output = static_cast<const float *>(output_ptr);
      for (std::size_t i = 0; i < combined.values.size(); ++i) {
        combined.values[i] += output[i];
      }
    }
    const float scale = 1.0f / static_cast<float>(starts.size());
    for (float &value : combined.values) {
      value *= scale;
    }
    *embedding = std::move(combined);
    return true;
  }

  void reset() {
    network_ = nullptr;
    network_name_.clear();
    input_frames_ = 0;
    if (runtime_) {
      bmrt_destroy(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bm_dev_free(handle_);
      handle_ = nullptr;
    }
  }

  bool initialized() const { return runtime_ != nullptr && network_ != nullptr; }
  int inputFrames() const { return input_frames_; }

 private:
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *network_ = nullptr;
  std::string network_name_;
  int input_frames_ = 0;
};

SpeakerRecognizer::SpeakerRecognizer() = default;

SpeakerRecognizer::~SpeakerRecognizer() {
  reset();
  delete impl_;
}

SpeakerRecognizer::SpeakerRecognizer(SpeakerRecognizer &&other) noexcept
    : config_(std::move(other.config_)), impl_(other.impl_) {
  other.impl_ = nullptr;
}

SpeakerRecognizer &SpeakerRecognizer::operator=(SpeakerRecognizer &&other) noexcept {
  if (this != &other) {
    reset();
    delete impl_;
    config_ = std::move(other.config_);
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

bool SpeakerRecognizer::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, error);
}

bool SpeakerRecognizer::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool SpeakerRecognizer::extract(const std::vector<std::int16_t> &pcm,
                                SpeakerEmbedding *embedding,
                                std::string *error) {
  if (!impl_) {
    setError(error, "speaker recognizer is not initialized");
    return false;
  }
  return impl_->extract(pcm, embedding, error);
}

bool SpeakerRecognizer::extract(const std::vector<std::uint8_t> &pcm,
                                SpeakerEmbedding *embedding,
                                std::string *error) {
  if (pcm.empty() || pcm.size() % sizeof(std::int16_t) != 0) {
    setError(error, "speaker PCM must be non-empty 16-bit samples");
    return false;
  }
  std::vector<std::int16_t> samples(pcm.size() / sizeof(std::int16_t));
  std::memcpy(samples.data(), pcm.data(), pcm.size());
  return extract(samples, embedding, error);
}

SpeakerMatch SpeakerRecognizer::identify(const SpeakerEmbedding &embedding,
                                         const SpeakerDatabase &database,
                                         float threshold) const {
  SpeakerMatch result;
  if (embedding.empty()) {
    return result;
  }
  result.score = -1.0f;
  for (const std::string &label : database.labels()) {
    SpeakerEmbedding candidate;
    if (!database.find(label, &candidate)) {
      continue;
    }
    const float score = cosineSimilarity(embedding, candidate);
    if (score > result.score) {
      result.label = label;
      result.score = score;
    }
  }
  result.matched = !result.label.empty() && result.score >= threshold;
  return result;
}

SpeakerMatch SpeakerRecognizer::verify(const std::string &label,
                                       const SpeakerEmbedding &embedding,
                                       const SpeakerDatabase &database,
                                       float threshold) const {
  SpeakerMatch result;
  result.label = label;
  SpeakerEmbedding enrolled;
  if (!database.find(label, &enrolled) || embedding.empty()) {
    return result;
  }
  result.score = cosineSimilarity(embedding, enrolled);
  result.matched = result.score >= threshold;
  return result;
}

bool SpeakerRecognizer::initialized() const { return impl_ && impl_->initialized(); }

int SpeakerRecognizer::inputFrames() const { return impl_ ? impl_->inputFrames() : 0; }

int SpeakerRecognizer::embeddingDim() const { return kEmbeddingDim; }

void SpeakerRecognizer::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
