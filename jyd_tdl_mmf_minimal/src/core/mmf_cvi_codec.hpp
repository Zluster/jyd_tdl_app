#ifndef MMF_CVI_CODEC_HPP
#define MMF_CVI_CODEC_HPP

#include "mmf_cvi_base.hpp"

namespace mmf_cvi {

class VencChannel {
 public:
  enum class Codec { H264, H265, Mjpeg };
  struct Config {
    int channel = 0;
    Codec codec = Codec::Mjpeg;
    int width = 1280;
    int height = 720;
    int bitrate_kbps = 0;
    int gop = 25;
    int src_fps = 25;
    int dst_fps = 25;
    int qfactor = 80;
  };
  struct EncodedPacket {
    std::vector<std::vector<uint8_t>> blocks;
    bool key_frame = false;
  };
  static Config mjpeg(int channel, int width, int height, int bitrate_kbps, int gop, int fps,
                      int qfactor) {
    Config c;
    c.channel = channel;
    c.codec = Codec::Mjpeg;
    c.width = width;
    c.height = height;
    c.bitrate_kbps = bitrate_kbps;
    c.gop = gop;
    c.src_fps = fps;
    c.dst_fps = fps;
    c.qfactor = qfactor;
    return c;
  }
  VencChannel();
  explicit VencChannel(const Config& config);
  ~VencChannel();
  bool open(std::string* error = nullptr);
  bool encode(const Frame& frame, EncodedPacket* packet, std::string* error = nullptr);
  void close();

 private:
  Config config_;
  bool opened_ = false;
};
class VdecChannel {
 public:
  enum class Codec { H264, H265, Jpeg, Mjpeg };
  enum class Mode { Stream, Frame, Compat };
  struct Config {
    int channel = 0;
    Codec codec = Codec::Jpeg;
    Mode mode = Mode::Frame;
    int width = 1920;
    int height = 1080;
    int output_pixel_format = PixelFormat::NV21;
    int timeout_ms = 1000;
    int frame_buffer_count = 1;
    int display_frame_count = 0;
    int stream_buffer_size = 0;
    int frame_buffer_size = 0;
    int compress_mode = COMPRESS_MODE_NONE;
    int command_queue_depth = 4;
    bool reorder_enable = false;
    int picture_pool_id = -1;
    int tmv_pool_id = -1;
  };
  struct StreamPacket {
    const void* data = nullptr;
    size_t size = 0;
    uint64_t pts = 0;
    uint64_t dts = 0;
    bool end_of_frame = true;
    bool end_of_stream = false;
    bool display = true;
  };
  static Config jpeg(int channel, int width, int height, Mode mode, int output_format) {
    Config c;
    c.channel = channel;
    c.codec = Codec::Jpeg;
    c.mode = mode;
    c.width = width;
    c.height = height;
    c.output_pixel_format = output_format;
    return c;
  }
  VdecChannel();
  explicit VdecChannel(const Config& config);
  ~VdecChannel();
  bool open(std::string* error = nullptr);
  bool sendStream(const StreamPacket& packet, std::string* error = nullptr);
  bool read(Frame* frame, std::string* error = nullptr);
  void releaseFrame();
  void close();

 private:
  Config config_;
  VIDEO_FRAME_INFO_S frame_info_{};
  bool frame_valid_ = false;
  bool opened_ = false;
  bool created_ = false;
  bool started_ = false;
};

}  // namespace mmf_cvi

#endif  // MMF_CVI_CODEC_HPP
