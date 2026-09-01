#pragma once

#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class MediaLink {
 public:
  struct Config {
    MediaChannel source;
    MediaChannel destination;
  };

  MediaLink();
  explicit MediaLink(const Config &config);
  ~MediaLink();

  MediaLink(const MediaLink &) = delete;
  MediaLink &operator=(const MediaLink &) = delete;

  bool bind(std::string *error = nullptr);
  void unbind();
  // Unbinds the (source, destination) pair unconditionally. CVI_SYS_UnBind is
  // keyed by the channel pair only, so this also removes binds created by
  // other processes (e.g. residue left behind by a killed host).
  void forceUnbind();
  bool isBound() const;

 private:
  Config config_;
  bool bound_ = false;
};

}  // namespace tdl_app
