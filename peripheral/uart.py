"""Raw byte-stream access to UART devices declared in the board pin map."""


from enum import Enum
from io import RawIOBase

from ._periphery.serial import Serial as _PeripherySerial
from ._periphery.serial import SerialError as _PeripherySerialError

from dara.core._error_helpers import wrap_error_as

from .pinmap import PinMap


class UARTError(OSError):
    """Raised when a UART operation cannot be completed."""


class UARTParity(str, Enum):
    """Parity modes supported by a UART connection."""

    NONE = "none"
    EVEN = "even"
    ODD = "odd"


class UART(RawIOBase):
    """A UART selected by its board configuration identifier.

    ``UART(0, 115200)`` selects the ``UART0`` entry in :class:`PinMap`.
    Named entries, such as ``UART("UART0", 115200)``, are also supported.
    """


    def __init__(
        self,
        id,
        baudrate = 115200,
        bits = 8,
        parity = UARTParity.NONE,
        stop = 1,
        *,
        auto_open = True,
    ):
        """Create a UART and optionally open its mapped serial device."""
        super().__init__()
        self.id = id
        self.info = PinMap.get_uart(id)
        if not isinstance(parity, UARTParity):
            raise ValueError("parity must be a UARTParity value")
        if stop not in (1, 2):
            raise ValueError("stop must be 1 or 2")
        self._baudrate = baudrate
        self._bits = bits
        self._parity = parity
        self._stop = int(stop)
        self._periphery_instance = None
        if auto_open:
            self.open()

    @wrap_error_as(UARTError, "UART open failed", catch=_PeripherySerialError)
    def open(self):
        """Open or re-open the configured serial device."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise UARTError("UART init command failed") from error
            if return_code:
                raise UARTError(
                    f"UART init command failed with exit status {return_code}"
                )
        if self.info.tx is not None and self.info.tx_func is not None:
            PinMap.set_pin_function(self.info.tx, self.info.tx_func)
        if self.info.rx is not None and self.info.rx_func is not None:
            PinMap.set_pin_function(self.info.rx, self.info.rx_func)
        self._periphery_instance = _PeripherySerial(
            self.info.dev,
            self._baudrate,
            databits=self._bits,
            parity=self._parity.value,
            stopbits=self._stop,
        )

    @wrap_error_as(UARTError, "UART close failed", catch=_PeripherySerialError)
    def close(self):
        """Flush and close the UART device."""
        if self._periphery_instance is not None:
            self._periphery_instance.close()
            self._periphery_instance = None

    def __enter__(self):
        """Open the UART if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(
        self,
        exc_type,
        exc_val,
        exc_tb,
    ):
        """Close the UART when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the configured serial device is open."""
        return self._periphery_instance is not None

    @wrap_error_as(UARTError, "UART available failed", catch=_PeripherySerialError)
    def available(self):
        """Return the number of bytes currently available to read."""
        return self._backend.input_waiting()

    @wrap_error_as(UARTError, "UART flush failed", catch=_PeripherySerialError)
    def flush(self):
        """Flush buffered output to the UART device."""
        if self._periphery_instance is not None:
            self._periphery_instance.flush()

    def readable(self):
        """Return whether the UART device can be read."""
        return self.is_opened

    def writable(self):
        """Return whether the UART device can be written."""
        return self.is_opened

    def seekable(self):
        """Return ``False`` because UART streams do not support seeking."""
        return False

    def readall(self):
        """Read all bytes currently available from the UART device."""
        return self.read(self.available(), timeout=0) or b""

    def readinto(
        self,
        buffer,
        *,
        timeout = None,
    ):
        """Read bytes into a buffer with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        data = self.read(len(memoryview(buffer)), timeout=timeout)
        if data is None:
            return None
        memoryview(buffer)[: len(data)] = data
        return len(data)

    @wrap_error_as(UARTError, "UART read failed", catch=_PeripherySerialError)
    def read(
        self,
        size = -1,
        *,
        timeout = None,
    ):
        """Read up to ``size`` bytes with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        if size < 0:
            size = self.available()
        return self._backend.read(size, timeout)

    def readline(  # pyright: ignore[reportIncompatibleMethodOverride]
        self,
        size = -1,
        *,
        timeout = None,
    ):  # pyright: ignore[reportIncompatibleMethodOverride]
        """Read one line with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        limit = None if size is None or size < 0 else size
        line = bytearray()
        while limit is None or len(line) < limit:
            data = self.read(1, timeout=timeout)
            if not data:
                break
            line.extend(data)
            if data == b"\n":
                break
        return bytes(line)

    def readlines(
        self,
        hint = -1,
        *,
        timeout = None,
    ):
        """Read lines with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        lines = []
        total = 0
        while hint < 0 or total < hint:
            line = self.readline(timeout=timeout)
            if not line:
                break
            lines.append(line)
            total += len(line)
        return lines

    @wrap_error_as(UARTError, "UART write failed", catch=_PeripherySerialError)
    def write(self, data):
        """Write bytes from a buffer to the UART device."""
        return self._backend.write(data)

    def readall_str(self, encoding = "utf-8"):
        """Read all available bytes and decode them using ``encoding``."""
        return self.readall().decode(encoding)

    def read_str(
        self,
        size = -1,
        encoding = "utf-8",
        *,
        timeout = None,
    ):
        """Read and decode up to ``size`` bytes with the selected timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        data = self.read(size, timeout=timeout)
        return None if data is None else data.decode(encoding)

    def write_str(self, data, encoding = "utf-8"):
        """Encode text with ``encoding`` and write it to the UART device."""
        return self.write(data.encode(encoding))

    def readline_str(
        self,
        size = -1,
        encoding = "utf-8",
        *,
        timeout = None,
    ):
        """Read and decode one line with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        return self.readline(size, timeout=timeout).decode(encoding)

    def readlines_str(
        self,
        hint = -1,
        encoding = "utf-8",
        *,
        timeout = None,
    ):
        """Read and decode lines with the selected receive timeout.

        ``None`` and ``-1`` wait indefinitely,
        ``0`` does not wait, and a positive value waits that many seconds.
        """
        return [line.decode(encoding) for line in self.readlines(hint, timeout=timeout)]

    @property
    def _backend(self):
        """Return the open serial backend."""
        if self._periphery_instance is None:
            raise UARTError("UART is not open")
        return self._periphery_instance
