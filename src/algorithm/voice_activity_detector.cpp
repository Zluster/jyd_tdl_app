#include "tdl_app/voice_activity_detector.hpp"

#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

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

std::string resolveModelPathFromConfig(const ModelSessionConfig &config,
                                       std::string *error) {
  if (config.model_spec.empty()) {
    setError(error, "vad model_spec is empty");
    return std::string();
  }
  ModelDescriptor descriptor;
  if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
    return std::string();
  }
  return resolveModelPath(descriptor);
}

}  // namespace

class VoiceActivityDetector::Impl {
 public:
  bool load(const Config &config, std::string *error) {
    reset();

    const std::string model_path = resolveModelPathFromConfig(config, error);
    if (model_path.empty()) {
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

    const int ret = TDL_OpenModel(handle_, TDL_MODEL_VAD_FSMN,
                                  model_path.c_str(), nullptr, 0);
    if (ret != 0) {
      setError(error, "TDL_OpenModel(VAD_FSMN) failed, ret=" +
                          std::to_string(ret));
      reset();
      return false;
    }
    return true;
  }

  bool run(const std::vector<std::uint8_t> &pcm16le_mono, bool is_final,
           VoiceActivityResult *result, std::string *error) {
    if (!handle_) {
      setError(error, "voice activity detector is not initialized");
      return false;
    }
    if (!result) {
      setError(error, "vad result pointer is null");
      return false;
    }
    if (pcm16le_mono.empty() && !is_final) {
      setError(error, "vad audio buffer is empty");
      return false;
    }

    TDLImage audio_frame = nullptr;
    if (!pcm16le_mono.empty()) {
      audio_frame = TDL_ReadAudioFrame(
          const_cast<std::uint8_t *>(pcm16le_mono.data()),
          static_cast<int>(pcm16le_mono.size()));
      if (!audio_frame) {
        setError(error, "TDL_ReadAudioFrame failed");
        return false;
      }
    }

    TDLVAD vad_meta;
    std::memset(&vad_meta, 0, sizeof(vad_meta));
    if (TDL_InitVADMeta(&vad_meta, 16) != 0) {
      if (audio_frame) {
        TDL_DestroyImage(audio_frame);
      }
      setError(error, "TDL_InitVADMeta failed");
      return false;
    }

    const int ret = TDL_VoiceActivityDetection(handle_, TDL_MODEL_VAD_FSMN,
                                               audio_frame, is_final ? 1 : 0,
                                               &vad_meta);
    if (audio_frame) {
      TDL_DestroyImage(audio_frame);
    }
    if (ret != 0) {
      TDL_ReleaseVADMeta(&vad_meta);
      setError(error, "TDL_VoiceActivityDetection failed, ret=" +
                          std::to_string(ret));
      return false;
    }

    result->clear();
    result->has_speech = vad_meta.has_speech;
    result->start_event = vad_meta.start_event;
    result->end_event = vad_meta.end_event;
    result->segments.reserve(vad_meta.size);
    for (std::uint32_t i = 0; i < vad_meta.size; ++i) {
      VoiceSegment segment;
      segment.start_ms = vad_meta.segments[i].start_ms;
      segment.end_ms = vad_meta.segments[i].end_ms;
      result->segments.push_back(segment);
    }
    TDL_ReleaseVADMeta(&vad_meta);
    return true;
  }

  void reset() {
    if (handle_) {
      TDL_CloseModel(handle_, TDL_MODEL_VAD_FSMN);
      TDL_DestroyHandle(handle_);
      handle_ = nullptr;
    }
  }

  bool initialized() const { return handle_ != nullptr; }

 private:
  TDLHandle handle_ = nullptr;
};

VoiceActivityDetector::VoiceActivityDetector() = default;

VoiceActivityDetector::~VoiceActivityDetector() {
  reset();
  delete impl_;
}

VoiceActivityDetector::VoiceActivityDetector(VoiceActivityDetector &&other) noexcept
    : config_(std::move(other.config_)), impl_(other.impl_) {
  other.impl_ = nullptr;
}

VoiceActivityDetector &VoiceActivityDetector::operator=(
    VoiceActivityDetector &&other) noexcept {
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

bool VoiceActivityDetector::load(const Config &config, std::string *error) {
  config_ = config;
  if (!impl_) {
    impl_ = new Impl;
  }
  return impl_->load(config_, error);
}

bool VoiceActivityDetector::load(const std::string &model_spec,
                                 std::string *error) {
  Config config;
  config.model_spec = model_spec;
  return load(config, error);
}

bool VoiceActivityDetector::load(const std::string &model_spec,
                                 const std::string &firmware,
                                 std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  return load(config, error);
}

bool VoiceActivityDetector::load(const std::string &model_spec,
                                 const std::string &firmware,
                                 const std::string &model_dir,
                                 std::string *error) {
  Config config;
  config.model_spec = model_spec;
  config.firmware = firmware;
  config.model_dir = model_dir;
  return load(config, error);
}

bool VoiceActivityDetector::run(const std::vector<std::uint8_t> &pcm16le_mono,
                                bool is_final, VoiceActivityResult *result,
                                std::string *error) {
  if (!impl_) {
    setError(error, "voice activity detector is not initialized");
    return false;
  }
  return impl_->run(pcm16le_mono, is_final, result, error);
}

bool VoiceActivityDetector::run(const AudioFrame &frame, bool is_final,
                                VoiceActivityResult *result,
                                std::string *error) {
  if (frame.channels.empty()) {
    setError(error, "vad audio frame is empty");
    return false;
  }
  return run(frame.channels.front(), is_final, result, error);
}

bool VoiceActivityDetector::initialized() const {
  return impl_ && impl_->initialized();
}

void VoiceActivityDetector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

}  // namespace tdl_app
