"""Hobby servo control through a board PWM channel."""


from math import isfinite

from dara.core._error_helpers import wrap_error_as
from dara.peripheral.pwm import PWM


class ServoError(OSError):
    """Raised when a servo operation cannot be completed."""


class Servo:
    """A positional servo driven by 50 Hz PWM pulses."""

    # Standard hobby servos use 50 Hz: 1 / 50 s = 20,000 us.
    _PERIOD_US = 20_000

    def __init__(
        self,
        pwm_channel,
        min_us = 500,
        max_us = 2500,
        angle_range = 180,
        *,
        auto_open = True,
    ):
        """Create a servo on a PWM identifier and optionally activate it."""
        if any(
            isinstance(value, bool) or not isinstance(value, (int, float))
            for value in (min_us, max_us, angle_range)
        ):
            raise ValueError("min_us, max_us, and angle_range must be numbers")
        if not 0 < min_us < max_us <= self._PERIOD_US:
            raise ValueError("min_us and max_us must define a positive PWM pulse range")
        if not isfinite(angle_range) or angle_range <= 0:
            raise ValueError("angle_range must be positive")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self._min_us = min_us
        self._max_us = max_us
        self._angle_range = angle_range
        self._pwm = PWM(
            pwm_channel,
            freq=1_000_000 / self._PERIOD_US,
            duty=min_us / self._PERIOD_US,
            enable=True,
            auto_open=False,
        )
        if auto_open:
            self.open()

    @wrap_error_as(ServoError, "Servo open failed", catch=OSError)
    def open(self):
        """Open the PWM output and move the servo to zero degrees."""
        self._pwm.open()

    @wrap_error_as(ServoError, "Servo close failed", catch=OSError)
    def close(self):
        """Close the PWM output."""
        self._pwm.close()

    def __enter__(self):
        """Open the servo if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the servo when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the servo PWM output is open."""
        return self._pwm.is_opened

    @wrap_error_as(ServoError, "Servo angle update failed", catch=OSError)
    def set_angle(self, angle):
        """Set the servo angle from zero through the configured range."""
        if (
            isinstance(angle, bool)
            or not isinstance(angle, (int, float))
            or not 0 <= angle <= self._angle_range
        ):
            raise ValueError(
                f"angle must be a number from 0 through {self._angle_range}"
            )
        pulse_us = self._min_us + (
            (self._max_us - self._min_us) * angle / self._angle_range
        )
        self._pwm.set_duty(pulse_us / self._PERIOD_US)
