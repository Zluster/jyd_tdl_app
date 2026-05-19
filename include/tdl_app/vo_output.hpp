#pragma once

#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class VoOutput {
 public:
  struct Config {
    int device = 0;
    int layer = 0;
    int channel = 0;
    int width = 640;
    int height = 640;
    int pixel_format = PixelFormat::NV12;
    int interface_type = VoInterfaceType::Mipi;
    int interface_sync = VoInterfaceSync::P720_1280_60;
    int display_buf_len = 3;
    int frame_rate = 25;
    int channel_x = 0;
    int channel_y = 0;
    int priority = 0;
  };

  static Config display(int device = 0, int layer = 0, int channel = 0,
                        int width = 640, int height = 640,
                        int pixel_format = PixelFormat::NV12,
                        int interface_type = VoInterfaceType::Mipi,
                        int interface_sync = VoInterfaceSync::P720_1280_60,
                        int display_buf_len = 3) {
    Config config;
    config.device = device;
    config.layer = layer;
    config.channel = channel;
    config.width = width;
    config.height = height;
    config.pixel_format = pixel_format;
    config.interface_type = interface_type;
    config.interface_sync = interface_sync;
    config.display_buf_len = display_buf_len;
    return config;
  }

  static Config argb8888Display(int device = 0, int layer = 0, int channel = 0,
                                int width = 720, int height = 1280,
                                int interface_type = VoInterfaceType::Mipi,
                                int interface_sync = VoInterfaceSync::P720_1280_60,
                                int display_buf_len = 2) {
    Config config = display(device, layer, channel, width, height,
                            PixelFormat::ARGB8888, interface_type,
                            interface_sync, display_buf_len);
    return config;
  }

  VoOutput();
  explicit VoOutput(const Config &config);
  ~VoOutput();

  VoOutput(const VoOutput &) = delete;
  VoOutput &operator=(const VoOutput &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool channel_enabled_ = false;
  bool layer_enabled_ = false;
  bool device_enabled_ = false;
};

}  // namespace tdl_app
