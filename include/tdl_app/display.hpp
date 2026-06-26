#pragma once

#include <memory>
#include <string>

#include "tdl_app/camera.hpp"
#include "tdl_app/layout.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_app {

class Image;
class MediaLink;
class VoOutput;

class Display {
 public:
  enum class Input {
    None,
    Live,
    Ai,
    Main,
    SubRgb,
  };

  struct Config {
    int device = DualOsLayout::kVoDevice;
    int layer = 0;
    int channel = DualOsLayout::kVoChannel;
    int width = DualOsLayout::kScreenWidth;
    int height = DualOsLayout::kScreenHeight;
    int pixel_format = PixelFormat::NV12;
    int interface_type = VoInterfaceType::Mipi;
    int interface_sync = VoInterfaceSync::P720_1280_60;
    std::string framebuffer = "/dev/fb0";
    int image_layer = 0;
    bool image_double_buffer = true;
  };

  Display();
  explicit Display(const Config &config);
  ~Display();

  Display(const Display &) = delete;
  Display &operator=(const Display &) = delete;
  Display(Display &&other) noexcept;
  Display &operator=(Display &&other) noexcept;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool showLive(std::string *error = nullptr);
  bool hideLive(std::string *error = nullptr);
  bool show(Input input, std::string *error = nullptr);

  bool showImage(const std::string &path, std::string *error = nullptr);
  bool showImage(const std::string &path, int x, int y, int width, int height,
                 std::string *error = nullptr);
  bool clearImage(std::string *error = nullptr);

  bool snapshot(const std::string &path, int timeout_ms = 1000,
                std::string *error = nullptr);

  bool isLiveVisible() const;
  Input input() const { return input_; }

  static CameraSourceId toCameraSource(Input input);
  static const char *inputName(Input input);

 private:
  Config config_;
  std::unique_ptr<VoOutput> vo_;
  std::unique_ptr<Image> image_;
  std::unique_ptr<MediaLink> preview_link_;
  Input input_ = Input::None;
};

}  // namespace tdl_app
