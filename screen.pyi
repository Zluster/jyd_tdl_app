"""LCD backlight control through Linux backlight sysfs."""

BACKLIGHT_PATH: str


class ScreenError(OSError):
    """Raised when the LCD backlight sysfs device cannot be used."""
    ...


def get_brightness() -> int:
    """Return the configured native brightness level."""
    ...

def get_actual_brightness() -> int:
    """Return the brightness level currently applied by the driver."""
    ...

def get_max_brightness() -> int:
    """Return the largest valid native brightness level."""
    ...

def get_scale() -> str:
    """Return the driver's brightness scale."""
    ...

def set_brightness(value: int) -> int:
    """Set a brightness in the inclusive range ``0..get_max_brightness()``."""
    ...

def is_enabled() -> bool:
    """Return whether the backlight is unblanked."""
