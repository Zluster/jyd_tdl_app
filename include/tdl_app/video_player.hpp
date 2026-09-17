#pragma once

#include <memory>
#include <string>

namespace tdl_app {

// Local MP4/H.264 player. FFmpeg decodes to NV12 because the current CV184X
// small-core image only links JPEG VDEC; decoded frames use hardware VPSS/VO.
class VideoPlayer {
 public:
  VideoPlayer();
  ~VideoPlayer();

  VideoPlayer(const VideoPlayer &) = delete;
  VideoPlayer &operator=(const VideoPlayer &) = delete;

  // Starts playback and takes over the existing display VPSS input.
  // Version one accepts an H.264 video stream up to 720x480, with no audio.
  bool play(const std::string &path, bool loop = false,
            std::string *error = nullptr);
  void pause();
  void resume();
  void stop();

  std::string state() const;
  std::string lastError() const;
  bool isPlaying() const;
  int width() const;
  int height() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
