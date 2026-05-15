#pragma once

#include <memory>
#include <string>

#include "tdl_app/frame_sink.hpp"

namespace tdl_app {

class RtspStreamer {
 public:
  using Codec = RtspFrameSink::Codec;

  struct Config {
    int venc_channel = 0;
    int width = 0;
    int height = 0;
    Codec codec = Codec::H264;
  };

  static Config h264(int venc_channel = 0, int width = 1920,
                     int height = 1080) {
    Config config;
    config.venc_channel = venc_channel;
    config.width = width;
    config.height = height;
    config.codec = Codec::H264;
    return config;
  }

  static Config h265(int venc_channel = 0, int width = 1920,
                     int height = 1080) {
    Config config = h264(venc_channel, width, height);
    config.codec = Codec::H265;
    return config;
  }

  RtspStreamer();
  explicit RtspStreamer(const Config &config);
  ~RtspStreamer();

  RtspStreamer(const RtspStreamer &) = delete;
  RtspStreamer &operator=(const RtspStreamer &) = delete;

  bool open(std::string *error = nullptr);
  bool write(const Frame &frame, const AlgorithmResult &result,
             std::string *error = nullptr);
  void close();

  std::unique_ptr<FrameSink> createSink() const;

 private:
  Config config_;
  std::unique_ptr<FrameSink> sink_;
};

}  // namespace tdl_app
