# ruff: noqa: E402
"""Temperature sensor types and abstract contract."""

from abc import ABC, abstractmethod

class TSError(OSError):
    """Raised when a temperature sensor operation cannot be completed."""
    ...


class TSData:
    """Temperature data from a temperature sensor capability read."""
    temperature: float
    """Temperature in degrees Celsius."""
    def __init__(self, temperature: float) -> None:
        """Initialize a temperature sample."""
        ...



class TS(ABC):
    """Abstract contract for temperature sensors."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and initialize its temperature sensor."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral."""
        ...

    def __enter__(self) -> TS:
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
    def read_temperature(self) -> float:
        """Return the temperature in degrees Celsius."""
        ...



ts_drivers = ...
