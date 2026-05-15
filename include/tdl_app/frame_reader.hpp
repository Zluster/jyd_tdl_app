#pragma once

#include <string>

#include "tdl_app/frame_source.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_app {

class FrameReader {
 public:
  struct Config {
    MediaChannel channel;
    int timeout_ms = 1000;
  };

  FrameReader();
  explicit FrameReader(const Config &config);
  ~FrameReader();

  FrameReader(const FrameReader &) = delete;
  FrameReader &operator=(const FrameReader &) = delete;

  bool open(std::string *error = nullptr);
  bool read(Frame *frame, std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
