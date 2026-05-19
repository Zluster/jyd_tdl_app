#include "tdl_app/graphic_vo_layer.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cvi_comm_gfbg.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

int bytesPerPixelForFormat(int format) {
  switch (format) {
    case GraphicBufferFormat::Argb8888:
      return 4;
    case GraphicBufferFormat::Argb4444:
    case GraphicBufferFormat::Argb1555:
      return 2;
    default:
      return 0;
  }
}

void applyColorFormat(int format, fb_var_screeninfo *var) {
  if (format == GraphicBufferFormat::Argb8888) {
    var->transp.offset = 24;
    var->transp.length = 8;
    var->red.offset = 16;
    var->red.length = 8;
    var->green.offset = 8;
    var->green.length = 8;
    var->blue.offset = 0;
    var->blue.length = 8;
    var->bits_per_pixel = 32;
    return;
  }

  if (format == GraphicBufferFormat::Argb4444) {
    var->transp.offset = 12;
    var->transp.length = 4;
    var->red.offset = 8;
    var->red.length = 4;
    var->green.offset = 4;
    var->green.length = 4;
    var->blue.offset = 0;
    var->blue.length = 4;
    var->bits_per_pixel = 16;
    return;
  }

  var->transp.offset = 15;
  var->transp.length = 1;
  var->red.offset = 10;
  var->red.length = 5;
  var->green.offset = 5;
  var->green.length = 5;
  var->blue.offset = 0;
  var->blue.length = 5;
  var->bits_per_pixel = 16;
}

}  // namespace

GraphicVoLayer::GraphicVoLayer() = default;

GraphicVoLayer::GraphicVoLayer(const Config &config) : config_(config) {}

GraphicVoLayer::~GraphicVoLayer() { close(); }

bool GraphicVoLayer::open(std::string *error) {
  if (isOpen()) {
    return true;
  }

  bytes_per_pixel_ = bytesPerPixelForFormat(config_.format);
  if (bytes_per_pixel_ <= 0) {
    setError(error, "unsupported graphic vo format: " +
                        std::to_string(config_.format));
    return false;
  }

  fd_ = ::open(config_.device.c_str(), O_RDWR, 0);
  if (fd_ < 0) {
    setError(error, "open framebuffer failed: " + config_.device +
                        ", errno=" + std::to_string(errno));
    return false;
  }

  cvi_fb_layer_info layer_info;
  std::memset(&layer_info, 0, sizeof(layer_info));
  layer_info.buf_mode = config_.double_buffer ? CVI_FB_LAYER_BUF_DOUBLE
                                              : CVI_FB_LAYER_BUF_ONE;
  layer_info.x_pos = config_.x;
  layer_info.y_pos = config_.y;
  layer_info.canvas_width = static_cast<CVI_U32>(config_.width);
  layer_info.canvas_height = static_cast<CVI_U32>(config_.height);
  layer_info.display_width = static_cast<CVI_U32>(
      config_.display_width > 0 ? config_.display_width : config_.width);
  layer_info.display_height = static_cast<CVI_U32>(
      config_.display_height > 0 ? config_.display_height : config_.height);
  layer_info.screen_width = static_cast<CVI_U32>(
      config_.screen_width > 0 ? config_.screen_width : config_.width);
  layer_info.screen_height = static_cast<CVI_U32>(
      config_.screen_height > 0 ? config_.screen_height : config_.height);
  layer_info.mask = CVI_FB_LAYER_MASK_BUF_MODE |
                    CVI_FB_LAYER_MASK_POS |
                    CVI_FB_LAYER_MASK_CANVAS_SIZE |
                    CVI_FB_LAYER_MASK_DISPLAY_SIZE |
                    CVI_FB_LAYER_MASK_SCREEN_SIZE;
  if (ioctl(fd_, FBIOPUT_LAYER_INFO, &layer_info) < 0) {
    setError(error, "FBIOPUT_LAYER_INFO failed, errno=" +
                        std::to_string(errno));
    close();
    return false;
  }

  fb_var_screeninfo var;
  std::memset(&var, 0, sizeof(var));
  if (ioctl(fd_, FBIOGET_VSCREENINFO, &var) < 0) {
    setError(error, "FBIOGET_VSCREENINFO failed, errno=" +
                        std::to_string(errno));
    close();
    return false;
  }

  applyColorFormat(config_.format, &var);
  var.xres = static_cast<CVI_U32>(config_.width);
  var.yres = static_cast<CVI_U32>(config_.height);
  var.xres_virtual = static_cast<CVI_U32>(config_.width);
  var.yres_virtual = static_cast<CVI_U32>(
      config_.double_buffer ? config_.height * 2 : config_.height);
  var.activate = FB_ACTIVATE_NOW;
  if (ioctl(fd_, FBIOPUT_VSCREENINFO, &var) < 0) {
    setError(error, "FBIOPUT_VSCREENINFO failed, errno=" +
                        std::to_string(errno));
    close();
    return false;
  }

  fb_fix_screeninfo fix;
  std::memset(&fix, 0, sizeof(fix));
  if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix) < 0) {
    setError(error, "FBIOGET_FSCREENINFO failed, errno=" +
                        std::to_string(errno));
    close();
    return false;
  }

  stride_ = static_cast<int>(fix.line_length);
  mapped_bytes_ =
      static_cast<std::size_t>(fix.line_length) * var.yres_virtual;
  mapped_ = mmap(nullptr, mapped_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd_, 0);
  if (mapped_ == MAP_FAILED) {
    mapped_ = nullptr;
    mapped_bytes_ = 0;
    setError(error, "mmap framebuffer failed, errno=" +
                        std::to_string(errno));
    close();
    return false;
  }

  if (!setVisible(config_.show, error)) {
    close();
    return false;
  }

  return true;
}

