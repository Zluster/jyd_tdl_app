# ruff: noqa: E402
"""I2C access to buses declared in the board pin map."""

from ._periphery.i2c import I2C as _PeripheryI2C
from .pinmap import I2CInfo

class I2CError(OSError):
    """Raised when an I2C operation cannot be completed."""
    ...


class I2C:
    """An I2C bus selected by its board configuration identifier.

    ``I2C(0)`` selects the ``I2C0`` entry in :class:`PinMap`. Named entries,
    such as ``I2C("I2C0")``, are also supported.
    """
    info: I2CInfo
    _periphery_instance: _PeripheryI2C | None
    def __init__(self, id: int | str, *, auto_open: bool = ...) -> None:
        """Create an I2C bus and optionally open its mapped device."""
        ...

    def open(self) -> None:
        """Open or re-open the mapped I2C device."""
        ...

    def close(self) -> None:
        """Close the mapped I2C device."""
        ...

    def __enter__(self) -> I2C:
        """Open the I2C bus if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the I2C bus when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the mapped I2C device is open."""
        ...

    def scan(self) -> list[int]:
        """Return the 7-bit addresses of devices that acknowledge the bus."""
        ...

    def read(self, addr: int, nbytes: int) -> bytes:
        """Read ``nbytes`` from ``addr`` and return them as bytes."""
        ...

    def write(self, addr: int, data: bytes) -> int:
        """Write ``data`` to ``addr`` and return the number of bytes written."""
        ...

    def read_mem(self, addr: int, memaddr: int, nbytes: int) -> bytes:
        """Read bytes from an 8-bit register address using a combined transfer."""
        ...

    def write_mem(self, addr: int, memaddr: int, data: bytes) -> int:
        """Write bytes to an 8-bit register address and return their count."""
        ...

    def read_byte(self, addr: int) -> int:
        """Read and return one byte from ``addr``."""
        ...

    def write_byte(self, addr: int, value: int) -> int:
        """Write one byte to ``addr`` and return the number of bytes written."""
        ...

    def read_mem_byte(self, addr: int, memaddr: int) -> int:
        """Read and return one byte from an 8-bit register address."""
        ...

    def write_mem_byte(self, addr: int, memaddr: int, value: int) -> int:
        """Write one byte to an 8-bit register and return the number written."""
        ...
    def _transfer(self, address: int, *messages: _PeripheryI2C.Message) -> None: ...
