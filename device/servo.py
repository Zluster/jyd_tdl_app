"""Hobby servo control through a board PWM channel."""


from math import isfinite

from dara.core._error_helpers import wrap_error_as
from dara.peripheral.pwm import PWM


class ServoError(OSError):
    """Raised when a servo operation cannot be completed."""


class Servo:
    """A 270-degree positional servo driven by 50 Hz PWM pulses.

    The default calibration is ``-45° = 600 us``, ``90° = 1500 us`` and
    ``225° = 2400 us``.  The three points are collinear, so pulse width is
    interpolated linearly over the supported ``-45° .. 225°`` range.
    """

    # Standard hobby servos use 50 Hz: 1 / 50 s = 20,000 us.
    _PERIOD_US = 20_000

    def __init__(
        self,
        pwm_channel,
        min_us = 600,
        max_us = 2400,
        min_angle = -45,
        max_angle = 225,
        *,
        auto_open = True,
    ):
        """Create a servo on a PWM identifier and optionally activate it."""
        if any(
            isinstance(value, bool) or not isinstance(value, (int, float))
            for value in (min_us, max_us, min_angle, max_angle)
        ):
            raise ValueError("min_us, max_us, min_angle, and max_angle must be numbers")
        if not 0 < min_us < max_us <= self._PERIOD_US:
            raise ValueError("min_us and max_us must define a positive PWM pulse range")
        if (not isfinite(min_angle) or not isfinite(max_angle)
                or min_angle >= max_angle):
            raise ValueError("min_angle must be less than max_angle")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self._min_us = min_us
        self._max_us = max_us
        self._min_angle = min_angle
        self._max_angle = max_angle
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
        """Open the PWM output at the calibrated -45 degree endpoint."""
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
        """Set the servo angle from -45 through 225 degrees by default."""
        if (
            isinstance(angle, bool)
            or not isinstance(angle, (int, float))
            or not self._min_angle <= angle <= self._max_angle
        ):
            raise ValueError(
                "angle must be a number from %s through %s"
                % (self._min_angle, self._max_angle)
            )
        pulse_us = self._min_us + (
            (self._max_us - self._min_us)
            * (angle - self._min_angle)
            / (self._max_angle - self._min_angle)
        )
        self._pwm.set_duty(pulse_us / self._PERIOD_US)
