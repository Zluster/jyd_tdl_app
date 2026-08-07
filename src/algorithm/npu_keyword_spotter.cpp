#include "tdl_app/npu_keyword_spotter.hpp"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sherpa-onnx/csrc/keyword-spotter.h"
#include "sherpa-onnx/csrc/online-stream.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kFeatureDim = 80;
constexpr std::size_t kTailPaddingSamples = 12800;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

std::string joinPath(const std::string &base, const std::string &path) {
  if (path.empty() || path.front() == '/' ||
      (path.size() > 1 && path[1] == ':')) {
    return path;
  }
  return base.empty() || base.back() == '/' ? base + path : base + "/" + path;
}

bool readTextFile(const std::string &path, std::string *text) {
  if (!text) return false;
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) return false;
  *text = std::string(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
  return static_cast<bool>(input) || input.eof();
}

}  // namespace

class NpuKeywordSpotter::Impl {
 public:
  ~Impl() { reset(); }

  bool load(const Config &config, std::string *error) {
    reset();

    ModelDescriptor descriptor;
    if (config.model_spec.empty() ||
        !loadModelDescriptor(config.model_spec, &descriptor, error)) {
      return false;
    }

    const char *required[] = {"encoder_model", "decoder_model", "joiner_model", "tokens"};
    for (const char *key : required) {
      if (descriptor.extra.find(key) == descriptor.extra.end()) {
        setError(error, std::string("KWS spec is missing ") + key);
        return false;
      }
    }

    encoder_path_ = joinPath(descriptor.descriptor_dir, descriptor.extra["encoder_model"]);
    decoder_path_ = joinPath(descriptor.descriptor_dir, descriptor.extra["decoder_model"]);
    joiner_path_ = joinPath(descriptor.descriptor_dir, descriptor.extra["joiner_model"]);
    tokens_path_ = joinPath(descriptor.descriptor_dir, descriptor.extra["tokens"]);

    for (const std::string *path : {&encoder_path_, &decoder_path_, &joiner_path_, &tokens_path_}) {
      std::ifstream input(path->c_str(), std::ios::binary);
      if (!input) {
        setError(error, "KWS model file is not readable: " + *path);
        reset();
        return false;
      }
    }
    std::string firmware = config.firmware;
    if (firmware.empty()) {
      firmware = joinPath(descriptor.descriptor_dir,
                          "../../firmware/libbm1688_kernel_module.so");
    }
    std::ifstream firmware_file(firmware.c_str(), std::ios::binary);
    if (firmware_file) {
      if (setenv("BMRUNTIME_USING_FIRMWARE", firmware.c_str(), 1) != 0) {
        setError(error, "failed to configure BM Runtime firmware: " + firmware);
        return false;
      }
    } else if (!std::getenv("BMRUNTIME_USING_FIRMWARE")) {
      setError(error, "BM Runtime firmware is not readable: " + firmware);
      return false;
    }
    loaded_ = true;
    return true;
  }

  bool loadKeywords(const std::string &path, std::string *error) {
    if (!loaded_) {
      setError(error, "load the KWS model before loading keywords");
      return false;
    }
    if (!readTextFile(path, &keywords_)) {
      setError(error, "cannot read KWS keyword file: " + path);
      return false;
    }
    if (keywords_.find_first_not_of(" \t\r\n") == std::string::npos) {
      setError(error, "KWS keyword file is empty: " + path);
      return false;
    }

    keyword_path_ = path;

    sherpa_onnx::KeywordSpotterConfig spotter_config;
    spotter_config.feat_config.sampling_rate = kSampleRate;
    spotter_config.feat_config.feature_dim = kFeatureDim;
    spotter_config.model_config.transducer.encoder = encoder_path_;
    spotter_config.model_config.transducer.decoder = decoder_path_;
    spotter_config.model_config.transducer.joiner = joiner_path_;
    spotter_config.model_config.tokens = tokens_path_;
    spotter_config.model_config.model_type = "zipformer2";
    spotter_config.model_config.provider_config.provider = "cpu";
    spotter_config.model_config.num_threads = 1;
    spotter_config.max_active_paths = 4;
    // Trigger as soon as every keyword token is present. Waiting for trailing
    // blanks drops fast keywords that are immediately followed by more speech.
    spotter_config.num_trailing_blanks = 0;
    spotter_config.keywords_score = 1.0f;
    spotter_config.keywords_threshold = 0.25f;
    spotter_config.keywords_file = keyword_path_;
    if (!spotter_config.Validate()) {
      setError(error, "invalid Sherpa CV184X KWS configuration");
      keyword_path_.clear();
      return false;
    }

    try {
      spotter_.reset(new sherpa_onnx::KeywordSpotter(spotter_config));
      stream_ = spotter_->CreateStream();
    } catch (const std::exception &e) {
      setError(error, std::string("failed to initialize KWS decoder: ") + e.what());
      spotter_.reset();
      stream_.reset();
      keyword_path_.clear();
      return false;
    }
    if (!stream_) {
      setError(error, "failed to create the KWS decode stream");
      spotter_.reset();
      keyword_path_.clear();
      return false;
    }
    return true;
  }

