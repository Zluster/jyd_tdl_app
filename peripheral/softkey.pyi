# ruff: noqa: E402
"""Long-press button access through configured GPIO or ADC inputs."""

from .pinmap import SoftKeyInfo
from collections.abc import Callable
from enum import Enum
from threading import Thread
from dara.core.common import DaraEvent
from .adc import ADC
from .gpio import GPIO

class SoftKeyError(OSError):
    """Raised when a soft-key operation cannot be completed."""
    ...


class SoftKeyEvent(DaraEvent):
    """A button release classified by its press duration."""
    class Type(str, Enum):
        """Button press classifications reported on release."""
        NORMAL_PRESS = ...
        LONG_PRESS = ...


    button: SoftKey
    """The button that produced the event."""
    type: Type
    """Whether the release completed a normal or long press."""
    timestamp: int
    """The release timestamp in nanoseconds."""
    def __init__(self, button: SoftKey, type: Type, timestamp: int) -> None:
        """Initialize a classified button release event."""
        ...



class SoftKey:
    """A button whose GPIO or ADC state is maintained by a background thread."""
    info: SoftKeyInfo | None
    _ADC_POLL_INTERVAL = ...
    _gpio: GPIO | None
    _adc: ADC | None
    _thread: Thread | None
    def __init__(self, id: int | str, long_press_ms: int | None = ...) -> None:
        """Create and open a button input with an optional long-press duration."""
        ...

    def open(self) -> None:
        """Open the input, reset button state, and start background monitoring."""
        ...

    def close(self) -> None:
        """Stop monitoring, close the input, and discard pending button state."""
        ...

    def __enter__(self) -> SoftKey:
        """Open the button if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the button when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the configured button input is open."""
        ...

    @property
    def long_press_ms(self) -> int | None:
        """The long-press duration in milliseconds, or ``None`` when disabled."""
        ...

    @long_press_ms.setter
    def long_press_ms(self, ms: int | None) -> None:
        """Set the long-press duration in milliseconds, or disable it with ``None``."""
        ...

    def is_down(self) -> bool:
        """Return the current down state maintained by the monitor thread."""
        ...

    def is_long_down(self) -> bool:
        """Return whether the monitored down state has reached the long threshold."""
        ...

    def is_pressed(self) -> bool:
        """Consume the normal-press state set by the monitor thread."""
        ...

    def is_long_pressed(self) -> bool:
        """Consume the long-press state set by the monitor thread."""
        ...

    def set_callback(self, cb: Callable[[SoftKeyEvent], None] | None) -> None:
        """Set or remove the callback without changing background monitoring."""
        ...
    def _start_watching(self) -> None: ...
    def _stop_watching(self) -> None: ...
    def _watch(self) -> None: ...
    def _watch_gpio(self) -> None: ...
    def _watch_adc(self) -> None: ...
    def _begin_press(self) -> None: ...
    def _finish_press(self, timestamp: int) -> None: ...
    def _reset_state(self) -> None: ...
