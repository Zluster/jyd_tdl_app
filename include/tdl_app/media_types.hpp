#pragma once

#include <cstdint>

namespace tdl_app {

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
  int pixel_format = 18;
  int block_count = 8;
  int align = 64;
  bool cached = true;
  int remap_mode = -1;
};

struct VpssGroupConfig {
  int group = 0;
  MediaSize max_size {1920, 1080};
  int pixel_format = 18;
  int device = 1;
  MediaFrameRate frame_rate;
};

struct VpssChannelConfig {
  int channel = 0;
  MediaSize output_size {640, 640};
  int pixel_format = 18;
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
