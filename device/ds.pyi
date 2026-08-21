# ruff: noqa: E402
"""Distance sensor types and abstract contract."""

from abc import ABC, abstractmethod

class DSError(OSError):
    """Raised when a distance sensor operation cannot be completed."""
    ...


class DSData:
    """Distance data from a distance sensor capability read."""
    distance: float
    """Distance in centimeters."""
    def __init__(self, distance: float) -> None:
        """Initialize a distance sample."""
        ...



class DS(ABC):
    """Abstract contract for distance sensors."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and initialize its distance sensor."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral."""
        ...

    def __enter__(self) -> DS:
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
    def read_distance(self) -> float:
        """Return the distance in centimeters."""
        ...



ds_drivers = ...
