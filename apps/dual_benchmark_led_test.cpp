#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "tdl_app/rgb_led.hpp"

extern "C" {
int CVI_SYS_Init(void);
int CVI_SYS_Exit(void);
}

namespace {

void usage(const char *program) {
  std::cerr << "Usage:\n"
            << "  " << program << " status\n"
            << "  " << program << " off\n"
            << "  " << program << " color R G B [brightness]\n"
            << "  " << program << " pixel INDEX R G B [brightness]\n"
            << "  " << program << " count N\n"
            << "  " << program << " blink R G B PERIOD_MS COUNT [brightness]\n";
}

bool parseByte(const char *text, std::uint8_t *value) {
  char *end = nullptr;
  const long parsed = std::strtol(text, &end, 0);
  if (!text || *text == '\0' || !end || *end != '\0' || parsed < 0 || parsed > 255)
    return false;
  *value = static_cast<std::uint8_t>(parsed);
  return true;
}

bool parsePositive(const char *text, unsigned *value) {
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 0);
  if (!text || *text == '\0' || !end || *end != '\0' || parsed == 0 || parsed > 1000000U)
    return false;
  *value = static_cast<unsigned>(parsed);
  return true;
}

bool check(bool ok, const std::string &error) {
  if (!ok) std::cerr << error << "\n";
  return ok;
}

class CviSysSession {
 public:
  bool open(std::string *error) {
    const int ret = CVI_SYS_Init();
    if (ret == 0) {
      opened_ = true;
      return true;
    }
    if (error) *error = "CVI_SYS_Init failed, ret=" + std::to_string(ret);
    return false;
  }

  ~CviSysSession() {
    if (opened_) CVI_SYS_Exit();
  }

 private:
  bool opened_ = false;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  std::string error;
  CviSysSession sys;
  if (!sys.open(&error)) {
    std::cerr << "MMF init failed: " << error << "\n";
    return 1;
  }

  tdl_app::RgbLed led;
  if (!led.isEnabled()) {
    std::cerr << "RGB LED enable failed: " << led.lastError() << "\n";
    return 1;
  }

  const std::string command = argv[1];
  bool ok = false;
  if (command == "status" && argc == 2) {
    tdl_app::RgbLed::Status status;
    ok = led.getStatus(&status, &error);
    if (ok) {
      std::cout << "enabled=" << status.enabled
                << " pixels=" << static_cast<unsigned>(status.pixel_count)
                << " last_error=" << static_cast<unsigned>(status.last_error) << "\n";
    }
  } else if (command == "off" && argc == 2) {
    ok = led.clear(&error) && led.show(&error);
  } else if (command == "count" && argc == 3) {
    std::uint8_t count;
    ok = parseByte(argv[2], &count) && count != 0 && led.setPixelCount(count, &error);
  } else if (command == "color" && (argc == 5 || argc == 6)) {
    std::uint8_t r, g, b, brightness = 255;
    ok = parseByte(argv[2], &r) && parseByte(argv[3], &g) && parseByte(argv[4], &b) &&
         (argc == 5 || parseByte(argv[5], &brightness));
    if (ok) ok = led.setBrightness(brightness, &error) && led.setAll(r, g, b, &error) && led.show(&error);
  } else if (command == "pixel" && (argc == 6 || argc == 7)) {
    std::uint8_t index, r, g, b, brightness = 255;
    ok = parseByte(argv[2], &index) && parseByte(argv[3], &r) && parseByte(argv[4], &g) &&
         parseByte(argv[5], &b) && (argc == 6 || parseByte(argv[6], &brightness));
    if (ok) ok = led.setBrightness(brightness, &error) &&
                     led.setPixel(index, {r, g, b}, &error) && led.show(&error);
  } else if (command == "blink" && (argc == 7 || argc == 8)) {
    std::uint8_t r, g, b, brightness = 255;
    unsigned period_ms, count;
    ok = parseByte(argv[2], &r) && parseByte(argv[3], &g) && parseByte(argv[4], &b) &&
         parsePositive(argv[5], &period_ms) && parsePositive(argv[6], &count) &&
         (argc == 7 || parseByte(argv[7], &brightness));
    if (ok) ok = led.setBrightness(brightness, &error);
    for (unsigned i = 0; ok && i < count; ++i) {
      ok = led.setAll(r, g, b, &error) && led.show(&error);
      if (ok) usleep(period_ms * 1000U);
      if (ok) ok = led.clear(&error) && led.show(&error);
      if (ok) usleep(period_ms * 1000U);
    }
  } else {
    usage(argv[0]);
    return 2;
  }

  return check(ok, error) ? 0 : 1;
}
