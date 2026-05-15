#pragma once

#include <string>

namespace tdl_app {

class VoOutput {
 public:
  struct Config {
    int device = 0;
    int layer = 0;
    int channel = 0;
    int width = 640;
    int height = 640;
    int pixel_format = 18;
    int interface_type = 1;
    int interface_sync = 39;
    int display_buf_len = 3;
  };

  static Config display(int device = 0, int layer = 0, int channel = 0,
                        int width = 640, int height = 640,
                        int pixel_format = 18, int interface_type = 1,
                        int interface_sync = 39, int display_buf_len = 3) {
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
