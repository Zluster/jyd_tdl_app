# ruff: noqa: E402
"""PWM access to outputs declared in the board pin map."""

from ._periphery.pwm import PWM as _PeripheryPWM
from .pinmap import PWMInfo

class PWMError(OSError):
    """Raised when a PWM operation cannot be completed."""
    ...


class PWM:
    """A PWM output selected by its board configuration identifier."""
    info: PWMInfo
    _periphery_instance: _PeripheryPWM | None
    def __init__(self, id: int | str, freq: int | float | None = ..., duty: int | float | None = ..., enable: bool | None = ..., *, auto_open: bool = ...) -> None:
        """Create a PWM output and optionally open its mapped channel."""
        ...

    def open(self) -> None:
        """Open or re-open the mapped PWM channel."""
        ...

    def close(self) -> None:
        """Close the mapped PWM channel."""
        ...

    def __enter__(self) -> PWM:
        """Open the PWM channel if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the PWM channel when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the mapped PWM channel is open."""
        ...

    @property
    def is_enabled(self) -> bool:
        """Return whether PWM output is enabled."""
        ...

    def enable(self) -> None:
        """Enable PWM output."""
        ...

    def disable(self) -> None:
        """Disable PWM output."""
        ...

    def get_freq(self) -> float:
        """Read the output frequency in Hertz."""
        ...

    def set_freq(self, value: int | float) -> None:
        """Set the output frequency in Hertz."""
        ...

    def get_duty(self) -> float:
        """Read the output duty cycle as a ratio from 0.0 to 1.0."""
        ...

    def set_duty(self, value: int | float) -> None:
        """Set the output duty cycle as a ratio from 0.0 to 1.0."""
        ...
    @property
    def _backend(self) -> _PeripheryPWM: ...
