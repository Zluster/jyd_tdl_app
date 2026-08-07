#include "tdl_app/npu_asr_recognizer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kFbankBins = 80;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

std::size_t shapeElements(const bm_shape_t &shape) {
  std::size_t count = 1;
  for (int i = 0; i < shape.num_dims; ++i) {
    if (shape.dims[i] <= 0) return 0;
    count *= static_cast<std::size_t>(shape.dims[i]);
  }
  return count;
}

std::string joinPath(const std::string &base, const std::string &path) {
  if (path.empty() || path.front() == '/' ||
      (path.size() > 1 && path[1] == ':')) return path;
  return base.empty() || base.back() == '/' ? base + path : base + "/" + path;
}

bool readTokens(const std::string &path, std::vector<std::string> *tokens,
                std::string *error) {
  std::ifstream input(path.c_str());
  if (!input) {
    setError(error, "cannot open ASR tokens: " + path);
    return false;
  }
  tokens->clear();
  std::string line;
  while (std::getline(input, line)) {
    const std::string::size_type separator = line.find(' ');
    tokens->push_back(line.substr(0, separator));
  }
  if (tokens->empty()) {
    setError(error, "ASR token file is empty: " + path);
    return false;
  }
  return true;
}

class RuntimeNet {
 public:
  ~RuntimeNet() { reset(); }

  bool load(bm_handle_t handle, const std::string &path, std::string *error) {
    reset();
    runtime_ = bmrt_create(handle);
    if (!runtime_ || !bmrt_load_bmodel(runtime_, path.c_str())) {
      setError(error, "failed to load ASR bmodel: " + path);
      reset();
      return false;
    }
    const char **names = nullptr;
    bmrt_get_network_names(runtime_, &names);
    if (!names || bmrt_get_network_number(runtime_) != 1) {
      if (names) std::free(names);
      setError(error, "ASR bmodel must contain exactly one network: " + path);
      reset();
      return false;
    }
    name_ = names[0];
    std::free(names);
    network_ = bmrt_get_network_info(runtime_, name_.c_str());
    if (!network_ || network_->is_dynamic || network_->stage_num != 1) {
      setError(error, "ASR bmodel must have one static network: " + path);
      reset();
      return false;
    }
    return true;
  }

  bool run(const std::vector<void *> &inputs,
           std::vector<std::vector<std::uint8_t>> *outputs,
           std::vector<bm_shape_t> *output_shapes) const {
    if (!network_ || inputs.size() != static_cast<std::size_t>(network_->input_num)) {
      return false;
    }
    const bm_stage_info_t &stage = network_->stages[0];
    std::vector<bm_shape_t> input_shapes(stage.input_shapes,
                                         stage.input_shapes + network_->input_num);
    outputs->assign(network_->output_num, {});
    std::vector<void *> output_ptrs(network_->output_num);
    output_shapes->assign(network_->output_num, bm_shape_t{});
    for (int i = 0; i < network_->output_num; ++i) {
      (*outputs)[i].resize(network_->max_output_bytes[i]);
      output_ptrs[i] = (*outputs)[i].data();
    }
    return bmrt_launch_data(runtime_, name_.c_str(), inputs.data(), input_shapes.data(),
                            network_->input_num, output_ptrs.data(),
                            output_shapes->data(), network_->output_num, true);
  }

  const bm_net_info_t *network() const { return network_; }

  void reset() {
    network_ = nullptr;
    name_.clear();
    if (runtime_) {
      bmrt_destroy(runtime_);
      runtime_ = nullptr;
    }
  }

 private:
  void *runtime_ = nullptr;
  const bm_net_info_t *network_ = nullptr;
  std::string name_;
};

struct Cache {
  bm_data_type_t type = BM_FLOAT32;
  std::vector<int32_t> integers;
  std::vector<float> floats;

  void *data() {
    return type == BM_INT32 ? static_cast<void *>(integers.data())
                            : static_cast<void *>(floats.data());
  }
};

