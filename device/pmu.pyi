# ruff: noqa: E402


"""Power management unit types and abstract contract."""

from abc import ABC, abstractmethod
from enum import Enum

class PMUPowerChannel(str, Enum):
    """Power rails switchable through a power management unit."""
    DCDC1 = ...
    DCDC2 = ...
    DCDC3 = ...
    DCDC4 = ...
    DCDC5 = ...
    ALDO1 = ...
    ALDO2 = ...
    ALDO3 = ...
    ALDO4 = ...
    BLDO1 = ...
    BLDO2 = ...
    DLDO1 = ...
    DLDO2 = ...
    VBACKUP = ...


class PMUError(OSError):
    """Raised when a power management operation cannot be completed."""
    ...


class PMU(ABC):
    """Abstract contract for power management units."""
    @abstractmethod
    def open(self) -> None:
        """Open the device and verify its power management unit."""
        ...

    @abstractmethod
    def close(self) -> None:
        """Deactivate the device without closing its peripheral bus."""
        ...

    def __enter__(self) -> PMU:
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
    def battery_voltage(self) -> int:
        """Return the battery voltage in millivolts."""
        ...

    @abstractmethod
    def battery_percent(self) -> int:
        """Return the battery percentage, or ``-1`` when no battery is present."""
        ...

    @abstractmethod
    def is_charging(self) -> bool:
        """Return whether the battery is charging."""
        ...

    @abstractmethod
    def set_power(self, channel: PMUPowerChannel, enable: bool) -> None:
        """Enable or disable a power channel."""
        ...

    @abstractmethod
    def poweroff(self) -> None:
        """Request immediate power management unit poweroff."""
        ...

    @abstractmethod
    def reboot(self) -> None:
        """Request a power management unit reboot if supported."""
        ...



pmu_drivers = ...
