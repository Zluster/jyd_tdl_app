#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tdl_app {

class RgbLed {
 public:
  struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
  };

  struct Status {
    bool enabled = false;
    std::uint8_t pixel_count = 0;
    std::uint8_t last_error = 0;
  };

  // The caller must keep the MMF system initialized for the lifetime of this
  // object, for example with SysContext.
  explicit RgbLed(std::uint8_t pixel_count = 14);
  ~RgbLed();

  RgbLed(const RgbLed &) = delete;
  RgbLed &operator=(const RgbLed &) = delete;

  bool isEnabled() const;
  int lastError() const;
  std::uint8_t brightness() const;

  bool getStatus(Status *status, std::string *error = nullptr) const;
  bool setPixelCount(std::uint8_t pixel_count, std::string *error = nullptr);
  bool setBrightness(std::uint8_t brightness, std::string *error = nullptr);
  bool setPixel(std::uint8_t index, Color color, std::string *error = nullptr);
  bool setAll(Color color, std::string *error = nullptr);
  bool setAll(std::uint8_t r, std::uint8_t g, std::uint8_t b,
              std::string *error = nullptr);
  bool clear(std::string *error = nullptr);
  bool show(std::string *error = nullptr);

 private:
  bool applyPixel(std::uint8_t index, std::string *error);
  bool applyAll(std::string *error);
  bool setFailure(int ret, const char *operation, std::string *error) const;
  Color scaled(Color color) const;

  bool enabled_ = false;
  int last_error_ = 0;
  std::uint8_t brightness_ = 255;
  std::vector<Color> pixels_;
};

}  // namespace tdl_app
