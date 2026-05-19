#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class GraphicVoLayer {
 public:
  struct Config {
    int layer = 0;
    std::string device = "/dev/fb0";
    int width = 720;
    int height = 1280;
    int format = GraphicBufferFormat::Argb8888;
    int x = 0;
    int y = 0;
    int display_width = 0;
    int display_height = 0;
    int screen_width = 0;
    int screen_height = 0;
    bool show = true;
    bool double_buffer = false;
  };

  struct BufferView {
    void *data = nullptr;
    std::size_t bytes = 0;
    int stride = 0;
    int width = 0;
    int height = 0;
    int bytes_per_pixel = 0;
  };

  static Config argb8888(int layer = 0, int width = 720, int height = 1280,
                         const std::string &device = "/dev/fb0") {
    Config config;
    config.layer = layer;
    config.device = device;
    config.width = width;
    config.height = height;
    config.format = GraphicBufferFormat::Argb8888;
    return config;
  }

  GraphicVoLayer();
  explicit GraphicVoLayer(const Config &config);
  ~GraphicVoLayer();

  GraphicVoLayer(const GraphicVoLayer &) = delete;
  GraphicVoLayer &operator=(const GraphicVoLayer &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool setVisible(bool visible, std::string *error = nullptr);
  bool clear(std::uint32_t argb, std::string *error = nullptr);
  bool present(std::string *error = nullptr);

  BufferView buffer() const;
  void *data() const;
  std::size_t bytes() const;
  int stride() const;
  int width() const;
  int height() const;
  int bytesPerPixel() const;

 private:
  Config config_;
  int fd_ = -1;
  void *mapped_ = nullptr;
  std::size_t mapped_bytes_ = 0;
  int stride_ = 0;
  int bytes_per_pixel_ = 0;
  bool visible_ = false;
};

}  // namespace tdl_app
