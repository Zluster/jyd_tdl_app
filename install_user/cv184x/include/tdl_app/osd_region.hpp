#pragma once

#include <cstdint>
#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

struct OsdCanvas {
  void *data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  int pixel_format = 0;
};

class OsdRegion {
 public:
  struct Config {
    int handle = 0;
    MediaSize size {0, 0};
    int pixel_format = 4;
    int canvas_count = 2;
    std::uint32_t bg_color = 0;
  };

  static Config canvas(int handle = 0, int width = 0, int height = 0,
                       int pixel_format = 4, int canvas_count = 2,
                       std::uint32_t bg_color = 0) {
    Config config;
    config.handle = handle;
    config.size.width = width;
    config.size.height = height;
    config.pixel_format = pixel_format;
    config.canvas_count = canvas_count;
    config.bg_color = bg_color;
    return config;
  }

  OsdRegion();
  explicit OsdRegion(const Config &config);
  ~OsdRegion();

  OsdRegion(const OsdRegion &) = delete;
  OsdRegion &operator=(const OsdRegion &) = delete;

  bool create(std::string *error = nullptr);
  bool attach(const MediaChannel &channel, int x, int y, int layer,
              std::string *error = nullptr);
  bool setBitmap(const OverlayBitmap &bitmap, std::string *error = nullptr);
  bool setVisible(bool visible, std::string *error = nullptr);
  bool moveTo(int x, int y, std::string *error = nullptr);
  bool getCanvas(OsdCanvas *canvas, std::string *error = nullptr);
  bool updateCanvas(std::string *error = nullptr);
  void detach();
  void destroy();
  bool isCreated() const;
  bool isAttached() const;
  int handle() const { return config_.handle; }
  const Config &config() const { return config_; }

 private:
  Config config_;
  MediaChannel attached_channel_;
  int attached_x_ = 0;
  int attached_y_ = 0;
  int attached_layer_ = 0;
  bool created_ = false;
  bool attached_ = false;
};

}  // namespace tdl_app
