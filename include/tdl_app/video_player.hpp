#pragma once

#include <cstdint>
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

  // Starts playback and takes over the existing display VPSS input.  The
  // container may also carry audio; it is decoded to the board's stable
  // 16 kHz mono PCM output path.
  bool play(const std::string &path, bool loop = false,
            std::string *error = nullptr);
  void pause();
  void resume();
  void stop();
  // Releases the display path and restores the source active before playback.
  void close();

  std::string state() const;
  std::string lastError() const;
  bool isPlaying() const;
  int width() const;
  int height() const;
  std::int64_t durationMs() const;
  std::int64_t positionMs() const;
  bool hasAudio() const;
  // CV184X AO uses an integer hardware level in the range [0, 32].
  bool setVolume(int volume_level, std::string *error = nullptr);
  int volume() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
