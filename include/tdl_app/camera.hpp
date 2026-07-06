#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tdl_app/frame_source.hpp"
#include "tdl_app/layout.hpp"

namespace tdl_app {

enum class CameraSourceId {
  Ai,
  Live,
  Screen,
  Main,
  SubRgb,
};

struct CameraFrameInfo {
  int width = 0;
  int height = 0;
  int pixel_format = 0;
  int stride[3] = {0, 0, 0};
  std::uint64_t sequence = 0;
  std::uint64_t timestamp_us = 0;
};

class Camera {
 public:
  using Backend = CameraSource::Backend;
  using Config = CameraSource::Config;

  Camera();
  explicit Camera(const Config &config);
  ~Camera();

  Camera(const Camera &) = delete;
  Camera &operator=(const Camera &) = delete;
  Camera(Camera &&other) noexcept;
  Camera &operator=(Camera &&other) noexcept;

  bool open(std::string *error = nullptr);
  bool open(CameraSourceId source, std::string *error = nullptr);
  bool read(Frame *frame, std::string *error = nullptr);
  bool readInfo(CameraFrameInfo *info, std::string *error = nullptr);
  bool snapshot(const std::string &path, std::string *error = nullptr);
  bool dumpFrame(const std::string &path, CameraFrameInfo *info = nullptr,
                 std::size_t *bytes_written = nullptr,
                 std::string *error = nullptr);
  bool dumpFrameBmp(const std::string &path, CameraFrameInfo *info = nullptr,
                    std::string *error = nullptr);
  void close();

  static Config vpss(int group = 0, int channel = 0, int width = 640,
                     int height = 640, int pixel_format = PixelFormat::NV12,
                     int timeout_ms = 1000) {
    Config config;
    config.backend = Backend::Vpss;
    config.group = group;
    config.channel = channel;
    config.width = width;
    config.height = height;
    config.pixel_format = pixel_format;
    config.timeout_ms = timeout_ms;
    return config;
  }

  static Config vi(int pipe = 0, int channel = 0, int width = 1920,
                   int height = 1080,
                   int pixel_format = PixelFormat::NV12,
                   int timeout_ms = 1000) {
    Config config;
    config.backend = Backend::Vi;
    config.pipe = pipe;
    config.channel = channel;
    config.width = width;
    config.height = height;
    config.pixel_format = pixel_format;
    config.timeout_ms = timeout_ms;
    return config;
  }

  std::unique_ptr<FrameSource> createSource() const;
  const Config &config() const { return config_; }
  void setConfig(const Config &config) { config_ = config; }

  static Config forSource(CameraSourceId source, int timeout_ms = 1000);
  static const char *sourceName(CameraSourceId source);

  static Config ai(int timeout_ms = 1000) {
    return vpss(DualOsLayout::kCaptureVpssGroup, DualOsLayout::kAiChannel,
                DualOsLayout::kAiWidth, DualOsLayout::kAiHeight,
                DualOsLayout::kAiPixelFormat, timeout_ms);
  }

  static Config live(int timeout_ms = 1000) {
    return vpss(DualOsLayout::kCaptureVpssGroup, DualOsLayout::kLiveChannel,
                DualOsLayout::kLiveWidth, DualOsLayout::kLiveHeight,
                DualOsLayout::kLivePixelFormat, timeout_ms);
  }

  static Config screen(int timeout_ms = 1000) {
    return vpss(DualOsLayout::kDisplayVpssGroup, DualOsLayout::kDisplayChannel,
                DualOsLayout::kDisplaySourceWidth,
                DualOsLayout::kDisplaySourceHeight,
                DualOsLayout::kDisplayPixelFormat, timeout_ms);
  }

  static Config mainFrame(int timeout_ms = 1000) {
    return vpss(DualOsLayout::kCaptureVpssGroup, DualOsLayout::kMainChannel,
                DualOsLayout::kMainWidth, DualOsLayout::kMainHeight,
                DualOsLayout::kMainPixelFormat, timeout_ms);
  }

  static Config subRgb(int timeout_ms = 1000) {
    return vpss(DualOsLayout::kCaptureVpssGroup, DualOsLayout::kSubRgbChannel,
                DualOsLayout::kSubRgbWidth, DualOsLayout::kSubRgbHeight,
                DualOsLayout::kSubRgbPixelFormat, timeout_ms);
  }

 private:
  Config config_;
  std::unique_ptr<FrameSource> source_;
};

}  // namespace tdl_app

