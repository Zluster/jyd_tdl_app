#include <cstdio>
#include <cstdlib>
#include <string>

#include "tdl_app/touch_input.hpp"

namespace {
const char *phaseName(tdl_app::TouchPhase phase) {
  switch (phase) {
    case tdl_app::TouchPhase::Down: return "down";
    case tdl_app::TouchPhase::Move: return "move";
    case tdl_app::TouchPhase::Up: return "up";
    case tdl_app::TouchPhase::Cancel: return "cancel";
  }
  return "unknown";
}
}

int main(int argc, char **argv) {
  int count = 10;
  int timeout_ms = 10000;
  if (argc > 1) count = std::atoi(argv[1]);
  if (argc > 2) timeout_ms = std::atoi(argv[2]);
  if (count <= 0 || timeout_ms < 0) {
    std::fprintf(stderr, "usage: %s [count=10] [timeout_ms=10000]\\n", argv[0]);
    return 2;
  }
  tdl_app::TouchInput touch;
  std::string error;
  if (!touch.open(&error)) {
    std::fprintf(stderr, "touch open failed: %s\\n", error.c_str());
    return 1;
  }
  std::printf("touch: waiting for %d event(s) on /dev/input/event0\\n", count);
  for (int index = 0; index < count; ++index) {
    tdl_app::TouchEvent event;
    if (!touch.read(&event, timeout_ms, &error)) {
      if (error.empty()) {
        std::fprintf(stderr, "touch timeout after %d ms\\n", timeout_ms);
      } else {
        std::fprintf(stderr, "touch read failed: %s\\n", error.c_str());
      }
      return 3;
    }
    std::printf("%s x=%d y=%d pressure=%d id=%d time_us=%llu\\n",
                phaseName(event.phase), event.x, event.y, event.pressure,
                event.tracking_id,
                static_cast<unsigned long long>(event.timestamp_us));
  }
  return 0;
}
