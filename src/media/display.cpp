#include "tdl_app/display.hpp"

#include <memory>
#include <utility>

#include "tdl_app/image.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/vo_output.hpp"

namespace tdl_app {
namespace {

VoOutput::Config toVoConfig(const Display::Config &config) {
  VoOutput::Config out;
  out.device = config.device;
  out.layer = config.layer;
  out.channel = config.channel;
  out.width = config.width;
  out.height = config.height;
  out.pixel_format = config.pixel_format;
  out.interface_type = config.interface_type;
  out.interface_sync = config.interface_sync;
  return out;
}

Image::Config toImageConfig(const Display::Config &config) {
  Image::Config out;
  out.layer = config.image_layer;
  out.device = config.framebuffer;
  out.screen_width = config.width;
  out.screen_height = config.height;
  out.double_buffer = config.image_double_buffer;
  return out;
}

MediaChannel sourceChannelForInput(Display::Input input) {
  switch (input) {
    case Display::Input::Live:
      return MediaChannel::vpss(DualOsLayout::kCaptureVpssGroup,
                                DualOsLayout::kLiveChannel);
    case Display::Input::Ai:
      return MediaChannel::vpss(DualOsLayout::kCaptureVpssGroup,
                                DualOsLayout::kAiChannel);
    case Display::Input::Main:
      return MediaChannel::vpss(DualOsLayout::kCaptureVpssGroup,
                                DualOsLayout::kMainChannel);
    case Display::Input::SubRgb:
      return MediaChannel::vpss(DualOsLayout::kCaptureVpssGroup,
                                DualOsLayout::kSubRgbChannel);
    case Display::Input::None:
    default:
      return MediaChannel{};
  }
}

}  // namespace

Display::Display() = default;

Display::Display(const Config &config) : config_(config) {}

Display::~Display() { close(); }

Display::Display(Display &&other) noexcept = default;

Display &Display::operator=(Display &&other) noexcept = default;

bool Display::open(std::string *error) {
  if (vo_ && vo_->isOpen()) {
    return true;
  }
  vo_.reset(new VoOutput(toVoConfig(config_)));
  if (!vo_->open(error)) {
    vo_.reset();
    return false;
  }
  image_.reset(new Image(toImageConfig(config_)));
  return true;
}

void Display::close() {
  if (preview_link_) {
    preview_link_->unbind();
    preview_link_.reset();
  }
  input_ = Input::None;
  if (image_) {
    image_->close();
    image_.reset();
  }
  if (vo_) {
    vo_->close();
    vo_.reset();
  }
}

bool Display::isOpen() const { return vo_ && vo_->isOpen(); }

bool Display::showLive(std::string *error) {
  return show(Input::Live, error);
}

bool Display::hideLive(std::string *error) {
  (void)error;
  if (preview_link_) {
    preview_link_->unbind();
    preview_link_.reset();
  }
  input_ = Input::None;
  return true;
}

bool Display::show(Input input, std::string *error) {
  if (input == Input::None) {
    return hideLive(error);
  }
  if (!open(error)) {
    return false;
  }

  if (preview_link_) {
    preview_link_->unbind();
    preview_link_.reset();
  }

  MediaLink::Config link_config;
  link_config.source = sourceChannelForInput(input);
  link_config.destination = MediaChannel::vo(config_.layer, config_.channel);
  preview_link_.reset(new MediaLink(link_config));
  if (!preview_link_->bind(error)) {
    preview_link_.reset();
    return false;
  }

  input_ = input;
  if (image_) {
    image_->clear(nullptr);
  }
  return true;
}

bool Display::showImage(const std::string &path, std::string *error) {
  if (!hideLive(error)) {
    return false;
  }
  if (!open(error)) {
    return false;
  }
  return image_ && image_->show(path, error);
}

bool Display::showImage(const std::string &path, int x, int y, int width,
                        int height, std::string *error) {
  if (!hideLive(error)) {
    return false;
  }
  if (!open(error)) {
    return false;
  }
  return image_ && image_->show(path, x, y, width, height, error);
}

bool Display::clearImage(std::string *error) {
  if (!open(error)) {
    return false;
  }
  return image_ && image_->clear(error);
}

bool Display::snapshot(const std::string &path, int timeout_ms,
                       std::string *error) {
  Camera camera(Camera::forSource(toCameraSource(
      input_ == Input::None ? Input::Live : input_), timeout_ms));
  return camera.snapshot(path, error);
}

bool Display::isLiveVisible() const {
  return preview_link_ && preview_link_->isBound();
}

CameraSourceId Display::toCameraSource(Input input) {
  switch (input) {
    case Input::Ai:
      return CameraSourceId::Ai;
    case Input::Main:
      return CameraSourceId::Main;
    case Input::SubRgb:
      return CameraSourceId::SubRgb;
    case Input::Live:
    case Input::None:
      return CameraSourceId::Live;
  }
  return CameraSourceId::Live;
}

const char *Display::inputName(Input input) {
  switch (input) {
    case Input::None:
      return "none";
    case Input::Live:
      return "live";
    case Input::Ai:
      return "ai";
    case Input::Main:
      return "main";
    case Input::SubRgb:
      return "subrgb";
  }
  return "unknown";
}

}  // namespace tdl_app
