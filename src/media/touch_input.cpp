#include "tdl_app/touch_input.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

int clamp(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

std::uint64_t eventTimestamp(const input_event &event) {
  return static_cast<std::uint64_t>(event.input_event_sec) * 1000000ULL +
         static_cast<std::uint64_t>(event.input_event_usec);
}

}  // namespace

TouchInput::TouchInput() = default;
TouchInput::TouchInput(const Config &config) : config_(config) {}
TouchInput::~TouchInput() { close(); }

bool TouchInput::open(std::string *error) {
  close();
  switch (config_.rotation) {
    case TouchRotation::Deg0:
    case TouchRotation::Deg90:
    case TouchRotation::Deg180:
    case TouchRotation::Deg270:
      break;
    default:
      setError(error, "touch rotation must be 0, 90, 180, or 270");
      return false;
  }
  fd_ = ::open(config_.device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    setError(error, "open " + config_.device + " failed: " +
                        std::strerror(errno));
    return false;
  }
  raw_x_ = 0;
  raw_y_ = 0;
  pressure_ = 0;
  tracking_id_ = -1;
  slot_ = 0;
  primary_slot_ = true;
  is_down_ = false;
  pending_down_ = false;
  pending_up_ = false;
  pending_cancel_ = false;
  position_dirty_ = false;
  timestamp_us_ = 0;
  if (error) error->clear();
  return true;
}

bool TouchInput::read(TouchEvent *output, int timeout_ms,
                      std::string *error) {
  if (!output) {
    setError(error, "touch event output is null");
    return false;
  }
  if (fd_ < 0) {
    setError(error, "touch input is not open");
    return false;
  }
  if (timeout_ms < 0) timeout_ms = -1;
  pollfd poll_fd {fd_, POLLIN, 0};
  const int poll_result = ::poll(&poll_fd, 1, timeout_ms);
  if (poll_result == 0) {
    if (error) error->clear();
    return false;
  }
  if (poll_result < 0) {
    if (errno == EINTR) {
      if (error) error->clear();
    } else {
      setError(error, "poll touch input failed: " + std::string(std::strerror(errno)));
    }
    return false;
  }

  for (;;) {
    input_event event {};
    const ssize_t bytes = ::read(fd_, &event, sizeof(event));
    if (bytes == static_cast<ssize_t>(sizeof(event))) {
      timestamp_us_ = eventTimestamp(event);
      if (event.type == EV_ABS) {
        if (event.code == ABS_MT_SLOT) {
          slot_ = event.value;
          primary_slot_ = slot_ == 0;
        } else if (primary_slot_) {
          if (event.code == ABS_MT_TRACKING_ID) {
            tracking_id_ = event.value;
            if (event.value < 0) {
              pending_up_ = true;
            } else if (!is_down_) {
              pending_down_ = true;
            }
          } else if (event.code == ABS_MT_POSITION_X || event.code == ABS_X) {
            raw_x_ = event.value;
            position_dirty_ = true;
          } else if (event.code == ABS_MT_POSITION_Y || event.code == ABS_Y) {
            raw_y_ = event.value;
            position_dirty_ = true;
          } else if (event.code == ABS_MT_PRESSURE || event.code == ABS_PRESSURE) {
            pressure_ = event.value;
          }
        }
      } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
        if (event.value == 0) {
          pending_up_ = true;
        } else if (!is_down_) {
          pending_down_ = true;
        }
      } else if (event.type == EV_SYN && event.code == SYN_DROPPED) {
        if (is_down_) pending_cancel_ = true;
      } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
        TouchPhase phase;
        bool emit = false;
        if (pending_cancel_) {
          phase = TouchPhase::Cancel;
          is_down_ = false;
          emit = true;
        } else if (pending_up_) {
          phase = TouchPhase::Up;
          is_down_ = false;
          emit = true;
        } else if (pending_down_) {
          phase = TouchPhase::Down;
          is_down_ = true;
          emit = true;
        } else if (is_down_ && position_dirty_) {
          phase = TouchPhase::Move;
          emit = true;
        }
        pending_down_ = false;
        pending_up_ = false;
        pending_cancel_ = false;
        position_dirty_ = false;
        if (!emit) continue;
        output->phase = phase;
        mapCoordinates(&output->x, &output->y);
        output->pressure = pressure_;
        output->tracking_id = tracking_id_;
        output->timestamp_us = timestamp_us_;
        if (error) error->clear();
        return true;
      }
      continue;
    }
    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (error) error->clear();
      return false;
    }
    if (bytes < 0 && errno == EINTR) continue;
    setError(error, bytes == 0 ? "touch input closed" :
        "read touch input failed: " + std::string(std::strerror(errno)));
    return false;
  }
}

void TouchInput::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool TouchInput::isOpen() const { return fd_ >= 0; }

void TouchInput::mapCoordinates(int *x, int *y) const {
  constexpr int kScreenWidth = 720;
  constexpr int kScreenHeight = 480;
  const int raw_x = clamp(raw_x_, 0, kScreenWidth - 1);
  const int raw_y = clamp(raw_y_, 0, kScreenHeight - 1);

  switch (config_.rotation) {
    case TouchRotation::Deg0:
      *x = raw_x;
      *y = raw_y;
      return;
    case TouchRotation::Deg90:
      *x = kScreenHeight - 1 - raw_y;
      *y = raw_x;
      return;
    case TouchRotation::Deg180:
      *x = kScreenWidth - 1 - raw_x;
      *y = kScreenHeight - 1 - raw_y;
      return;
    case TouchRotation::Deg270:
      *x = raw_y;
      *y = kScreenWidth - 1 - raw_x;
      return;
  }
}

}  // namespace tdl_app
