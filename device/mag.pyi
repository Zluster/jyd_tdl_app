# ruff: noqa: E402


"""Shared magnetometer types and abstract contract."""

from abc import ABC, abstractmethod

class MagError(OSError):
    """Raised when a magnetometer operation cannot be completed."""
    ...


class MagData:
    """Magnetic-field data from a magnetometer capability read."""
    mag: tuple[float, float, float]
    """Magnetic field along each axis in microteslas."""
    def __init__(self, mag: tuple[float, float, float]) -> None:
        """Initialize a magnetic-field sample."""
        ...



class Mag(ABC):
    """Abstract contract for magnetometers."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and initialize its sensor."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral bus."""
        ...

    def __enter__(self) -> Mag:
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
    def read_mag(self) -> tuple[float, float, float]:
        """Return magnetic field axes in microteslas."""
        ...



mag_drivers = ...
