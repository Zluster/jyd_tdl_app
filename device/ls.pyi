# ruff: noqa: E402
"""Light sensor types and abstract contract."""

from abc import ABC, abstractmethod

class LSError(OSError):
    """Raised when a light sensor operation cannot be completed."""
    ...


class LSData:
    """Brightness data from a light sensor capability read."""
    brightness: float
    """Brightness in lux, or UV Index for ultraviolet-capable sensors."""
    def __init__(self, brightness: float) -> None:
        """Initialize a brightness sample."""
        ...



class LS(ABC):
    """Abstract contract for light sensors."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and initialize its light sensor."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral."""
        ...

    def __enter__(self) -> LS:
        """Open the device if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the device when leaving a ``with`` statement."""
        ...

    @property
    @abstractmethod
    def is_opened(self) -> bool:
        """Return whether the device is open."""
        ...

    @abstractmethod
    def read_brightness(self) -> float:
        """Return brightness in lux, or UV Index in ultraviolet mode."""
        ...



ls_drivers = ...
