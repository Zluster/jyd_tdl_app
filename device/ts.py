"""Temperature sensor types and abstract contract."""


from abc import ABC, abstractmethod

from dara.core.registry import Registry


class TSError(OSError):
    """Raised when a temperature sensor operation cannot be completed."""


class TSData:
    """Temperature data from a temperature sensor capability read."""

    def __init__(self, temperature):
        """Initialize a temperature sample."""
        self.temperature = temperature


class TS(ABC):
    """Abstract contract for temperature sensors."""

    @abstractmethod
    def open(self):
        """Open the device and initialize its temperature sensor."""

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
    def read_temperature(self):
        """Return the temperature in degrees Celsius."""


ts_drivers = Registry[TS]("temperature sensor drivers")
"""Registry of available temperature sensor driver classes."""
