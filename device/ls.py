"""Light sensor types and abstract contract."""


from abc import ABC, abstractmethod

from dara.core.registry import Registry


class LSError(OSError):
    """Raised when a light sensor operation cannot be completed."""


class LSData:
    """Brightness data from a light sensor capability read."""

    def __init__(self, brightness):
        """Initialize a brightness sample."""
        self.brightness = brightness


class LS(ABC):
    """Abstract contract for light sensors."""

    @abstractmethod
    def open(self):
        """Open the device and initialize its light sensor."""

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
    def read_brightness(self):
        """Return brightness in lux, or UV Index in ultraviolet mode."""


ls_drivers = Registry[LS]("light sensor drivers")
"""Registry of available light sensor driver classes."""