bool updateCache(const std::vector<std::uint8_t> &source, bm_data_type_t source_type,
                 Cache *destination) {
  if (destination->type == BM_INT32) {
    if (source_type == BM_INT32) {
      std::memcpy(destination->integers.data(), source.data(),
                  destination->integers.size() * sizeof(int32_t));
      return true;
    }
    if (source_type == BM_FLOAT32) {
      const float *values = reinterpret_cast<const float *>(source.data());
      for (std::size_t i = 0; i < destination->integers.size(); ++i) {
        destination->integers[i] = static_cast<int32_t>(values[i]);
      }
      return true;
    }
    return false;
  }
  if (destination->type == BM_FLOAT32) {
    if (source_type == BM_FLOAT32) {
      std::memcpy(destination->floats.data(), source.data(),
                  destination->floats.size() * sizeof(float));
      return true;
    }
    if (source_type == BM_INT32) {
      const int32_t *values = reinterpret_cast<const int32_t *>(source.data());
      for (std::size_t i = 0; i < destination->floats.size(); ++i) {
        destination->floats[i] = static_cast<float>(values[i]);
      }
      return true;
    }
  }
  return false;
}

}  // namespace

class NpuStreamingAsr::Impl {
 public:
  ~Impl() { reset(); }

  bool load(const Config &config, std::string *error) {
    reset();
    ModelDescriptor descriptor;
    if (config.model_spec.empty() ||
        !loadModelDescriptor(config.model_spec, &descriptor, error)) return false;
    const char *required[] = {"encoder_model", "decoder_model", "joiner_model", "tokens"};
    for (const char *key : required) {
      if (descriptor.extra.find(key) == descriptor.extra.end()) {
        setError(error, std::string("ASR spec is missing ") + key);
        return false;
      }
    }
    if (!config.firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.firmware.c_str(), 0);
    }
    const std::string encoder_path = joinPath(descriptor.descriptor_dir,
                                               descriptor.extra["encoder_model"]);
    const std::string decoder_path = joinPath(descriptor.descriptor_dir,
                                               descriptor.extra["decoder_model"]);
    const std::string joiner_path = joinPath(descriptor.descriptor_dir,
                                              descriptor.extra["joiner_model"]);
    const std::string tokens_path = joinPath(descriptor.descriptor_dir,
                                              descriptor.extra["tokens"]);
    if (!readTokens(tokens_path, &tokens_, error) ||
        bm_dev_request(&handle_, 0) != BM_SUCCESS ||
        !encoder_.load(handle_, encoder_path, error) ||
        !decoder_.load(handle_, decoder_path, error) ||
        !joiner_.load(handle_, joiner_path, error)) {
      if (handle_ == nullptr) setError(error, "bm_dev_request failed");
      reset();
      return false;
    }
    const bm_net_info_t *encoder = encoder_.network();
    const bm_net_info_t *decoder = decoder_.network();
    const bm_net_info_t *joiner = joiner_.network();
    if (encoder->input_num != 36 || encoder->output_num != 36 ||
        decoder->input_num != 1 || decoder->output_num != 1 ||
        joiner->input_num != 2 || joiner->output_num != 1 ||
        decoder->input_dtypes[0] != BM_INT32) {
      setError(error, "ASR bmodel IO layout is not CV184X Zipformer 39/320/5537");
      reset();
      return false;
    }
    const bm_stage_info_t &encoder_stage = encoder->stages[0];
    chunk_frames_ = encoder_stage.input_shapes[0].dims[1];
    feature_dim_ = encoder_stage.output_shapes[0].dims[2];
    const int decoder_dim = static_cast<int>(shapeElements(decoder->stages[0].output_shapes[0]) /
                                             decoder->stages[0].output_shapes[0].dims[0]);
    vocab_size_ = static_cast<int>(shapeElements(joiner->stages[0].output_shapes[0]) /
                                  joiner->stages[0].output_shapes[0].dims[0]);
    if (chunk_frames_ != 39 || feature_dim_ != 320 || decoder_dim != feature_dim_ ||
        vocab_size_ != 5537 || vocab_size_ > static_cast<int>(tokens_.size())) {
      setError(error, "ASR bmodels are not the validated 39/320/5537 model set");
      reset();
      return false;
    }
    caches_.assign(35, Cache{});
    for (int i = 1; i < encoder->input_num; ++i) {
      Cache &cache = caches_[i - 1];
      cache.type = encoder->input_dtypes[i];
      const std::size_t count = shapeElements(encoder_stage.input_shapes[i]);
      if (cache.type == BM_INT32) cache.integers.assign(count, 0);
      else if (cache.type == BM_FLOAT32) cache.floats.assign(count, 0.0f);
      else {
        setError(error, "unsupported encoder cache type");
        reset();
        return false;
      }
    }
    knf::FbankOptions options;
    options.frame_opts.samp_freq = kSampleRate;
    options.frame_opts.dither = 0.0f;
    options.frame_opts.snip_edges = false;
    options.mel_opts.num_bins = kFbankBins;
    options.mel_opts.high_freq = -400.0f;
    fbank_.reset(new knf::OnlineFbank(options));
    encoder_input_.resize(static_cast<std::size_t>(chunk_frames_) * kFbankBins);
    encoder_inputs_.resize(36);
    encoder_inputs_[0] = encoder_input_.data();
    for (int i = 1; i < 36; ++i) encoder_inputs_[i] = caches_[i - 1].data();
    decoder_input_.assign(2, 0);
    decoder_feature_.assign(feature_dim_, 0.0f);
    processed_frames_ = 0;
    decoder_ready_ = false;
    finished_ = false;
    emitted_tokens_.clear();
    text_.clear();
    return true;
  }

  bool accept(const std::vector<std::int16_t> &pcm, std::string *new_text,
              std::string *error) {
    if (!fbank_) {
      setError(error, "NPU ASR is not initialized");
      return false;
    }
    if (finished_) {
      setError(error, "NPU ASR has already been finished; load it again for a new utterance");
      return false;
    }
    std::vector<float> waveform(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) waveform[i] = pcm[i] / 32768.0f;
    if (!waveform.empty()) {
      fbank_->AcceptWaveform(kSampleRate, waveform.data(), waveform.size());
    }
    return processReady(new_text, error);
  }

  bool finish(std::string *new_text, std::string *error) {
    if (!fbank_) {
      setError(error, "NPU ASR is not initialized");
      return false;
    }
    if (finished_) {
      if (new_text) new_text->clear();
      return true;
    }
    std::vector<float> silence(kSampleRate / 2, 0.0f);
    fbank_->AcceptWaveform(kSampleRate, silence.data(), silence.size());
    finished_ = true;
    return processReady(new_text, error);
  }

  bool initialized() const { return fbank_ != nullptr; }
  const std::string &text() const { return text_; }

  void reset() {
    fbank_.reset();
    encoder_.reset();
    decoder_.reset();
    joiner_.reset();
    if (handle_) {
      bm_dev_free(handle_);
      handle_ = nullptr;
    }
    tokens_.clear();
    encoder_input_.clear();
    encoder_inputs_.clear();
    decoder_input_.clear();
    decoder_feature_.clear();
    caches_.clear();
    emitted_tokens_.clear();
    chunk_frames_ = 0;
    feature_dim_ = 0;
    vocab_size_ = 0;
    processed_frames_ = 0;
    decoder_ready_ = false;
    finished_ = false;
    text_.clear();
  }

 private:
  bool processReady(std::string *new_text, std::string *error) {
    if (new_text) new_text->clear();
    while (fbank_->NumFramesReady() - processed_frames_ >= chunk_frames_) {
      for (int frame = 0; frame < chunk_frames_; ++frame) {
        const float *source = fbank_->GetFrame(processed_frames_ + frame);
        if (!source) {
          setError(error, "ASR fbank frame is unavailable");
          return false;
        }
        std::memcpy(encoder_input_.data() + frame * kFbankBins, source,
                    kFbankBins * sizeof(float));
      }
      std::vector<std::vector<std::uint8_t>> encoder_outputs;
      std::vector<bm_shape_t> encoder_shapes;
      if (!encoder_.run(encoder_inputs_, &encoder_outputs, &encoder_shapes)) {
        setError(error, "NPU ASR encoder launch failed");
        return false;
      }
      const bm_net_info_t *encoder = encoder_.network();
      for (int i = 1; i < encoder->output_num; ++i) {
        if (!updateCache(encoder_outputs[i], encoder->output_dtypes[i], &caches_[i - 1])) {
          setError(error, "NPU ASR encoder cache conversion failed");
          return false;
        }
      }
      if (!decoder_ready_) {
        std::vector<std::vector<std::uint8_t>> decoder_outputs;
        std::vector<bm_shape_t> decoder_shapes;
        if (!decoder_.run({decoder_input_.data()}, &decoder_outputs, &decoder_shapes)) {
          setError(error, "NPU ASR decoder initialization failed");
          return false;
        }
        std::memcpy(decoder_feature_.data(), decoder_outputs[0].data(),
                    decoder_feature_.size() * sizeof(float));
        decoder_ready_ = true;
      }
      const float *features = reinterpret_cast<const float *>(encoder_outputs[0].data());
      const int output_frames = encoder_shapes[0].dims[1];
      for (int frame = 0; frame < output_frames; ++frame) {
        std::vector<std::vector<std::uint8_t>> joiner_outputs;
        std::vector<bm_shape_t> joiner_shapes;
        void *encoder_feature = const_cast<float *>(features + frame * feature_dim_);
        if (!joiner_.run({encoder_feature, decoder_feature_.data()}, &joiner_outputs,
                         &joiner_shapes)) {
          setError(error, "NPU ASR joiner launch failed");
          return false;
        }
        const float *logits = reinterpret_cast<const float *>(joiner_outputs[0].data());
        const int token = std::max_element(logits, logits + vocab_size_) - logits;
        if (token != 0) {
          decoder_input_[0] = emitted_tokens_.empty() ? 0 : emitted_tokens_.back();
          decoder_input_[1] = token;
          emitted_tokens_.push_back(token);
          std::vector<std::vector<std::uint8_t>> decoder_outputs;
          std::vector<bm_shape_t> decoder_shapes;
          if (!decoder_.run({decoder_input_.data()}, &decoder_outputs, &decoder_shapes)) {
            setError(error, "NPU ASR decoder update failed");
            return false;
          }
          std::memcpy(decoder_feature_.data(), decoder_outputs[0].data(),
                      decoder_feature_.size() * sizeof(float));
          std::string piece = tokens_[token];
          const std::string marker = "\xE2\x96\x81";
          for (std::string::size_type pos = 0; (pos = piece.find(marker, pos)) != std::string::npos;) {
            piece.replace(pos, marker.size(), " ");
          }
          text_ += piece;
          if (new_text) *new_text += piece;
        }
      }
      processed_frames_ += 32;
    }
    return true;
  }

  bm_handle_t handle_ = nullptr;
  RuntimeNet encoder_;
  RuntimeNet decoder_;
  RuntimeNet joiner_;
  std::unique_ptr<knf::OnlineFbank> fbank_;
  std::vector<std::string> tokens_;
  std::vector<Cache> caches_{35};
  std::vector<float> encoder_input_;
  std::vector<void *> encoder_inputs_;
  std::vector<int32_t> decoder_input_;
  std::vector<float> decoder_feature_;
  std::vector<int> emitted_tokens_;
  std::string text_;
  int chunk_frames_ = 0;
  int feature_dim_ = 0;
  int vocab_size_ = 0;
  int processed_frames_ = 0;
  bool decoder_ready_ = false;
  bool finished_ = false;
};

NpuStreamingAsr::NpuStreamingAsr() = default;
NpuStreamingAsr::~NpuStreamingAsr() { reset(); delete impl_; }

bool NpuStreamingAsr::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) impl_ = new Impl;
  return impl_->load(config_, error);
}

bool NpuStreamingAsr::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool NpuStreamingAsr::acceptPcm(const std::vector<std::int16_t> &pcm,
                                std::string *newly_decoded, std::string *error) {
  if (!impl_) {
    setError(error, "NPU ASR is not initialized");
    return false;
  }
  return impl_->accept(pcm, newly_decoded, error);
}

bool NpuStreamingAsr::finish(std::string *newly_decoded, std::string *error) {
  if (!impl_) {
    setError(error, "NPU ASR is not initialized");
    return false;
  }
  return impl_->finish(newly_decoded, error);
}

bool NpuStreamingAsr::initialized() const { return impl_ && impl_->initialized(); }
const std::string &NpuStreamingAsr::text() const {
  static const std::string empty;
  return impl_ ? impl_->text() : empty;
}
void NpuStreamingAsr::reset() { if (impl_) impl_->reset(); }

}  // namespace tdl_app
