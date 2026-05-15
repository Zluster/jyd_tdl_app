#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tdl_app {

struct Frame;

class VencChannel {
 public:
  enum class Codec {
    H264,
    H265,
  };

  struct Config {
    int channel = 0;
    int width = 0;
    int height = 0;
    Codec codec = Codec::H264;
    int src_fps = 25;
    int dst_fps = 25;
    int bitrate_kbps = 1024;
    int gop = 25;
  };

  static Config h264(int channel = 0, int width = 1920, int height = 1080,
                     int bitrate_kbps = 1024, int src_fps = 25,
                     int dst_fps = 25, int gop = 25) {
    Config config;
    config.channel = channel;
    config.width = width;
    config.height = height;
    config.codec = Codec::H264;
    config.bitrate_kbps = bitrate_kbps;
    config.src_fps = src_fps;
    config.dst_fps = dst_fps;
    config.gop = gop;
    return config;
  }

  static Config h265(int channel = 0, int width = 1920, int height = 1080,
                     int bitrate_kbps = 1024, int src_fps = 25,
                     int dst_fps = 25, int gop = 25) {
    Config config = h264(channel, width, height, bitrate_kbps, src_fps,
                         dst_fps, gop);
    config.codec = Codec::H265;
    return config;
  }

  struct EncodedPacket {
    std::vector<std::vector<std::uint8_t>> blocks;
  };

  VencChannel();
  explicit VencChannel(const Config &config);
  ~VencChannel();

  VencChannel(const VencChannel &) = delete;
  VencChannel &operator=(const VencChannel &) = delete;

  bool open(std::string *error = nullptr);
  bool encode(const Frame &frame, EncodedPacket *packet,
              std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
