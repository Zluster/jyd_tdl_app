# ruff: noqa: E402
"""Hobby servo control through a board PWM channel."""

class ServoError(OSError):
    """Raised when a servo operation cannot be completed."""
    ...


class Servo:
    """A positional servo driven by 50 Hz PWM pulses."""
    _PERIOD_US = ...
    def __init__(self, pwm_channel: int | str, min_us: int | float = ..., max_us: int | float = ..., angle_range: int | float = ..., *, auto_open: bool = ...) -> None:
        """Create a servo on a PWM identifier and optionally activate it."""
        ...

    def open(self) -> None:
        """Open the PWM output and move the servo to zero degrees."""
        ...

    def close(self) -> None:
        """Close the PWM output."""
        ...

    def __enter__(self) -> Servo:
        """Open the servo if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the servo when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the servo PWM output is open."""
        ...

    def set_angle(self, angle: int | float) -> None:
        """Set the servo angle from zero through the configured range."""
        ...
