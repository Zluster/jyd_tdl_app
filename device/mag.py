"""Shared magnetometer types and abstract contract."""


from abc import ABC, abstractmethod

from dara.core.registry import Registry


class MagError(OSError):
    """Raised when a magnetometer operation cannot be completed."""


class MagData:
    """Magnetic-field data from a magnetometer capability read."""

    """Magnetic field along each axis in microteslas."""

    def __init__(self, mag):
        """Initialize a magnetic-field sample."""
        self.mag = mag


class Mag(ABC):
    """Abstract contract for magnetometers."""

    @abstractmethod
    def open(self):
        """Open the device and initialize its sensor."""

    @abstractmethod
    def close(self):
        """Deactivate the device without closing its peripheral bus."""

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
    def read_mag(self):
        """Return magnetic field axes in microteslas."""


mag_drivers = Registry[Mag]("magnetometer drivers")
"""Registry of available magnetometer driver classes."""
