# ruff: noqa: E402
"""GPIO access to pins declared in the board pin map."""

from ._periphery.gpio import GPIO as _PeripheryGPIO
from .pinmap import GPIOInfo
from enum import Enum
from dara.core.common import DaraEvent

class GPIOError(OSError):
    """Raised when a GPIO operation cannot be completed."""
    ...


class GPIODirection(str, Enum):
    """Directions supported by a GPIO pin."""
    IN = ...
    OUT = ...
    HIGH = ...
    LOW = ...


class GPIOEdge(str, Enum):
    """Input edge events supported by a GPIO pin."""
    NONE = ...
    RISING = ...
    FALLING = ...
    BOTH = ...


class GPIOPull(str, Enum):
    """Input bias settings supported by a GPIO pin."""
    DEFAULT = ...
    UP = ...
    DOWN = ...
    DISABLE = ...


class GPIODrive(str, Enum):
    """Output drive modes supported by a GPIO pin."""
    DEFAULT = ...
    OPEN_DRAIN = ...
    OPEN_SOURCE = ...


class GPIO:
    """A GPIO pin selected by its board configuration identifier."""
    info: GPIOInfo
    _periphery_instance: _PeripheryGPIO | None
    def __init__(self, id: int | str, direction: GPIODirection = ..., pull: GPIOPull = ..., edge: GPIOEdge = ..., drive: GPIODrive = ..., inverted: bool = ..., *, auto_open: bool = ...) -> None:
        """Create a GPIO pin and optionally open its mapped line."""
        ...

    def reset(self, direction: GPIODirection = ..., pull: GPIOPull = ..., edge: GPIOEdge = ..., drive: GPIODrive = ..., inverted: bool = ...) -> None:
        """Set the GPIO direction, pull, edge, drive, and active-low configuration."""
        ...

    @property
    def direction(self) -> GPIODirection:
        """The configured GPIO direction or initial output level."""
        ...

    @property
    def pull(self) -> GPIOPull:
        """The configured GPIO input bias."""
        ...

    @property
    def edge(self) -> GPIOEdge:
        """The configured GPIO input edge detection mode."""
        ...

    @property
    def drive(self) -> GPIODrive:
        """The configured GPIO output drive mode."""
        ...

    @property
    def inverted(self) -> bool:
        """Whether GPIO values use active-low logic."""
        ...

    def open(self) -> None:
        """Open or re-open the mapped GPIO line."""
        ...

    def close(self) -> None:
        """Close the mapped GPIO line."""
        ...

    def __enter__(self) -> GPIO:
        """Open the GPIO if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the GPIO when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the mapped GPIO line is open."""
        ...

    def read(self) -> bool:
        """Return the current GPIO state."""
        ...

    def write(self, value: bool) -> None:
        """Set the GPIO state to ``value``."""
        ...

    def poll(self, timeout: float | None = ...) -> bool:
        """Wait for a configured edge and return whether one occurred.

        ``None`` and negative values wait indefinitely, ``0`` does not wait,
        and a positive value waits that many seconds.
        """
        ...

    def read_event(self) -> PollEvent:
        """Return the edge event found by :meth:`poll`.

        This is supported for GPIO character devices, including Dara's mapped
        ``/dev/gpiochip*`` lines.
        """
        ...

    def high(self) -> None:
        """Set the GPIO output high."""
        ...

    def low(self) -> None:
        """Set the GPIO output low."""
        ...

    def toggle(self) -> None:
        """Invert the current GPIO output state."""
        ...
    @property
    def _backend(self) -> _PeripheryGPIO: ...



class PollEvent(DaraEvent):
    """An input edge event reported by :meth:`GPIO.poll`."""
    edge: GPIOEdge
    """The edge that occurred."""
    timestamp: int
    """The Linux event timestamp in nanoseconds."""
    def __init__(self, edge: GPIOEdge, timestamp: int) -> None:
        """Initialize an input edge event."""
        ...
