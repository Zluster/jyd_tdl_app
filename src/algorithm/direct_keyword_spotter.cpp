#include "tdl_app/direct_keyword_spotter.hpp"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "algorithm/private/bmrt_utils.hpp"
#include "tdl_app/audio_input.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app { namespace {

constexpr int kSampleRate = 16000;
constexpr int kFbankBins = 80;
constexpr int kBlank = 0;
constexpr int kContextSize = 2;
constexpr int kPrefixEndpointFrames = 12;

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

std::size_t shapeElements(const bm_shape_t &shape) {
  std::size_t elements = 1;
  for (int i = 0; i < shape.num_dims; ++i) {
    if (shape.dims[i] <= 0) return 0;
    elements *= static_cast<std::size_t>(shape.dims[i]);
  }
  return elements;
}

std::string joinPath(const std::string &base, const std::string &path) {
  if (path.empty() || path.front() == '/' ||
      (path.size() > 1 && path[1] == ':')) return path;
  return base.empty() || base.back() == '/' ? base + path : base + "/" + path;
}

class RuntimeNet {
 public:
  ~RuntimeNet() { reset(); }

  bool load(bm_handle_t handle, const std::string &path, std::string *error) {
    reset();
    runtime_ = tdl_app::bmrt_runtime::createRuntime(handle);
    if (!runtime_ || !bmrt_load_bmodel(runtime_, path.c_str())) {
      setError(error, "failed to load KWS bmodel: " + path);
      reset();
      return false;
    }
    const char **names = nullptr;
    if (bmrt_get_network_number(runtime_) != 1) {
      setError(error, "KWS bmodel must have exactly one network: " + path);
      reset();
      return false;
    }
    bmrt_get_network_names(runtime_, &names);
    if (!names) {
      setError(error, "KWS bmodel has no network name: " + path);
      reset();
      return false;
    }
    name_ = names[0];
    std::free(names);
    network_ = bmrt_get_network_info(runtime_, name_.c_str());
    if (!network_ || network_->is_dynamic || network_->stage_num != 1) {
      setError(error, "KWS bmodel must have one static network: " + path);
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
    outputs->assign(network_->output_num, std::vector<std::uint8_t>());
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
      tdl_app::bmrt_runtime::destroyRuntime(runtime_);
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
  std::vector<float> floats;
  std::vector<std::int32_t> integers;

  void *data() {
    return type == BM_INT32 ? static_cast<void *>(integers.data())
                            : static_cast<void *>(floats.data());
  }
};

bool updateCache(const std::vector<std::uint8_t> &source, bm_data_type_t source_type,
                 Cache *destination) {
  if (destination->type == BM_FLOAT32) {
    if (source_type == BM_FLOAT32) {
      std::memcpy(destination->floats.data(), source.data(),
                  destination->floats.size() * sizeof(float));
      return true;
    }
    if (source_type == BM_INT32) {
      const std::int32_t *values = reinterpret_cast<const std::int32_t *>(source.data());
      for (std::size_t i = 0; i < destination->floats.size(); ++i) {
        destination->floats[i] = static_cast<float>(values[i]);
      }
      return true;
    }
  }
  if (destination->type == BM_INT32) {
    if (source_type == BM_INT32) {
      std::memcpy(destination->integers.data(), source.data(),
                  destination->integers.size() * sizeof(std::int32_t));
      return true;
    }
    if (source_type == BM_FLOAT32) {
      const float *values = reinterpret_cast<const float *>(source.data());
      for (std::size_t i = 0; i < destination->integers.size(); ++i) {
        destination->integers[i] = static_cast<std::int32_t>(values[i]);
      }
      return true;
    }
  }
  return false;
}

struct Keyword {
  std::string name;
  std::vector<int> tokens;
  float threshold = 0.10f;
};

struct KeywordHit {
  std::string name;
  float score = 0.0f;
  float threshold = 0.0f;
  int matched_tokens = 0;
  int total_tokens = 0;
  std::string matched_text;
  bool complete = false;
  bool triggered = false;
};

struct EmittedToken {
  int id = 0;
  float log_probability = 0.0f;
};

std::string replaceAll(std::string value, const std::string &from,
                       const std::string &to) {
  std::string::size_type position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.size(), to);
    position += to.size();
  }
  return value;
}

