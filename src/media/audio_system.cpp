#include "tdl_app/audio_system.hpp"

#include <string>

#include "cvi_audio.h"
#include "media/private/audio_runtime.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

AudioSystem::~AudioSystem() { close(); }

bool AudioSystem::open(std::string *error) {
  if (opened_) {
    return true;
  }
  if (!retainAudioRuntime(error)) {
    return false;
  }
  opened_ = true;
  return true;
}

void AudioSystem::close() {
  if (!opened_) {
    return;
  }
  releaseAudioRuntime();
  opened_ = false;
}

bool AudioSystem::isOpen() const { return opened_; }

bool AudioSystem::bind(const MMF_CHN_S &source, const MMF_CHN_S &dest,
                       std::string *error) {
  const int ret = CVI_AUD_SYS_Bind(&source, &dest);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AUD_SYS_Bind failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioSystem::unbind(const MMF_CHN_S &source, const MMF_CHN_S &dest,
                         std::string *error) {
  const int ret = CVI_AUD_SYS_UnBind(&source, &dest);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AUD_SYS_UnBind failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

}  // namespace tdl_app
