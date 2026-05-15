#pragma once

#include <memory>
#include <string>

#include "tdl_app/frame_source.hpp"

namespace tdl_app {

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
  bool read(Frame *frame, std::string *error = nullptr);
  void close();

  static Config vpss(int group = 0, int channel = 0, int width = 640,
                     int height = 640, int pixel_format = 18,
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
                   int height = 1080, int pixel_format = 18,
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

 private:
  Config config_;
  std::unique_ptr<FrameSource> source_;
};

}  // namespace tdl_app
