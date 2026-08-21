"""Distance sensor types and abstract contract."""


from abc import ABC, abstractmethod

from dara.core.registry import Registry


class DSError(OSError):
    """Raised when a distance sensor operation cannot be completed."""


class DSData:
    """Distance data from a distance sensor capability read."""

    """Distance in centimeters."""

    def __init__(self, distance):
        """Initialize a distance sample."""
        self.distance = distance


class DS(ABC):
    """Abstract contract for distance sensors."""

    @abstractmethod
    def open(self):
        """Open the device and initialize its distance sensor."""

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
    def read_distance(self):
        """Return the distance in centimeters."""


ds_drivers = Registry[DS]("distance sensor drivers")
"""Registry of available distance sensor driver classes."""
