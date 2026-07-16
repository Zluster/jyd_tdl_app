#include "tdl_app/speech_recognizer.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "c_apis/tdl_model_def.h"
#include "c_apis/tdl_sdk.h"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string resolveModelPathFromSpec(const std::string &spec_path,
                                     std::string *error) {
  if (spec_path.empty()) {
    setError(error, "model spec path is empty");
    return std::string();
  }
  ModelDescriptor descriptor;
  if (!loadModelDescriptor(spec_path, &descriptor, error)) {
    return std::string();
  }
  return resolveModelPath(descriptor);
}

bool fileExists(const std::string &path) {
  std::ifstream ifs(path, std::ios::binary);
  return static_cast<bool>(ifs);
}

}  // namespace

class SpeechRecognizer::Impl {
 public:
  bool load(const Config &config, std::string *error) {
    reset();

    if (config.model_spec.empty()) {
      setError(error, "speech recognizer model_spec is empty");
      return false;
    }

    ModelDescriptor descriptor;
    if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
      return false;
    }

    const auto encoder_it = descriptor.extra.find("encoder_model");
    const auto decoder_it = descriptor.extra.find("decoder_model");
    const auto joiner_it = descriptor.extra.find("joiner_model");
    const auto tokens_it = descriptor.extra.find("tokens");
    if (encoder_it == descriptor.extra.end() ||
        decoder_it == descriptor.extra.end() ||
        joiner_it == descriptor.extra.end() ||
        tokens_it == descriptor.extra.end()) {
      setError(error,
               "speech recognizer spec requires encoder_model / decoder_model / "
               "joiner_model / tokens");
      return false;
    }

    const std::string encoder_path =
        resolveModelPathFromSpec(joinPath(descriptor.descriptor_dir, encoder_it->second),
                                 error);
    if (encoder_path.empty()) {
      return false;
    }
    const std::string decoder_path =
        resolveModelPathFromSpec(joinPath(descriptor.descriptor_dir, decoder_it->second),
                                 error);
    if (decoder_path.empty()) {
      return false;
    }
    const std::string joiner_path =
        resolveModelPathFromSpec(joinPath(descriptor.descriptor_dir, joiner_it->second),
                                 error);
    if (joiner_path.empty()) {
      return false;
    }
    const std::string tokens_path =
        joinPath(descriptor.descriptor_dir, tokens_it->second);
    if (!fileExists(tokens_path)) {
      setError(error, "speech recognizer tokens.txt not found: " + tokens_path);
      return false;
    }