std::string nearSoundKey(std::string token) {
  // Tone-insensitive pinyin plus the common -n/-ng near-sound pair.
  const char *tone_chars[][2] = {
      {"ā", "a"}, {"á", "a"}, {"ǎ", "a"}, {"à", "a"},
      {"ē", "e"}, {"é", "e"}, {"ě", "e"}, {"è", "e"},
      {"ī", "i"}, {"í", "i"}, {"ǐ", "i"}, {"ì", "i"},
      {"ō", "o"}, {"ó", "o"}, {"ǒ", "o"}, {"ò", "o"},
      {"ū", "u"}, {"ú", "u"}, {"ǔ", "u"}, {"ù", "u"},
      {"ǖ", "u"}, {"ǘ", "u"}, {"ǚ", "u"}, {"ǜ", "u"},
      {"ü", "u"}, {"ê", "e"}};
  for (const auto &entry : tone_chars) token = replaceAll(token, entry[0], entry[1]);
  if (token.size() > 1 && token.back() == 'g' &&
      token[token.size() - 2] == 'n') {
    token.pop_back();
  }
  return token;
}

bool readTokens(const std::string &path, std::map<std::string, int> *ids,
                std::vector<std::string> *names,
                std::string *error) {
  std::ifstream input(path.c_str());
  if (!input) {
    setError(error, "cannot open KWS token file: " + path);
    return false;
  }
  ids->clear();
  std::string token;
  int id = 0;
  while (input >> token >> id) {
    (*ids)[token] = id;
    if (id >= static_cast<int>(names->size())) names->resize(id + 1);
    (*names)[id] = token;
  }
  if (ids->empty()) {
    setError(error, "KWS token file is empty: " + path);
    return false;
  }
  return true;
}

bool readKeywords(const std::string &path, const std::map<std::string, int> &ids,
                  float default_threshold, std::vector<Keyword> *keywords,
                  std::string *error) {
  std::ifstream input(path.c_str());
  if (!input) {
    setError(error, "cannot open KWS keyword file: " + path);
    return false;
  }
  keywords->clear();
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream stream(line);
    Keyword keyword;
    keyword.threshold = default_threshold;
    std::string word;
    while (stream >> word) {
      if (word[0] == ':') continue;
      if (word[0] == '#') {
        try {
          keyword.threshold = std::stof(word.substr(1));
        } catch (...) {
          setError(error, "invalid KWS keyword threshold: " + word);
          return false;
        }
        continue;
      }
      if (word[0] == '@') {
        keyword.name = word.substr(1);
        std::string extra;
        while (stream >> extra) keyword.name += " " + extra;
        break;
      }
      const std::map<std::string, int>::const_iterator it = ids.find(word);
      if (it == ids.end()) {
        setError(error, "KWS keyword token is absent from tokens: " + word);
        return false;
      }
      keyword.tokens.push_back(it->second);
    }
    if (!keyword.tokens.empty()) {
      if (keyword.name.empty()) keyword.name = line;
      keywords->push_back(keyword);
    }
  }
  if (keywords->empty()) {
    setError(error, "KWS keyword file contains no keywords: " + path);
    return false;
  }
  return true;
}

void addNearSoundVariants(const std::map<std::string, int> &ids,
                          const std::vector<std::string> &token_names,
                          std::vector<Keyword> *keywords) {
  // The KWS model commonly confuses nasal finals -n and -ng.  This mirrors
  // Sipeed's near-sound keyword matching without accepting unrelated tokens.
  const std::size_t original_count = keywords->size();
  for (std::size_t phrase = 0; phrase < original_count; ++phrase) {
    const Keyword source = (*keywords)[phrase];
    for (std::size_t position = 0; position < source.tokens.size(); ++position) {
      const int token_id = source.tokens[position];
      if (token_id < 0 || token_id >= static_cast<int>(token_names.size())) continue;
      const std::string &token = token_names[token_id];
      std::string alternate;
      if (token.size() > 1 && token.back() == 'g' &&
          token[token.size() - 2] == 'n') {
        alternate = token.substr(0, token.size() - 1);
      } else if (!token.empty() && token.back() == 'n') {
        alternate = token + "g";
      } else {
        continue;
      }
      const std::map<std::string, int>::const_iterator it = ids.find(alternate);
      if (it == ids.end()) continue;
      Keyword variant = source;
      variant.tokens[position] = it->second;
      bool duplicate = false;
      for (const Keyword &existing : *keywords) {
        if (existing.tokens == variant.tokens && existing.name == variant.name) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) keywords->push_back(variant);
    }
  }
}

