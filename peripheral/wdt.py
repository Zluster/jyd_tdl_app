"""Linux watchdog timer access."""


from array import array
import fcntl
import os

from dara.core._error_helpers import wrap_error_as

from .pinmap import PinMap


_WDT_DEVICE = "/dev/watchdog"
_WDIOC_SETTIMEOUT = 0xC0045706


class WDTError(OSError):
    """Raised when a watchdog timer operation cannot be completed."""


class WDT:
    """A Linux watchdog timer backed by ``/dev/watchdog``."""


    def __init__(self, timeout_ms = 8000, *, auto_open = True):
        """Create a watchdog with a timeout in milliseconds.

        Linux watchdog drivers accept whole-second timeouts, so values are
        rounded up to ensure the requested timeout is not shortened.
        """
        if (
            isinstance(timeout_ms, bool)
            or not isinstance(timeout_ms, int)
            or timeout_ms <= 0
        ):
            raise ValueError("timeout_ms must be a positive integer")
        self.timeout_ms = timeout_ms
        self.info = PinMap.get_wdt()
        self._fd = None
        if auto_open:
            self.open()

    @wrap_error_as(WDTError, "WDT open failed", catch=OSError)
    def open(self):
        """Open the watchdog and configure its timeout."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise WDTError("WDT init command failed") from error
            if return_code:
                raise WDTError(
                    f"WDT init command failed with exit status {return_code}"
                )
        self._fd = os.open(_WDT_DEVICE, os.O_WRONLY)
        try:
            timeout = array("i", [(self.timeout_ms + 999) // 1_000])
            fcntl.ioctl(self._fd, _WDIOC_SETTIMEOUT, timeout, True)
        except Exception:
            self.close()
            raise

    @wrap_error_as(WDTError, "WDT close failed", catch=OSError)
    def close(self):
        """Stop the watchdog with Linux's magic close sequence."""
        if self._fd is None:
            return
        fd, self._fd = self._fd, None
        try:
            os.write(fd, b"V")
        finally:
            os.close(fd)

    def __enter__(self):
        """Open the watchdog if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the watchdog when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the watchdog device is open."""
        return self._fd is not None

    @wrap_error_as(WDTError, "WDT feed failed", catch=OSError)
    def feed(self):
        """Reset the watchdog countdown."""
        if self._fd is None:
            raise WDTError("WDT is not open")
        os.write(self._fd, b"\0")