    if (!config.firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.firmware.c_str(), 0);
    }

    handle_ = TDL_CreateHandle(0);
    if (!handle_) {
      setError(error, "TDL_CreateHandle failed");
      return false;
    }

    int ret = TDL_OpenModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_ENCODER,
                            encoder_path.c_str(), nullptr, 0);
    if (ret != 0) {
      setError(error, "TDL_OpenModel(encoder) failed, ret=" + std::to_string(ret));
      reset();
      return false;
    }
    ret = TDL_OpenModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_DECODER,
                        decoder_path.c_str(), nullptr, 0);
    if (ret != 0) {
      setError(error, "TDL_OpenModel(decoder) failed, ret=" + std::to_string(ret));
      reset();
      return false;
    }
    ret = TDL_OpenModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_JOINER,
                        joiner_path.c_str(), nullptr, 0);
    if (ret != 0) {
      setError(error, "TDL_OpenModel(joiner) failed, ret=" + std::to_string(ret));
      reset();
      return false;
    }

    ret = TDL_SpeechRecognition_Init(
        handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_ENCODER,
        TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_DECODER,
        TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_JOINER, tokens_path.c_str());
    if (ret != 0) {
      setError(error, "TDL_SpeechRecognition_Init failed, ret=" +
                          std::to_string(ret));
      reset();
      return false;
    }
    return true;
  }

  bool run(const std::vector<std::uint8_t> &pcm16le_mono,
           SpeechRecognitionResult *result, std::string *error) {
    if (!handle_) {
      setError(error, "speech recognizer is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "speech result pointer is null");
      return false;
    }
    if (pcm16le_mono.empty()) {
      setError(error, "speech audio buffer is empty");
      return false;
    }

    // The CV184X C API sample feeds one second of PCM per call. Keep the
    // handle alive across chunks so Zipformer retains its streaming state.
    constexpr std::size_t kChunkBytes = 16000 * 2;
    std::vector<std::uint8_t> padded_pcm = pcm16le_mono;
    result->clear();
    const std::size_t tail_bytes =
        (kChunkBytes - padded_pcm.size() % kChunkBytes) % kChunkBytes;
    padded_pcm.insert(padded_pcm.end(), tail_bytes, 0);

    for (std::size_t offset = 0; offset < padded_pcm.size();
         offset += kChunkBytes) {
      TDLImage audio_frame = TDL_ReadAudioFrame(
          padded_pcm.data() + offset, static_cast<int>(kChunkBytes));
      if (!audio_frame) {
        setError(error, "TDL_ReadAudioFrame failed");
        return false;
      }

      TDLText text_meta;
      std::memset(&text_meta, 0, sizeof(text_meta));
      const int ret = TDL_SpeechRecognition(
          handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_ENCODER,
          audio_frame, &text_meta);
      TDL_DestroyImage(audio_frame);
      if (ret != 0) {
        setError(error, "TDL_SpeechRecognition failed, ret=" +
                            std::to_string(ret));
        return false;
      }
      if (text_meta.text_info) {
        result->text += text_meta.text_info;
      }
      TDL_ReleaseCharacterMeta(&text_meta);
    }
    return true;
  }

  void reset() {
    if (handle_) {
      TDL_CloseModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_ENCODER);
      TDL_CloseModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_DECODER);
      TDL_CloseModel(handle_, TDL_MODEL_RECOGNITION_SPEECH_ZIPFORMER_JOINER);
      TDL_DestroyHandle(handle_);
      handle_ = nullptr;
    }
  }

  bool initialized() const { return handle_ != nullptr; }

 private:
  std::string joinPath(const std::string &base, const std::string &path) const {
    if (path.empty()) {
      return path;
    }
    if (path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\')) {
      return path;
    }
    if (base.empty()) {
      return path;
    }
    const char sep =
        base.back() == '/' || base.back() == '\\' ? '\0' : '/';
    return sep == '\0' ? base + path : base + sep + path;
  }

  TDLHandle handle_ = nullptr;
};

SpeechRecognizer::SpeechRecognizer() = default;

SpeechRecognizer::~SpeechRecognizer() {
  reset();
  delete impl_;
}

SpeechRecognizer::SpeechRecognizer(SpeechRecognizer &&other) noexcept
    : config_(std::move(other.config_)), impl_(other.impl_) {
  other.impl_ = nullptr;
}

SpeechRecognizer &SpeechRecognizer::operator=(SpeechRecognizer &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  delete impl_;
  config_ = std::move(other.config_);
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool SpeechRecognizer::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, error);
}

bool SpeechRecognizer::load(const std::string &model_spec, std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool SpeechRecognizer::load(const std::string &model_spec,
                            const std::string &firmware,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool SpeechRecognizer::load(const std::string &model_spec,
                            const std::string &firmware,
                            const std::string &model_dir,
                            std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool SpeechRecognizer::run(const std::vector<std::uint8_t> &pcm16le_mono,
                           SpeechRecognitionResult *result,
                           std::string *error) {
  if (!impl_) {
    setError(error, "speech recognizer is not initialized");
    return false;
  }
  return impl_->run(pcm16le_mono, result, error);
}

bool SpeechRecognizer::run(const AudioFrame &frame,
                           SpeechRecognitionResult *result,
                           std::string *error) {
  if (frame.channels.empty()) {
    setError(error, "speech audio frame is empty");
    return false;
  }
  return run(frame.channels.front(), result, error);
}

bool SpeechRecognizer::initialized() const {
  return impl_ && impl_->initialized();
}

void SpeechRecognizer::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
