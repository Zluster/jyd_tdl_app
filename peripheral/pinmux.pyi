# ruff: noqa: E402
"""Pin-multiplexing register access."""

class PinMuxError(OSError):
    """Raised when a pin-multiplexing register cannot be accessed."""
    ...


class PinMux:
    """Access 32-bit pin-multiplexing registers through memory-mapped I/O."""
    @classmethod
    def set_bits(cls, phys_addr: int, bit_offset: int, bit_width: int, value: int) -> None:
        """Replace a bit field in the 32-bit register at ``address``."""
        ...
