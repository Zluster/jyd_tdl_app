# ruff: noqa: E402
"""Linux IIO ADC access for inputs declared in the board pin map."""

from .pinmap import ADCInfo
class ADCError(OSError):
    """Raised when an ADC operation cannot be completed."""
    ...


class ADC:
    """An ADC input exposed through the Linux IIO sysfs interface."""
    info: ADCInfo
    def __init__(self, id: int | str, resolution: int | None = ..., vref: int | float | None = ..., *, auto_open: bool = ...) -> None:
        """Create an ADC input and optionally open its mapped IIO value file."""
        ...

    def open(self) -> None:
        """Open or re-open the mapped IIO raw-value file."""
        ...

    def close(self) -> None:
        """Close the mapped IIO raw-value file."""
        ...

    def __enter__(self) -> ADC:
        """Open the ADC if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the ADC when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the mapped IIO raw-value file is open."""
        ...

    def read_raw(self) -> int:
        """Return the current unscaled ADC sample."""
        ...

    def read_vol(self) -> float:
        """Return the current ADC sample converted to volts."""
        ...
