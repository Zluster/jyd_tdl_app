# ruff: noqa: E402
"""Humidity sensor types and abstract contract."""

from abc import ABC, abstractmethod

class HSError(OSError):
    """Raised when a humidity sensor operation cannot be completed."""
    ...


class HSData:
    """Relative humidity data from a humidity sensor capability read."""
    humidity: float
    """Relative humidity as a percentage."""
    def __init__(self, humidity: float) -> None:
        """Initialize a relative humidity sample."""
        ...



class HS(ABC):
    """Abstract contract for humidity sensors."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and initialize its humidity sensor."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral."""
        ...

    def __enter__(self) -> HS:
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
    def read_humidity(self) -> float:
        """Return the relative humidity as a percentage."""
        ...



hs_drivers = ...
