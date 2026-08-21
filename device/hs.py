"""Humidity sensor types and abstract contract."""


from abc import ABC, abstractmethod

from dara.core.registry import Registry


class HSError(OSError):
    """Raised when a humidity sensor operation cannot be completed."""


class HSData:
    """Relative humidity data from a humidity sensor capability read."""

    """Relative humidity as a percentage."""

    def __init__(self, humidity):
        """Initialize a relative humidity sample."""
        self.humidity = humidity


class HS(ABC):
    """Abstract contract for humidity sensors."""

    @abstractmethod
    def open(self):
        """Open the device and initialize its humidity sensor."""

    @abstractmethod
    def close(self):
        """Deactivate the device without closing its peripheral."""

    def __enter__(self):
        """Open the device if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the device when leaving a ``with`` statement."""
        self.close()

    @property
    @abstractmethod
    def is_opened(self):
        """Return whether the device is open."""

    @abstractmethod
    def read_humidity(self):
        """Return the relative humidity as a percentage."""


hs_drivers = Registry[HS]("humidity sensor drivers")
"""Registry of available humidity sensor driver classes."""