class DirectKeywordSpotterCore {
 public:
  ~DirectKeywordSpotterCore() { reset(); }

  bool load(const std::string &model_spec, const std::string &keywords_path,
            const std::string &firmware, float threshold_override,
            bool print_tokens, bool print_scores, int beam_width,
            std::string *error) {
    reset();
    tdl_app::ModelDescriptor descriptor;
    if (!tdl_app::loadModelDescriptor(model_spec, &descriptor, error)) return false;
    const char *required[] = {"encoder_model", "decoder_model", "joiner_model", "tokens"};
    for (const char *key : required) {
      if (descriptor.extra.find(key) == descriptor.extra.end()) {
        setError(error, std::string("KWS spec is missing ") + key);
        return false;
      }
    }
    if (!firmware.empty()) setenv("BMRUNTIME_USING_FIRMWARE", firmware.c_str(), 0);
    if (!readTokens(joinPath(descriptor.descriptor_dir, descriptor.extra["tokens"]),
                    &token_ids_, &token_names_, error) ||
        !readKeywords(keywords_path, token_ids_,
                      threshold_override >= 0.0f ? threshold_override : 0.10f,
                      &keywords_, error) ||
        !tdl_app::bmrt_runtime::acquireDevice(&handle_, error) ||
        !encoder_.load(handle_, joinPath(descriptor.descriptor_dir,
                                        descriptor.extra["encoder_model"]), error) ||
        !decoder_.load(handle_, joinPath(descriptor.descriptor_dir,
                                        descriptor.extra["decoder_model"]), error) ||
        !joiner_.load(handle_, joinPath(descriptor.descriptor_dir,
                                       descriptor.extra["joiner_model"]), error)) {
      reset();
      return false;
    }
    for (std::size_t token = 0; token < token_names_.size(); ++token) {
      if (!token_names_[token].empty()) {
        near_token_ids_[nearSoundKey(token_names_[token])].push_back(
            static_cast<int>(token));
      }
    }
    addNearSoundVariants(token_ids_, token_names_, &keywords_);
    if (threshold_override >= 0.0f) {
      for (Keyword &keyword : keywords_) keyword.threshold = threshold_override;
    }
    const bm_net_info_t *encoder = encoder_.network();
    const bm_net_info_t *decoder = decoder_.network();
    const bm_net_info_t *joiner = joiner_.network();
    if (encoder->input_num != 39 || encoder->output_num != 39 ||
        decoder->input_num != 1 || decoder->output_num != 1 ||
        joiner->input_num != 2 || joiner->output_num != 1 ||
        decoder->input_dtypes[0] != BM_INT32) {
      setError(error, "KWS bmodels are not the CV184X 45/8/320/197 model set");
      reset();
      return false;
    }
    const bm_stage_info_t &encoder_stage = encoder->stages[0];
    chunk_frames_ = encoder_stage.input_shapes[0].dims[1];
    output_frames_ = encoder_stage.output_shapes[0].dims[1];
    feature_dim_ = encoder_stage.output_shapes[0].dims[2];
    vocab_size_ = static_cast<int>(shapeElements(joiner->stages[0].output_shapes[0]));
    if (chunk_frames_ != 45 || output_frames_ != 8 || feature_dim_ != 320 ||
        vocab_size_ != 197) {
      setError(error, "KWS bmodels have unexpected tensor shapes");
      reset();
      return false;
    }
    caches_.assign(38, Cache{});
    encoder_inputs_.resize(39);
    encoder_input_.assign(static_cast<std::size_t>(chunk_frames_) * kFbankBins, 0.0f);
    encoder_inputs_[0] = encoder_input_.data();
    for (int i = 1; i < encoder->input_num; ++i) {
      Cache &cache = caches_[i - 1];
      cache.type = encoder->input_dtypes[i];
      const std::size_t count = shapeElements(encoder_stage.input_shapes[i]);
      if (cache.type == BM_FLOAT32) cache.floats.assign(count, 0.0f);
      else if (cache.type == BM_INT32) cache.integers.assign(count, 0);
      else {
        setError(error, "KWS encoder has unsupported state type");
        reset();
        return false;
      }
      encoder_inputs_[i] = cache.data();
    }
    knf::FbankOptions options;
    options.frame_opts.samp_freq = kSampleRate;
    options.frame_opts.dither = 0.0f;
    options.frame_opts.snip_edges = false;
    options.mel_opts.num_bins = kFbankBins;
    options.mel_opts.high_freq = -400.0f;
    fbank_.reset(new knf::OnlineFbank(options));
    processed_frames_ = 0;
    beam_width_ = beam_width;
    beams_.clear();
    print_tokens_ = print_tokens;
    print_scores_ = print_scores;
    return true;
  }

