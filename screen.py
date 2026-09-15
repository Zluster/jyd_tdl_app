"""LCD backlight control through the Linux backlight sysfs device.

The values exposed here are the driver's native brightness levels.  Read
:func:`get_max_brightness` first rather than assuming a 0..100 percentage.
For the current JYD LCD the expected root is ``/sys/class/backlight/backlight``.
"""

from __future__ import annotations

import os


class ScreenError(OSError):
    """Raised when the LCD backlight sysfs device cannot be used."""


BACKLIGHT_PATH = "/sys/class/backlight/backlight"


def _path(name: str) -> str:
    return os.path.join(BACKLIGHT_PATH, name)


def _read_int(name: str) -> int:
    try:
        with open(_path(name), encoding="ascii") as source:
            return int(source.read().strip())
    except (OSError, ValueError) as error:
        raise ScreenError("cannot read backlight %s: %s" % (name, error)) from error


def _write_int(name: str, value: int) -> None:
    try:
        with open(_path(name), "w", encoding="ascii") as target:
            target.write("%d\n" % value)
    except OSError as error:
        raise ScreenError("cannot write backlight %s: %s" % (name, error)) from error


def get_brightness() -> int:
    """Return the configured native brightness level."""
    return _read_int("brightness")


def get_actual_brightness() -> int:
    """Return the brightness level currently applied by the driver."""
    return _read_int("actual_brightness")


def get_max_brightness() -> int:
    """Return the largest valid native brightness level."""
    return _read_int("max_brightness")


def get_scale() -> str:
    """Return the driver's brightness scale, for example ``"non-linear"``."""
    try:
        with open(_path("scale"), encoding="ascii") as source:
            return source.read().strip()
    except OSError as error:
        raise ScreenError("cannot read backlight scale: %s" % error) from error


def set_brightness(value: int) -> int:
    """Set native brightness and return the configured value.

    ``value`` must be an integer from zero through
    :func:`get_max_brightness`.  A value of zero turns off the backlight but
    does not change the display power state.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("brightness must be an integer")
    maximum = get_max_brightness()
    if not 0 <= value <= maximum:
        raise ValueError("brightness must be in 0..%d" % maximum)
    _write_int("brightness", value)
    return get_brightness()


def is_enabled() -> bool:
    """Return whether the backlight is unblanked (``bl_power == 0``)."""
    return _read_int("bl_power") == 0


def set_enabled(enabled: bool) -> bool:
    """Unblank or power down the LCD backlight and return the new state.

    Linux backlight sysfs uses ``0`` for unblank and ``4`` for power down.
    This method changes only ``bl_power``; it preserves the configured
    brightness so a later enable restores the previous level.
    """
    if not isinstance(enabled, bool):
        raise ValueError("enabled must be a bool")
    _write_int("bl_power", 0 if enabled else 4)
    return is_enabled()


__all__ = [
    "BACKLIGHT_PATH",
    "ScreenError",
    "get_brightness",
    "get_actual_brightness",
    "get_max_brightness",
    "get_scale",
    "set_brightness",
    "is_enabled",
    "set_enabled",
]
