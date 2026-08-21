# ruff: noqa: E402
"""SPI access to buses declared in the board pin map."""

from ._periphery.spi import SPI as _PeripherySPI
from .pinmap import SPIInfo
from .gpio import GPIO

_SPI_NO_CS = ...
class SPIError(OSError):
    """Raised when an SPI operation cannot be completed."""
    ...


class SPI:
    """An SPI bus selected by its bus and chip-select identifiers.

    ``SPI(0, cs=0)`` selects ``SPI0.0``. With ``soft_cs=True``, ``cs`` is a
    GPIO identifier driven around each transfer.
    """
    info: SPIInfo
    _periphery_instance: _PeripherySPI | None
    _cs_gpio: GPIO | None
    def __init__(self, id: int | str, freq: int = ..., polarity: int = ..., phase: int = ..., bits: int = ..., cs: int | str | None = ..., soft_cs: bool = ..., *, auto_open: bool = ...) -> None:
        """Create an SPI bus and optionally open its mapped device.

        When ``soft_cs`` is false, ``cs`` selects the configured hardware
        chip-select entry. When true, ``cs`` selects a GPIO output.
        """
        ...

    def open(self) -> None:
        """Open or re-open the mapped SPI device."""
        ...

    def close(self) -> None:
        """Close the mapped SPI device."""
        ...

    def __enter__(self) -> SPI:
        """Open the SPI bus if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the SPI bus when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the mapped SPI device is open."""
        ...

    def get_freq(self) -> int:
        """Return the SPI clock frequency in Hertz."""
        ...

    def set_freq(self, value: int) -> None:
        """Set the SPI clock frequency in Hertz."""
        ...

    def get_mode(self) -> tuple[int, int]:
        """Return the SPI clock polarity and phase."""
        ...

    def set_mode(self, polarity: int, phase: int) -> None:
        """Set the SPI clock polarity and phase, each to 0 or 1."""
        ...

    def read(self, nbytes: int) -> bytes:
        """Read ``nbytes`` from the SPI device."""
        ...

    def write(self, data: bytes) -> None:
        """Write a byte buffer to the SPI device."""
        ...

    def write_read(self, data: bytes) -> bytes:
        """Transmit bytes and return the simultaneously received bytes."""
        ...
    def _transfer(self, data: bytes) -> bytes: ...
    @property
    def _backend(self) -> _PeripherySPI: ...