  bool accept(const std::vector<std::int16_t> &pcm, std::vector<KeywordHit> *hits,
              std::string *error) {
    if (!fbank_) {
      setError(error, "direct KWS is not initialized");
      return false;
    }
    std::vector<float> waveform(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) waveform[i] = pcm[i] / 32768.0f;
    if (!waveform.empty()) fbank_->AcceptWaveform(kSampleRate, waveform.data(), waveform.size());
    return processReady(hits, error);
  }

  bool finish(std::vector<KeywordHit> *hits, std::string *error) {
    if (!fbank_) {
      setError(error, "direct KWS is not initialized");
      return false;
    }
    std::vector<float> silence(kSampleRate / 2, 0.0f);
    fbank_->AcceptWaveform(kSampleRate, silence.data(), silence.size());
    return processReady(hits, error);
  }

  bool initialized() const { return fbank_ != nullptr; }
  std::vector<KeywordHit> scores() const { return last_scores_; }
  void clear() { reset(); }

 private:
  struct BeamHypothesis {
    std::vector<std::int32_t> context;
    std::vector<float> decoder_feature;
    std::vector<EmittedToken> emitted;
    float log_score = 0.0f;
    float search_score = 0.0f;
    int blank_frames = 0;
  };

  bool runDecoder(const std::vector<std::int32_t> &context,
                  std::vector<float> *feature, std::string *error) {
    std::vector<std::vector<std::uint8_t>> outputs;
    std::vector<bm_shape_t> shapes;
    if (!decoder_.run({const_cast<std::int32_t *>(context.data())}, &outputs, &shapes) ||
        outputs.empty() || outputs[0].size() < feature->size() * sizeof(float)) {
      setError(error, "KWS decoder launch failed");
      return false;
    }
    std::memcpy(feature->data(), outputs[0].data(), feature->size() * sizeof(float));
    return true;
  }

  std::vector<KeywordHit> keywordScores() const {
    std::map<std::string, KeywordHit> by_name;
    for (const Keyword &keyword : keywords_) {
      KeywordHit best;
      best.name = keyword.name;
      best.threshold = keyword.threshold;
      best.total_tokens = static_cast<int>(keyword.tokens.size());
      for (const BeamHypothesis &beam : beams_) {
        const std::size_t maximum =
            std::min(keyword.tokens.size(), beam.emitted.size());
        std::size_t matched = 0;
        float log_sum = 0.0f;
        for (std::size_t length = maximum; length > 0; --length) {
          bool same_tokens = true;
          log_sum = 0.0f;
          for (std::size_t i = 0; i < length; ++i) {
            const EmittedToken &emitted = beam.emitted[beam.emitted.size() - length + i];
            if (!equivalentToken(keyword.tokens[i], emitted.id)) {
              same_tokens = false;
              break;
            }
            log_sum += emitted.log_probability;
          }
          if (same_tokens) {
            const float score = std::exp(log_sum / static_cast<float>(length)) *
                                static_cast<float>(length) /
                                static_cast<float>(keyword.tokens.size());
            if (score > best.score ||
                (score == best.score && static_cast<int>(length) > best.matched_tokens)) {
              best.score = score;
              best.matched_tokens = static_cast<int>(length);
              best.complete = length == keyword.tokens.size();
              best.matched_text.clear();
              for (std::size_t i = 0; i < length; ++i) {
                if (!best.matched_text.empty()) best.matched_text += " ";
                const int token = beam.emitted[beam.emitted.size() - length + i].id;
                best.matched_text += token >= 0 &&
                                     token < static_cast<int>(token_names_.size())
                                         ? token_names_[token] : "?";
              }
            }
            matched = length;
            break;
          }
        }
        (void)matched;
      }
      best.triggered = best.complete && best.score >= best.threshold;
      std::map<std::string, KeywordHit>::iterator current = by_name.find(best.name);
      if (current == by_name.end() || best.score > current->second.score) {
        by_name[best.name] = best;
      }
    }
    std::vector<KeywordHit> scores;
    for (const std::pair<const std::string, KeywordHit> &item : by_name) {
      scores.push_back(item.second);
    }
    return scores;
  }

