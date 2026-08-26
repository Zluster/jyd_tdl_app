"""Thread-safe high-level ZW101 API corresponding to zw101_api.c."""

from __future__ import annotations

import errno
import threading

from jydbus_uart import JydbusUart
from zw101_control import (ZW101_CONTROL_CLEAR_DATABASE, ZW101_CONTROL_DELETE,
                           ZW101_CONTROL_ENROLL, ZW101_CONTROL_MATCH,
                           ZW101_CONTROL_DEFAULT_TIMEOUT_MS, Zw101ControlResult,
                           zw101_control_execute)

ZW101_API_UART_BAUD = 115200
Zw101Result = Zw101ControlResult


class Zw101Device:
    def __init__(self, uart_device: str, node_number: int = 1) -> None:
        if node_number == 0:
            raise OSError(errno.EINVAL, "node number must be nonzero")
        self.uart = JydbusUart(uart_device, ZW101_API_UART_BAUD)
        self.node_number = node_number
        self.timeout_ms = ZW101_CONTROL_DEFAULT_TIMEOUT_MS
        self._operation_lock = threading.Lock()

    def close(self) -> None:
        self.uart.close()

    def set_timeout(self, timeout_ms: int) -> None:
        if timeout_ms <= 0:
            raise OSError(errno.EINVAL, "timeout must be positive")
        with self._operation_lock:
            self.timeout_ms = timeout_ms

    def _execute(self, command: int, fingerprint_id: int = 0) -> Zw101Result:
        with self._operation_lock:
            result = zw101_control_execute(self.uart, self.node_number, command,
                                           fingerprint_id, self.timeout_ms)
        status_errno = {1: errno.EBUSY, 2: errno.EINVAL, 3: errno.ETIMEDOUT,
                        4: errno.EPROTO, 5: errno.EOVERFLOW, 6: errno.EIO}
        if result.status:
            code = status_errno.get(result.status, errno.EIO)
            error = OSError(code, zw101_status_string(result.status))
            error.result = result  # type: ignore[attr-defined]
            raise error
        return result

    def enroll(self, fingerprint_id: int) -> Zw101Result:
        return self._execute(ZW101_CONTROL_ENROLL, fingerprint_id)

    def match(self) -> Zw101Result:
        return self._execute(ZW101_CONTROL_MATCH)

    def delete(self, fingerprint_id: int) -> Zw101Result:
        return self._execute(ZW101_CONTROL_DELETE, fingerprint_id)

    def clear(self) -> Zw101Result:
        return self._execute(ZW101_CONTROL_CLEAR_DATABASE)

    def __enter__(self) -> "Zw101Device":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()


def zw101_status_string(status: int) -> str:
    names = ("ok", "busy", "invalid-param", "timeout", "protocol-error",
             "uart-overflow", "module-error")
    return names[status] if 0 <= status < len(names) else "unknown"


def zw101_open(uart_device: str, node_number: int = 1) -> Zw101Device:
    return Zw101Device(uart_device, node_number)


def zw101_close(device: Zw101Device) -> None:
    device.close()


def zw101_set_timeout(device: Zw101Device, timeout_ms: int) -> None:
    device.set_timeout(timeout_ms)


def zw101_enroll(device: Zw101Device, fingerprint_id: int) -> Zw101Result:
    return device.enroll(fingerprint_id)


def zw101_match(device: Zw101Device) -> tuple[int, int, Zw101Result]:
    result = device.match()
    return result.fingerprint_id, result.score, result


def zw101_delete(device: Zw101Device, fingerprint_id: int) -> Zw101Result:
    return device.delete(fingerprint_id)


def zw101_clear(device: Zw101Device) -> Zw101Result:
    return device.clear()
