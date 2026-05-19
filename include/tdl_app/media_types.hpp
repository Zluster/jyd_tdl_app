#pragma once

#include <cstdint>

namespace tdl_app {

struct PixelFormat {
  static constexpr int RGB888 = 0;
  static constexpr int BGR888 = 1;
  static constexpr int RGB888_PLANAR = 2;
  static constexpr int BGR888_PLANAR = 3;
  static constexpr int ARGB1555 = 4;
  static constexpr int ARGB4444 = 5;
  static constexpr int ARGB8888 = 6;
  static constexpr int YUV400 = 15;
  static constexpr int NV12 = 18;
  static constexpr int NV21 = 19;
};

struct VoInterfaceType {
  static constexpr int Bt656 = (0x01L << 7);
  static constexpr int Bt1120 = (0x01L << 8);
  static constexpr int ParallelRgb = (0x01L << 9);
  static constexpr int SerialRgb = (0x01L << 10);
  static constexpr int I80 = (0x01L << 11);
  static constexpr int HwMcu = (0x01L << 12);
  static constexpr int Mipi = (0x01L << 13);
  static constexpr int Lvds = (0x01L << 14);
};

struct VoInterfaceSync {
  static constexpr int P720_1280_60 = 21;
  static constexpr int P1080_1920_60 = 22;
  static constexpr int P480_800_60 = 23;
  static constexpr int P440_1920_60 = 24;
  static constexpr int P480_640_60 = 25;
};

struct GraphicBufferFormat {
  static constexpr int Argb8888 = 0;
  static constexpr int Argb4444 = 1;
  static constexpr int Argb1555 = 2;
};

enum class MediaModule {
  Vi = 0,
  Vpss = 1,
  Venc = 2,
  Vo = 3,
  Rgn = 4,
  Vdec = 5,
  Unknown = 255,
};

struct MediaSize {
  int width = 0;
  int height = 0;

  static MediaSize make(int w, int h) {
    MediaSize size;
    size.width = w;
    size.height = h;
    return size;
  }
};

struct MediaFrameRate {
  int src_fps = -1;
  int dst_fps = -1;
};

struct MediaChannel {
  MediaModule module = MediaModule::Unknown;
  int device = 0;
  int channel = 0;

  static MediaChannel vi(int pipe = 0, int channel = 0) {
    MediaChannel out;
    out.module = MediaModule::Vi;
    out.device = pipe;
    out.channel = channel;
    return out;
  }

  static MediaChannel vpss(int group = 0, int channel = 0) {
    MediaChannel out;
    out.module = MediaModule::Vpss;
    out.device = group;
    out.channel = channel;
    return out;
  }

  static MediaChannel venc(int channel = 0) {
    MediaChannel out;
    out.module = MediaModule::Venc;
    out.channel = channel;
    return out;
  }

  static MediaChannel vo(int device = 0, int channel = 0) {
    MediaChannel out;
    out.module = MediaModule::Vo;
    out.device = device;
    out.channel = channel;
    return out;
  }

  static MediaChannel vdec(int channel = 0) {
    MediaChannel out;
    out.module = MediaModule::Vdec;
    out.channel = channel;
    return out;
  }
};

struct VideoBufferPoolConfig {
  MediaSize size {1920, 1080};
  int pixel_format = PixelFormat::NV12;
  int block_count = 8;
  int align = 64;
  bool cached = true;
  int remap_mode = -1;
};

struct VpssGroupConfig {
  int group = 0;
  MediaSize max_size {1920, 1080};
  int pixel_format = PixelFormat::NV12;
  int device = 1;
  MediaFrameRate frame_rate;
};

struct VpssChannelConfig {
  int channel = 0;
  MediaSize output_size {640, 640};
  int pixel_format = PixelFormat::NV12;
  MediaFrameRate frame_rate;
  int depth = 0;
  bool mirror = false;
  bool flip = false;
  bool normalize = false;
};

struct OverlayRegionConfig {
  int handle = 0;
  MediaSize size {0, 0};
  int pixel_format = 24;
  int canvas_count = 2;
  std::uint32_t bg_color = 0;
};

struct OverlayBitmap {
  const void *data = nullptr;
  MediaSize size {0, 0};
  int pixel_format = 24;
};

}  // namespace tdl_app
