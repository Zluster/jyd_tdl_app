#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "tdl_app/frame_source.hpp"

namespace tdl_app {

class VdecChannel {
 public:
  enum class Codec {
    H264,
    H265,
    Jpeg,
    Mjpeg,
  };

  enum class Mode {
    Stream,
    Frame,
    Compat,
  };

  struct Config {
    int channel = 0;
    Codec codec = Codec::H264;
    Mode mode = Mode::Stream;
    int width = 1920;
    int height = 1080;
    int output_pixel_format = 18;
    int display_frame_count = 2;
    int stream_buffer_size = 0;
    int frame_buffer_size = 0;
    int frame_buffer_count = 6;
    int compress_mode = 0;
    int command_queue_depth = 0;
    bool reorder_enable = true;
    int timeout_ms = 1000;
    int picture_pool_id = -1;
    int tmv_pool_id = -1;
  };

  static Config h264(int channel = 0, int width = 1920, int height = 1080,
                     Mode mode = Mode::Stream, int output_pixel_format = 18,
                     int timeout_ms = 1000) {
    Config config;
    config.channel = channel;
    config.codec = Codec::H264;
    config.mode = mode;
    config.width = width;
    config.height = height;
    config.output_pixel_format = output_pixel_format;
    config.timeout_ms = timeout_ms;
    return config;
  }

  static Config h265(int channel = 0, int width = 1920, int height = 1080,
                     Mode mode = Mode::Stream, int output_pixel_format = 18,
                     int timeout_ms = 1000) {
    Config config = h264(channel, width, height, mode, output_pixel_format,
                         timeout_ms);
    config.codec = Codec::H265;
    return config;
  }

  static Config jpeg(int channel = 0, int width = 1920, int height = 1080,
                     Mode mode = Mode::Frame, int output_pixel_format = 18,
                     int timeout_ms = 1000) {
    Config config = h264(channel, width, height, mode, output_pixel_format,
                         timeout_ms);
    config.codec = Codec::Jpeg;
    return config;
  }

  static Config mjpeg(int channel = 0, int width = 1920, int height = 1080,
                      Mode mode = Mode::Stream, int output_pixel_format = 18,
                      int timeout_ms = 1000) {
    Config config = h264(channel, width, height, mode, output_pixel_format,
                         timeout_ms);
    config.codec = Codec::Mjpeg;
    return config;
  }

  struct StreamPacket {
    const void *data = nullptr;
    std::size_t size = 0;
    std::uint64_t pts = 0;
    std::uint64_t dts = 0;
    bool end_of_frame = true;
    bool end_of_stream = false;
    bool display = true;
  };

  struct Status {
    int left_stream_bytes = 0;
    int left_stream_frames = 0;
    int left_pics = 0;
    bool started = false;
    std::uint32_t received_frames = 0;
    std::uint32_t decoded_frames = 0;
    int width = 0;
    int height = 0;
  };

  VdecChannel();
  explicit VdecChannel(const Config &config);
  ~VdecChannel();

  VdecChannel(const VdecChannel &) = delete;
  VdecChannel &operator=(const VdecChannel &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool sendStream(const StreamPacket &packet, std::string *error = nullptr);
  bool read(Frame *frame, std::string *error = nullptr);
  void releaseFrame();
  bool queryStatus(Status *status, std::string *error = nullptr) const;

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
