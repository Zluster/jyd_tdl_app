#pragma once

#include <cstdint>
#include <string>

namespace tdl_app {

enum class TouchPhase : std::uint8_t {
  Down,
  Move,
  Up,
  Cancel,
};

enum class TouchRotation : std::uint16_t {
  Deg0 = 0,
  Deg90 = 90,
  Deg180 = 180,
  Deg270 = 270,
};

struct TouchEvent {
  TouchPhase phase = TouchPhase::Move;
  int x = 0;
  int y = 0;
  int pressure = 0;
  int tracking_id = -1;
  std::uint64_t timestamp_us = 0;
};

class TouchInput {
 public:
  struct Config {
    std::string device = "/dev/input/event0";
    TouchRotation rotation = TouchRotation::Deg0;
  };

  TouchInput();
  explicit TouchInput(const Config &config);
  ~TouchInput();

  TouchInput(const TouchInput &) = delete;
  TouchInput &operator=(const TouchInput &) = delete;

  bool open(std::string *error = nullptr);
  // Returns false with an empty error when no event arrives before timeout_ms.
  bool read(TouchEvent *event, int timeout_ms = 0,
            std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  void mapCoordinates(int *x, int *y) const;

  Config config_;
  int fd_ = -1;
  int raw_x_ = 0;
  int raw_y_ = 0;
  int pressure_ = 0;
  int tracking_id_ = -1;
  int slot_ = 0;
  bool primary_slot_ = true;
  bool is_down_ = false;
  bool pending_down_ = false;
  bool pending_up_ = false;
  bool pending_cancel_ = false;
  bool position_dirty_ = false;
  std::uint64_t timestamp_us_ = 0;
};

}  // namespace tdl_app
