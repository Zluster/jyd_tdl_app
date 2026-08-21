# ruff: noqa: E402
"""Raw byte-stream access to UART devices declared in the board pin map."""

from _typeshed import ReadableBuffer, WriteableBuffer
from enum import Enum
from io import RawIOBase
from types import TracebackType
from ._periphery.serial import Serial as _PeripherySerial
from .pinmap import UARTInfo

class UARTError(OSError):
    """Raised when a UART operation cannot be completed."""
    ...


class UARTParity(str, Enum):
    """Parity modes supported by a UART connection."""
    NONE = ...
    EVEN = ...
    ODD = ...


class UART(RawIOBase):
    """A UART selected by its board configuration identifier.

    ``UART(0, 115200)`` selects the ``UART0`` entry in :class:`PinMap`.
    Named entries, such as ``UART("UART0", 115200)``, are also supported.
    """
    info: UARTInfo
    _periphery_instance: _PeripherySerial | None
    def __init__(self, id: int | str, baudrate: int = ..., bits: int = ..., parity: UARTParity = ..., stop: float = ..., *, auto_open: bool = ...) -> None:
        """Create a UART and optionally open its mapped serial device."""
        ...

    def open(self) -> None:
        """Open or re-open the configured serial device."""
        ...

    def close(self) -> None:
        """Flush and close the UART device."""
        ...

    def __enter__(self) -> UART:
        """Open the UART if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, exc_type: type[BaseException] | None, exc_val: BaseException | None, exc_tb: TracebackType | None) -> None:
        """Close the UART when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the configured serial device is open."""
        ...

    def available(self) -> int:
        """Return the number of bytes currently available to read."""
        ...

    def flush(self) -> None:
        """Flush buffered output to the UART device."""
        ...

    def readable(self) -> bool:
        """Return whether the UART device can be read."""
        ...

    def writable(self) -> bool:
        """Return whether the UART device can be written."""
        ...

    def seekable(self) -> bool:
        """Return ``False`` because UART streams do not support seeking."""
        ...

    def readall(self) -> bytes:
        """Read all bytes currently available from the UART device."""
        ...

    def readinto(self, buffer: WriteableBuffer, *, timeout: float | None = ...) -> int | None:
        """Read bytes into a buffer with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def read(self, size: int = ..., *, timeout: float | None = ...) -> bytes | None:
        """Read up to ``size`` bytes with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def readline(self, size: int | None = ..., *, timeout: float | None = ...) -> bytes:
        """Read one line with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def readlines(self, hint: int = ..., *, timeout: float | None = ...) -> list[bytes]:
        """Read lines with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def write(self, data: ReadableBuffer | list[int]) -> int | None:
        """Write bytes from a buffer to the UART device."""
        ...

    def readall_str(self, encoding: str = ...) -> str:
        """Read all available bytes and decode them using ``encoding``."""
        ...

    def read_str(self, size: int = ..., encoding: str = ..., *, timeout: float | None = ...) -> str | None:
        """Read and decode up to ``size`` bytes with the selected timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def write_str(self, data: str, encoding: str = ...) -> int | None:
        """Encode text with ``encoding`` and write it to the UART device."""
        ...

    def readline_str(self, size: int = ..., encoding: str = ..., *, timeout: float | None = ...) -> str:
        """Read and decode one line with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...

    def readlines_str(self, hint: int = ..., encoding: str = ..., *, timeout: float | None = ...) -> list[str]:
        """Read and decode lines with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        ...
    @property
    def _backend(self) -> _PeripherySerial: ...
