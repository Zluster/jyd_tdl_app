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
  bool isBound() const;

 private:
  Config config_;
  bool bound_ = false;
};

}  // namespace tdl_app
