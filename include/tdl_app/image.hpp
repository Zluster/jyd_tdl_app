#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "tdl_app/frame_source.hpp"
#include "tdl_app/layout.hpp"

namespace tdl_app {

class GraphicVoLayer;

class Image {
 public:
  struct Config {
    int layer = 0;
    std::string device = "/dev/fb0";
    int screen_width = DualOsLayout::kScreenWidth;
    int screen_height = DualOsLayout::kScreenHeight;
    bool double_buffer = true;
    std::uint32_t clear_color = 0xFF000000u;
  };

  Image();
  explicit Image(const Config &config);
  ~Image();

  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&other) noexcept;
  Image &operator=(Image &&other) noexcept;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool show(const std::string &path, std::string *error = nullptr);
  bool show(const std::string &path, int x, int y, int width, int height,
            std::string *error = nullptr);
  bool clear(std::string *error = nullptr);
  bool setVisible(bool visible, std::string *error = nullptr);

  static bool save(const Frame &frame, const std::string &path,
                   std::string *error = nullptr);

 private:
  Config config_;
  std::unique_ptr<GraphicVoLayer> layer_;
};

}  // namespace tdl_app