void GraphicVoLayer::close() {
  if (mapped_) {
    munmap(mapped_, mapped_bytes_);
    mapped_ = nullptr;
    mapped_bytes_ = 0;
  }
  if (fd_ >= 0) {
    if (visible_) {
      CVI_BOOL show = CVI_FALSE;
      ioctl(fd_, FBIOPUT_SHOW_GFBG, &show);
      visible_ = false;
    }
    ::close(fd_);
    fd_ = -1;
  }
  stride_ = 0;
  bytes_per_pixel_ = 0;
}

bool GraphicVoLayer::isOpen() const { return fd_ >= 0; }

bool GraphicVoLayer::setVisible(bool visible, std::string *error) {
  if (fd_ < 0) {
    setError(error, "graphic vo layer is not open");
    return false;
  }
  CVI_BOOL show = visible ? CVI_TRUE : CVI_FALSE;
  if (ioctl(fd_, FBIOPUT_SHOW_GFBG, &show) < 0) {
    setError(error, "FBIOPUT_SHOW_GFBG failed, errno=" +
                        std::to_string(errno));
    return false;
  }
  visible_ = visible;
  return true;
}

bool GraphicVoLayer::clear(std::uint32_t argb, std::string *error) {
  if (!mapped_) {
    setError(error, "graphic vo buffer is not mapped");
    return false;
  }

  if (bytes_per_pixel_ == 4) {
    std::uint32_t *pixels = static_cast<std::uint32_t *>(mapped_);
    const std::size_t count = mapped_bytes_ / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
      pixels[i] = argb;
    }
    return true;
  }

  const std::uint16_t value = static_cast<std::uint16_t>(argb & 0xFFFFU);
  std::uint16_t *pixels = static_cast<std::uint16_t *>(mapped_);
  const std::size_t count = mapped_bytes_ / sizeof(std::uint16_t);
  for (std::size_t i = 0; i < count; ++i) {
    pixels[i] = value;
  }
  return true;
}

bool GraphicVoLayer::present(std::string *error) {
  if (fd_ < 0) {
    setError(error, "graphic vo layer is not open");
    return false;
  }
  if (ioctl(fd_, FBIOGET_VER_BLANK_GFBG, nullptr) < 0) {
    setError(error, "FBIOGET_VER_BLANK_GFBG failed, errno=" +
                        std::to_string(errno));
    return false;
  }
  return true;
}

GraphicVoLayer::BufferView GraphicVoLayer::buffer() const {
  BufferView out;
  out.data = mapped_;
  out.bytes = mapped_bytes_;
  out.stride = stride_;
  out.width = config_.width;
  out.height = config_.height;
  out.bytes_per_pixel = bytes_per_pixel_;
  return out;
}

void *GraphicVoLayer::data() const { return mapped_; }

std::size_t GraphicVoLayer::bytes() const { return mapped_bytes_; }

int GraphicVoLayer::stride() const { return stride_; }

int GraphicVoLayer::width() const { return config_.width; }

int GraphicVoLayer::height() const { return config_.height; }

int GraphicVoLayer::bytesPerPixel() const { return bytes_per_pixel_; }

}  // namespace tdl_app
