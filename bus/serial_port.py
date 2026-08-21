"""Small POSIX serial-port helper using only the Python standard library."""

from __future__ import annotations

import errno
import os
import select
import sys
import time

if os.name == "posix":
    import termios
    import tty


class SerialPort:
    """Non-blocking 8N1 serial port with deadline based reads and writes."""

    _BAUD_NAMES = {
        9600: "B9600",
        19200: "B19200",
        38400: "B38400",
        57600: "B57600",
        115200: "B115200",
        230400: "B230400",
    }

    def __init__(self, device: str, baud_rate: int) -> None:
        if os.name != "posix":
            raise OSError(errno.ENOSYS, "serial access requires Linux/POSIX")
        baud_name = self._BAUD_NAMES.get(baud_rate)
        speed = getattr(termios, baud_name, None) if baud_name else None
        if speed is None:
            raise OSError(errno.EINVAL, f"unsupported baud rate: {baud_rate}")

        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(self.fd, termios.TCSANOW)
            settings = termios.tcgetattr(self.fd)
            settings[2] |= termios.CLOCAL | termios.CREAD
            settings[2] &= ~(termios.CSTOPB | termios.PARENB)
            if hasattr(termios, "CRTSCTS"):
                settings[2] &= ~termios.CRTSCTS
            settings[2] = (settings[2] & ~termios.CSIZE) | termios.CS8
            settings[4] = speed
            settings[5] = speed
            settings[6][termios.VMIN] = 0
            settings[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except BaseException:
            os.close(self.fd)
            self.fd = -1
            raise

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def flush(self) -> None:
        if self.fd >= 0:
            termios.tcflush(self.fd, termios.TCIOFLUSH)

    def drain(self) -> None:
        termios.tcdrain(self.fd)

    def read_available(self, maximum: int = 256) -> bytes:
        try:
            return os.read(self.fd, maximum)
        except BlockingIOError:
            return b""
        except InterruptedError:
            return b""

    def read_exact(self, length: int, timeout: float) -> bytes:
        data = bytearray()
        deadline = time.monotonic() + timeout
        while len(data) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(errno.ETIMEDOUT, "operation timed out")
            readable, _, _ = select.select([self.fd], [], [], remaining)
            if not readable:
                raise TimeoutError(errno.ETIMEDOUT, "operation timed out")
            chunk = self.read_available(length - len(data))
            if chunk:
                data.extend(chunk)
        return bytes(data)

    def write_all(self, data: bytes, timeout: float = 0.5, drain: bool = False) -> None:
        view = memoryview(data)
        sent = 0
        deadline = time.monotonic() + timeout
        while sent < len(view):
            try:
                count = os.write(self.fd, view[sent:])
                if count == 0:
                    raise OSError(errno.EIO, "serial write returned zero")
                sent += count
            except InterruptedError:
                continue
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(errno.ETIMEDOUT, "serial write timed out")
                _, writable, _ = select.select([], [self.fd], [], remaining)
                if not writable:
                    raise TimeoutError(errno.ETIMEDOUT, "serial write timed out")
        if drain:
            self.drain()

    def __enter__(self) -> "SerialPort":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()


def ensure_linux() -> None:
    if os.name != "posix":
        print("This program controls a Linux serial device and must run on Linux.", file=sys.stderr)
        raise SystemExit(1)
