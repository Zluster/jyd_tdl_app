#include "audio_runtime.hpp"

#include <mutex>
#include <string>

#include "cvi_audio.h"

namespace tdl_app {
namespace {

std::mutex &audioMutex() {
  static std::mutex mutex;
  return mutex;
}

int &audioRefCount() {
  static int ref_count = 0;
  return ref_count;
}

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

bool retainAudioRuntime(std::string *error) {
  std::lock_guard<std::mutex> lock(audioMutex());
  if (audioRefCount() > 0) {
    ++audioRefCount();
    return true;
  }

  const int ret = CVI_AUDIO_INIT();
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AUDIO_INIT failed, ret=" + std::to_string(ret));
    return false;
  }

  audioRefCount() = 1;
  return true;
}

void releaseAudioRuntime() {
  std::lock_guard<std::mutex> lock(audioMutex());
  if (audioRefCount() <= 0) {
    audioRefCount() = 0;
    return;
  }

  --audioRefCount();
  if (audioRefCount() == 0) {
    CVI_AUDIO_DEINIT();
  }
}

}  // namespace tdl_app
