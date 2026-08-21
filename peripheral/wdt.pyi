# ruff: noqa: E402
"""Linux watchdog timer access."""

from .pinmap import WDTInfo


_WDT_DEVICE = ...
_WDIOC_SETTIMEOUT = ...
class WDTError(OSError):
    """Raised when a watchdog timer operation cannot be completed."""
    ...


class WDT:
    """A Linux watchdog timer backed by ``/dev/watchdog``."""
    info: WDTInfo
    _fd: int | None
    def __init__(self, timeout_ms: int = ..., *, auto_open: bool = ...) -> None:
        """Create a watchdog with a timeout in milliseconds.

        Linux watchdog drivers accept whole-second timeouts, so values are
        rounded up to ensure the requested timeout is not shortened.
        """
        ...

    def open(self) -> None:
        """Open the watchdog and configure its timeout."""
        ...

    def close(self) -> None:
        """Stop the watchdog with Linux's magic close sequence."""
        ...

    def __enter__(self) -> WDT:
        """Open the watchdog if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the watchdog when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the watchdog device is open."""
        ...

    def feed(self) -> None:
        """Reset the watchdog countdown."""
        ...
