"""Pin-multiplexing register access."""

from dara.peripheral._periphery.mmio import MMIO as _PeripheryMMIO
from dara.peripheral._periphery.mmio import MMIOError as _PeripheryMMIOError


class PinMuxError(OSError):
    """Raised when a pin-multiplexing register cannot be accessed."""


class PinMux:
    """Access 32-bit pin-multiplexing registers through memory-mapped I/O."""

    @classmethod
    def set_bits(cls, phys_addr, bit_offset, bit_width, value):
        """Replace a bit field in the 32-bit register at ``address``."""
        if (
            isinstance(phys_addr, bool)
            or not isinstance(phys_addr, int)
            or phys_addr < 0
            or isinstance(bit_offset, bool)
            or not isinstance(bit_offset, int)
            or isinstance(bit_width, bool)
            or not isinstance(bit_width, int)
            or not 0 <= bit_offset < 32
            or not 1 <= bit_width <= 32 - bit_offset
            or isinstance(value, bool)
            or not isinstance(value, int)
            or not 0 <= value < 1 << bit_width
        ):
            raise ValueError("invalid 32-bit register bit field")

        mask = ((1 << bit_width) - 1) << bit_offset
        try:
            with _PeripheryMMIO(phys_addr, 4) as memory:
                memory.write32(0, (memory.read32(0) & ~mask) | (value << bit_offset))
        except _PeripheryMMIOError as error:
            raise PinMuxError("pin mux register access failed") from error