  bool acceptPcm(const std::vector<std::int16_t> &pcm,
                  std::vector<KeywordEvent> *events,
                  std::vector<KeywordScore> *scores, std::string *error) {
    if (!spotter_ || !stream_) {
      setError(error, "KWS is not initialized; call loadKeywords first");
      return false;
    }
    if (events) events->clear();
    if (scores) scores->clear();
    if (pcm.empty()) return true;

    std::vector<float> samples(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      samples[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    stream_->AcceptWaveform(kSampleRate, samples.data(), samples.size());
    return decodeReady(events, scores, error);
  }

  bool finish(std::vector<KeywordEvent> *events, std::string *error) {
    if (!spotter_ || !stream_) {
      setError(error, "KWS is not initialized; call loadKeywords first");
      return false;
    }
    if (events) events->clear();
    std::vector<float> padding(kTailPaddingSamples, 0.0f);
    stream_->AcceptWaveform(kSampleRate, padding.data(), padding.size());
    stream_->InputFinished();
    if (!decodeReady(events, nullptr, error)) return false;
    stream_ = spotter_->CreateStream();
    if (!stream_) {
      setError(error, "failed to restart the KWS decode stream");
      return false;
    }
    return true;
  }

  bool initialized() const { return loaded_ && spotter_ && stream_; }

  void reset() {
    stream_.reset();
    spotter_.reset();
    loaded_ = false;
    encoder_path_.clear();
    decoder_path_.clear();
    joiner_path_.clear();
    tokens_path_.clear();
    keywords_.clear();
    keyword_path_.clear();
  }

 private:
  bool decodeReady(std::vector<KeywordEvent> *events,
                   std::vector<KeywordScore> *scores, std::string *error) {
    try {
      while (spotter_->IsReady(stream_.get())) {
        spotter_->DecodeStream(stream_.get());
        const sherpa_onnx::KeywordResult result = spotter_->GetResult(stream_.get());
        if (scores) {
          scores->clear();
          const std::size_t count = std::min(result.score_keywords.size(),
                                             result.keyword_scores.size());
          scores->reserve(count);
          for (std::size_t i = 0; i < count; ++i) {
            KeywordScore score;
            score.text = result.score_keywords[i];
            score.probability = result.keyword_scores[i];
            scores->push_back(std::move(score));
          }
        }
        if (!result.keyword.empty()) {
          KeywordEvent event;
          event.text.assign(result.keyword.c_str());
          event.confidence = result.confidence;
          event.tokens.reserve(result.tokens.size());
          for (const std::string &token : result.tokens) {
            event.tokens.emplace_back(token.c_str());
          }
          if (events) events->push_back(std::move(event));
        }
      }
    } catch (const std::exception &e) {
      setError(error, std::string("KWS decode failed: ") + e.what());
      return false;
    }
    return true;
  }

  bool loaded_ = false;
  std::string encoder_path_;
  std::string decoder_path_;
  std::string joiner_path_;
  std::string tokens_path_;
  std::string keywords_;
  std::string keyword_path_;
  std::unique_ptr<sherpa_onnx::KeywordSpotter> spotter_;
  std::unique_ptr<sherpa_onnx::OnlineStream> stream_;
};

NpuKeywordSpotter::NpuKeywordSpotter() : impl_(new Impl) {}
NpuKeywordSpotter::~NpuKeywordSpotter() { delete impl_; }

bool NpuKeywordSpotter::load(const Config &config, std::string *error) {
  return impl_->load(config, error);
}

bool NpuKeywordSpotter::load(const std::string &model_spec, std::string *error) {
  return load(ModelSessionConfig::fromSpec(model_spec), error);
}

bool NpuKeywordSpotter::loadKeywords(const std::string &path, std::string *error) {
  return impl_->loadKeywords(path, error);
}

bool NpuKeywordSpotter::acceptPcm(const std::vector<std::int16_t> &pcm16le_mono,
                                  std::vector<KeywordEvent> *events,
                                  std::string *error) {
  return impl_->acceptPcm(pcm16le_mono, events, nullptr, error);
}

bool NpuKeywordSpotter::acceptPcm(const std::vector<std::int16_t> &pcm16le_mono,
                                  std::vector<KeywordEvent> *events,
                                  std::vector<KeywordScore> *scores,
                                  std::string *error) {
  return impl_->acceptPcm(pcm16le_mono, events, scores, error);
}

bool NpuKeywordSpotter::finish(std::vector<KeywordEvent> *events,
                               std::string *error) {
  return impl_->finish(events, error);
}

bool NpuKeywordSpotter::initialized() const { return impl_->initialized(); }
void NpuKeywordSpotter::reset() { impl_->reset(); }

}  // namespace tdl_app