  static float logProbability(const float *logits, int count, int index) {
    const float maximum = *std::max_element(logits, logits + count);
    double sum = 0.0;
    for (int i = 0; i < count; ++i) sum += std::exp(static_cast<double>(logits[i] - maximum));
    return logits[index] - maximum - static_cast<float>(std::log(sum));
  }

  std::vector<int> trieCandidates(const BeamHypothesis &beam) const {
    std::set<int> candidates;
    for (const Keyword &keyword : keywords_) {
      for (std::size_t prefix = 0; prefix < keyword.tokens.size(); ++prefix) {
        if (prefix > beam.emitted.size()) continue;
        bool prefix_matches = true;
        for (std::size_t i = 0; i < prefix; ++i) {
          if (!equivalentToken(keyword.tokens[i],
                               beam.emitted[beam.emitted.size() - prefix + i].id)) {
            prefix_matches = false;
            break;
          }
        }
        if (prefix_matches) {
          const int expected = keyword.tokens[prefix];
          const std::string key = tokenNearKey(expected);
          const std::map<std::string, std::vector<int>>::const_iterator variants =
              near_token_ids_.find(key);
          if (variants != near_token_ids_.end()) {
            candidates.insert(variants->second.begin(), variants->second.end());
          } else {
            candidates.insert(expected);
          }
        }
      }
    }
    return std::vector<int>(candidates.begin(), candidates.end());
  }

  bool ensureBeam(std::string *error) {
    if (!beams_.empty()) return true;
    BeamHypothesis initial;
    // Match the transducer initial decoder input: leading history is the
    // sentinel -1 and only the newest item is the blank token.
    initial.context.assign(kContextSize, -1);
    initial.context.back() = kBlank;
    initial.decoder_feature.assign(feature_dim_, 0.0f);
    if (!runDecoder(initial.context, &initial.decoder_feature, error)) return false;
    beams_.push_back(initial);
    return true;
  }

