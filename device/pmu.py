"""Power management unit types and abstract contract."""


from abc import ABC, abstractmethod
from enum import Enum

from dara.core.registry import Registry


class PMUPowerChannel(str, Enum):
    """Power rails switchable through a power management unit."""

    DCDC1 = "dcdc1"
    DCDC2 = "dcdc2"
    DCDC3 = "dcdc3"
    DCDC4 = "dcdc4"
    DCDC5 = "dcdc5"
    ALDO1 = "aldo1"
    ALDO2 = "aldo2"
    ALDO3 = "aldo3"
    ALDO4 = "aldo4"
    BLDO1 = "bldo1"
    BLDO2 = "bldo2"
    DLDO1 = "dldo1"
    DLDO2 = "dldo2"
    VBACKUP = "vbackup"


class PMUError(OSError):
    """Raised when a power management operation cannot be completed."""


class PMU(ABC):
    """Abstract contract for power management units."""

    @abstractmethod
    def open(self):
        """Open the device and verify its power management unit."""

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
    def battery_voltage(self):
        """Return the battery voltage in millivolts."""

    @abstractmethod
    def battery_percent(self):
        """Return the battery percentage, or ``-1`` when no battery is present."""

    @abstractmethod
    def is_charging(self):
        """Return whether the battery is charging."""

    @abstractmethod
    def set_power(self, channel, enable):
        """Enable or disable a power channel."""

    @abstractmethod
    def poweroff(self):
        """Request immediate power management unit poweroff."""

    @abstractmethod
    def reboot(self):
        """Request a power management unit reboot if supported."""


pmu_drivers = Registry[PMU]("PMU drivers")
"""Registry of available power management unit driver classes."""
