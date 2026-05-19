#pragma once

#include <string>

#include "cvi_common.h"

namespace tdl_app {

class AudioSystem {
 public:
  AudioSystem() = default;
  ~AudioSystem();

  AudioSystem(const AudioSystem &) = delete;
  AudioSystem &operator=(const AudioSystem &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  static bool bind(const MMF_CHN_S &source, const MMF_CHN_S &dest,
                   std::string *error = nullptr);
  static bool unbind(const MMF_CHN_S &source, const MMF_CHN_S &dest,
                     std::string *error = nullptr);

 private:
  bool opened_ = false;
};

}  // namespace tdl_app