  bool decodeFrame(const float *encoder_feature, std::vector<KeywordHit> *hits,
                   std::string *error) {
    if (!ensureBeam(error)) return false;
    // The Zipformer model uses its modified beam search: each encoder
    // frame gets exactly one decoder/joiner expansion. A regular RNNT inner
    // loop can emit several labels for one acoustic frame and breaks both the
    // decoder state timing and keyword-context scoring.
    struct Candidate {
      int parent = 0;
      int token = kBlank;
      float log_probability = 0.0f;
      float search_score = 0.0f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(beams_.size() * static_cast<std::size_t>(vocab_size_));
    for (std::size_t i = 0; i < beams_.size(); ++i) {
      const BeamHypothesis &beam = beams_[i];
      std::vector<std::vector<std::uint8_t>> outputs;
      std::vector<bm_shape_t> shapes;
      if (!joiner_.run({const_cast<float *>(encoder_feature),
                        const_cast<float *>(beam.decoder_feature.data())},
                       &outputs, &shapes) || outputs.empty()) {
        setError(error, "KWS joiner launch failed");
        return false;
      }
      const float *logits = reinterpret_cast<const float *>(outputs[0].data());
      const std::vector<int> expected = trieCandidates(beam);
      const std::set<int> expected_set(expected.begin(), expected.end());
      for (int token = 0; token < vocab_size_; ++token) {
        const float log_p = logProbability(logits, vocab_size_, token);
        Candidate candidate;
        candidate.parent = static_cast<int>(i);
        candidate.token = token;
        candidate.log_probability = log_p;
        candidate.search_score = beam.search_score + log_p;
        if (token != kBlank && expected_set.find(token) != expected_set.end()) {
          // Minimal ContextGraph equivalent. This bonus affects beam ranking,
          // while KeywordHit confidence remains based on acoustic log_prob.
          candidate.search_score += 1.0f;
        }
        candidates.push_back(candidate);
      }
    }
    const std::size_t keep = std::min(
        candidates.size(), static_cast<std::size_t>(std::max(1, beam_width_)));
    std::partial_sort(candidates.begin(), candidates.begin() + keep, candidates.end(),
                      [](const Candidate &a, const Candidate &b) {
                        return a.search_score > b.search_score;
                      });
    std::vector<BeamHypothesis> next_time;
    next_time.reserve(keep);
    for (std::size_t i = 0; i < keep; ++i) {
      const Candidate &candidate = candidates[i];
      BeamHypothesis child = beams_[candidate.parent];
      child.log_score += candidate.log_probability;
      child.search_score = candidate.search_score;
      if (candidate.token == kBlank) {
        ++child.blank_frames;
      } else {
        child.blank_frames = 0;
        child.context[0] = child.context[1];
        child.context[1] = candidate.token;
        child.emitted.push_back({candidate.token, candidate.log_probability});
        if (child.emitted.size() > 24) child.emitted.erase(child.emitted.begin());
        if (!runDecoder(child.context, &child.decoder_feature, error)) return false;
        if (print_tokens_) {
          const std::string text = candidate.token < static_cast<int>(token_names_.size())
                                       ? token_names_[candidate.token] : "?";
          std::cout << "beam_token=" << text << " id=" << candidate.token
                    << " confidence=" << std::exp(candidate.log_probability) << "\n";
        }
      }
      next_time.push_back(std::move(child));
    }
    beams_.swap(next_time);
    for (BeamHypothesis &beam : beams_) {
      if (beam.blank_frames < kPrefixEndpointFrames || beam.emitted.empty()) continue;
      beam.context.assign(kContextSize, -1);
      beam.context.back() = kBlank;
      beam.emitted.clear();
      beam.blank_frames = 0;
      beam.log_score = 0.0f;
      beam.search_score = 0.0f;
      if (!runDecoder(beam.context, &beam.decoder_feature, error)) return false;
    }
    const std::vector<KeywordHit> scores = keywordScores();
    last_scores_ = scores;
    bool triggered = false;
    for (const KeywordHit &score : scores) {
      if (score.triggered) {
        hits->push_back(score);
        triggered = true;
      }
    }
    if (triggered) beams_.clear();
    return true;
  }

  std::string tokenNearKey(int token) const {
    if (token < 0 || token >= static_cast<int>(token_names_.size())) return "";
    return nearSoundKey(token_names_[token]);
  }

  bool equivalentToken(int expected, int actual) const {
    return expected == actual || tokenNearKey(expected) == tokenNearKey(actual);
  }

  bool processReady(std::vector<KeywordHit> *hits, std::string *error) {
    if (hits) hits->clear();
    while (fbank_->NumFramesReady() - processed_frames_ >= chunk_frames_) {
      for (int i = 0; i < chunk_frames_; ++i) {
        const float *frame = fbank_->GetFrame(processed_frames_ + i);
        if (!frame) {
          setError(error, "KWS Fbank frame is unavailable");
          return false;
        }
        std::memcpy(encoder_input_.data() + i * kFbankBins, frame,
                    kFbankBins * sizeof(float));
      }
      std::vector<std::vector<std::uint8_t>> outputs;
      std::vector<bm_shape_t> shapes;
      if (!encoder_.run(encoder_inputs_, &outputs, &shapes) || outputs.size() != 39) {
        setError(error, "KWS encoder launch failed");
        return false;
      }
      const bm_net_info_t *encoder = encoder_.network();
      for (int i = 1; i < encoder->output_num; ++i) {
        if (!updateCache(outputs[i], encoder->output_dtypes[i], &caches_[i - 1])) {
          setError(error, "KWS encoder state conversion failed");
          return false;
        }
      }
      const float *features = reinterpret_cast<const float *>(outputs[0].data());
      for (int i = 0; i < output_frames_; ++i) {
        if (!decodeFrame(features + i * feature_dim_, hits, error)) return false;
      }
      if (print_scores_) {
        for (const KeywordHit &score : last_scores_) {
          std::cout << "score keyword=" << score.name
                    << " confidence=" << score.score
                    << " matched=" << score.matched_tokens
                    << "/" << score.total_tokens
                    << " tokens=\"" << score.matched_text << "\""
                    << " complete=" << (score.complete ? 1 : 0)
                    << " threshold=" << score.threshold << "\n";
        }
      }
      processed_frames_ += 32;
    }
    return true;
  }

  void reset() {
    fbank_.reset();
    encoder_.reset();
    decoder_.reset();
    joiner_.reset();
    if (handle_) tdl_app::bmrt_runtime::releaseDevice(&handle_);
    token_ids_.clear();
    token_names_.clear();
    near_token_ids_.clear();
    keywords_.clear();
    caches_.clear();
    encoder_input_.clear();
    encoder_inputs_.clear();
    beams_.clear();
    last_scores_.clear();
    chunk_frames_ = output_frames_ = feature_dim_ = vocab_size_ = processed_frames_ = 0;
  }

  bm_handle_t handle_ = nullptr;
  RuntimeNet encoder_;
  RuntimeNet decoder_;
  RuntimeNet joiner_;
  std::unique_ptr<knf::OnlineFbank> fbank_;
  std::map<std::string, int> token_ids_;
  std::vector<std::string> token_names_;
  std::map<std::string, std::vector<int>> near_token_ids_;
  std::vector<Keyword> keywords_;
  std::vector<Cache> caches_;
  std::vector<float> encoder_input_;
  std::vector<void *> encoder_inputs_;
  std::vector<BeamHypothesis> beams_;
  std::vector<KeywordHit> last_scores_;
  int chunk_frames_ = 0;
  int output_frames_ = 0;
  int feature_dim_ = 0;
  int vocab_size_ = 0;
  int processed_frames_ = 0;
  int beam_width_ = 4;
  bool print_tokens_ = false;
  bool print_scores_ = false;
};

}  // namespace
class DirectKeywordSpotter::Impl { public: DirectKeywordSpotterCore spotter; };
namespace {
DirectKeywordResult copyResult(const KeywordHit &source) { DirectKeywordResult out; out.name = source.name; out.confidence = source.score; out.threshold = source.threshold; out.matched_tokens = source.matched_tokens; out.total_tokens = source.total_tokens; out.matched_text = source.matched_text; out.complete = source.complete; out.triggered = source.triggered; return out; }
void copyResults(const std::vector<KeywordHit> &source, std::vector<DirectKeywordResult> *destination) { if (!destination) return; destination->clear(); destination->reserve(source.size()); for (const KeywordHit &value : source) destination->push_back(copyResult(value)); }
}  // namespace
DirectKeywordSpotter::DirectKeywordSpotter() : impl_(new Impl) {}
DirectKeywordSpotter::~DirectKeywordSpotter() { delete impl_; }
bool DirectKeywordSpotter::load(const std::string &model_spec, const std::string &keywords_path, const std::string &firmware, float threshold_override, int beam_width, std::string *error) { if (!impl_) impl_ = new Impl; return impl_->spotter.load(model_spec, keywords_path, firmware, threshold_override, false, false, beam_width, error); }
bool DirectKeywordSpotter::accept(const std::vector<std::int16_t> &pcm, std::vector<DirectKeywordResult> *hits, std::string *error) { std::vector<KeywordHit> raw; const bool ok = impl_ && impl_->spotter.accept(pcm, &raw, error); if (ok) copyResults(raw, hits); return ok; }
bool DirectKeywordSpotter::finish(std::vector<DirectKeywordResult> *hits, std::string *error) { std::vector<KeywordHit> raw; const bool ok = impl_ && impl_->spotter.finish(&raw, error); if (ok) copyResults(raw, hits); return ok; }
std::vector<DirectKeywordResult> DirectKeywordSpotter::scores() const { std::vector<DirectKeywordResult> out; if (impl_) copyResults(impl_->spotter.scores(), &out); return out; }
bool DirectKeywordSpotter::initialized() const { return impl_ && impl_->spotter.initialized(); }
void DirectKeywordSpotter::reset() { if (impl_) impl_->spotter.clear(); }
}  // namespace tdl_app
