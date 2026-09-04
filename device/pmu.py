"""Power management unit data types shared by concrete drivers."""


from enum import Enum


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
