#include "tdl_app/rgb_led.hpp"

#include <algorithm>
#include <string>

extern "C" {
struct CVI_SYS_RGBLED_PIXEL_S {
  std::uint8_t index;
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

struct CVI_SYS_RGBLED_COLOR_S {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

struct CVI_SYS_RGBLED_STATUS_S {
  std::uint8_t enabled;
  std::uint8_t pixel_count;
  std::uint8_t last_error;
  std::uint8_t reserved;
};

int CVI_SYS_RGBLED_Enable(int enable);
int CVI_SYS_RGBLED_SetPixelCount(std::uint8_t pixel_count);
int CVI_SYS_RGBLED_SetPixel(const CVI_SYS_RGBLED_PIXEL_S *pixel);
int CVI_SYS_RGBLED_SetAll(const CVI_SYS_RGBLED_COLOR_S *color);
int CVI_SYS_RGBLED_Show(void);
int CVI_SYS_RGBLED_Clear(void);
int CVI_SYS_RGBLED_GetStatus(CVI_SYS_RGBLED_STATUS_S *status);
}

namespace tdl_app {
namespace {

constexpr int kSuccess = 0;

}  // namespace

RgbLed::RgbLed(std::uint8_t pixel_count) {
  const int ret = CVI_SYS_RGBLED_Enable(1);
  if (ret != kSuccess) {
    last_error_ = ret;
    return;
  }
  enabled_ = true;

  Status status;
  if (getStatus(&status)) {
    pixels_.assign(status.pixel_count, Color{});
  }
  if (pixel_count != 0) {
    setPixelCount(pixel_count);
  }
}

RgbLed::~RgbLed() {
  if (!enabled_) return;
  CVI_SYS_RGBLED_Clear();
  CVI_SYS_RGBLED_Show();
  CVI_SYS_RGBLED_Enable(0);
}

bool RgbLed::isEnabled() const { return enabled_; }

int RgbLed::lastError() const { return last_error_; }

std::uint8_t RgbLed::brightness() const { return brightness_; }

bool RgbLed::setFailure(int ret, const char *operation, std::string *error) const {
  if (error) {
    *error = std::string(operation) + " failed, ret=" + std::to_string(ret);
  }
  return false;
}

RgbLed::Color RgbLed::scaled(Color color) const {
  auto scale = [this](std::uint8_t value) {
    return static_cast<std::uint8_t>(
        (static_cast<unsigned>(value) * brightness_ + 127U) / 255U);
  };
  return Color{scale(color.r), scale(color.g), scale(color.b)};
}

bool RgbLed::getStatus(Status *status, std::string *error) const {
  if (!status) {
    return setFailure(-1, "RGBLED status pointer", error);
  }
  CVI_SYS_RGBLED_STATUS_S native{};
  const int ret = CVI_SYS_RGBLED_GetStatus(&native);
  if (ret != kSuccess) {
    return setFailure(ret, "CVI_SYS_RGBLED_GetStatus", error);
  }
  status->enabled = native.enabled != 0;
  status->pixel_count = native.pixel_count;
  status->last_error = native.last_error;
  return true;
}

bool RgbLed::setPixelCount(std::uint8_t pixel_count, std::string *error) {
  if (!enabled_) {
    return setFailure(last_error_, "RGBLED is not enabled", error);
  }
  if (pixel_count == 0) {
    return setFailure(-1, "RGBLED pixel count", error);
  }
  const int ret = CVI_SYS_RGBLED_SetPixelCount(pixel_count);
  if (ret != kSuccess) {
    last_error_ = ret;
    return setFailure(ret, "CVI_SYS_RGBLED_SetPixelCount", error);
  }
  pixels_.assign(pixel_count, Color{});
  return true;
}

bool RgbLed::applyPixel(std::uint8_t index, std::string *error) {
  if (index >= pixels_.size()) {
    return setFailure(-1, "RGBLED pixel index", error);
  }
  const Color color = scaled(pixels_[index]);
  const CVI_SYS_RGBLED_PIXEL_S pixel{index, color.r, color.g, color.b};
  const int ret = CVI_SYS_RGBLED_SetPixel(&pixel);
  if (ret != kSuccess) {
    last_error_ = ret;
    return setFailure(ret, "CVI_SYS_RGBLED_SetPixel", error);
  }
  return true;
}

bool RgbLed::applyAll(std::string *error) {
  if (pixels_.empty()) {
    return setFailure(-1, "RGBLED pixel count", error);
  }
  for (std::size_t i = 0; i < pixels_.size(); ++i) {
    if (!applyPixel(static_cast<std::uint8_t>(i), error)) {
      return false;
    }
  }
  return true;
}

bool RgbLed::setBrightness(std::uint8_t brightness, std::string *error) {
  brightness_ = brightness;
  return pixels_.empty() || applyAll(error);
}

bool RgbLed::setPixel(std::uint8_t index, Color color, std::string *error) {
  if (!enabled_) {
    return setFailure(last_error_, "RGBLED is not enabled", error);
  }
  if (index >= pixels_.size()) {
    return setFailure(-1, "RGBLED pixel index", error);
  }
  pixels_[index] = color;
  return applyPixel(index, error);
}

bool RgbLed::setPixel(std::uint8_t index, std::uint8_t r, std::uint8_t g,
                    std::uint8_t b, std::string *error) {
  return setPixel(index, Color{r, g, b}, error);
}

bool RgbLed::setAll(Color color, std::string *error) {
  if (!enabled_) {
    return setFailure(last_error_, "RGBLED is not enabled", error);
  }
  if (pixels_.empty()) {
    return setFailure(-1, "RGBLED pixel count", error);
  }
  std::fill(pixels_.begin(), pixels_.end(), color);
  const Color output = scaled(color);
  const CVI_SYS_RGBLED_COLOR_S native{output.r, output.g, output.b};
  const int ret = CVI_SYS_RGBLED_SetAll(&native);
  if (ret != kSuccess) {
    last_error_ = ret;
    return setFailure(ret, "CVI_SYS_RGBLED_SetAll", error);
  }
  return true;
}

bool RgbLed::setAll(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                    std::string *error) {
  return setAll(Color{r, g, b}, error);
}

bool RgbLed::clear(std::string *error) {
  if (!enabled_) {
    return setFailure(last_error_, "RGBLED is not enabled", error);
  }
  std::fill(pixels_.begin(), pixels_.end(), Color{});
  const int ret = CVI_SYS_RGBLED_Clear();
  if (ret != kSuccess) {
    last_error_ = ret;
    return setFailure(ret, "CVI_SYS_RGBLED_Clear", error);
  }
  return show(error);  // Apply the clear immediately.
}

bool RgbLed::show(std::string *error) {
  if (!enabled_) {
    return setFailure(last_error_, "RGBLED is not enabled", error);
  }
  const int ret = CVI_SYS_RGBLED_Show();
  if (ret != kSuccess) {
    last_error_ = ret;
    return setFailure(ret, "CVI_SYS_RGBLED_Show", error);
  }
  return true;
}

}  // namespace tdl_app
