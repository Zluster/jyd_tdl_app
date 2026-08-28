"""Low-level ZW101 control over the cascaded sensor UART."""

from __future__ import annotations

import errno
import time
from dataclasses import dataclass

from jydbus_uart import (JYDBUS_FRAME_TYPE_QUERY, JYDBUS_TYPE_ZW101,
                         JydbusUart)

ZW101_CONTROL_DEFAULT_TIMEOUT_MS = 35000
ZW101_CONTROL_MAX_TEMPLATE_ID = 49
ZW101_CONTROL_RESULT_MARKER = 0xA5

ZW101_CONTROL_ENROLL = 1
ZW101_CONTROL_MATCH = 2
ZW101_CONTROL_DELETE = 3
ZW101_CONTROL_CLEAR_DATABASE = 4


@dataclass
class ZW101Result:
    operation: int = 0
    status: int = 0
    module_status: int = 0
    fingerprint_id: int = 0
    score: int = 0
    response_ms: int = 0


def _validate(sensor_number: int, command: int, fingerprint_id: int) -> None:
    if sensor_number == 0:
        raise OSError(errno.EINVAL, "node number must be nonzero")
    if command not in range(ZW101_CONTROL_ENROLL, ZW101_CONTROL_CLEAR_DATABASE + 1):
        raise OSError(errno.EINVAL, "invalid ZW101 command")
    if command in (ZW101_CONTROL_ENROLL, ZW101_CONTROL_DELETE) and not 0 <= fingerprint_id <= 49:
        raise OSError(errno.ERANGE, "fingerprint ID must be 0..49")


def send_zw101_command(uart: JydbusUart, sensor_number: int,
                       command: int, fingerprint_id: int = 0) -> int:
    _validate(sensor_number, command, fingerprint_id)
    return uart.write(JYDBUS_FRAME_TYPE_QUERY, JYDBUS_TYPE_ZW101, sensor_number,
                      bytes((command, fingerprint_id, 0, 0)))


def run_zw101_command(uart: JydbusUart, sensor_number: int, command: int,
                      fingerprint_id: int = 0,
                      timeout_ms: int = ZW101_CONTROL_DEFAULT_TIMEOUT_MS) -> ZW101Result:
    if timeout_ms <= 0:
        raise OSError(errno.EINVAL, "timeout must be positive")
    _validate(sensor_number, command, fingerprint_id)
    try:
        previous_sequence = uart.read_cached(JYDBUS_TYPE_ZW101,
                                             sensor_number).sequence
    except OSError:
        previous_sequence = 0
    send_zw101_command(uart, sensor_number, command, fingerprint_id)
    started = time.monotonic()
    deadline = started + timeout_ms / 1000.0
    while time.monotonic() < deadline:
        try:
            data = uart.read_cached(JYDBUS_TYPE_ZW101, sensor_number)
        except OSError:
            data = None
        if data is not None and data.sequence != previous_sequence:
            previous_sequence = data.sequence
            value = data.value
            if (data.decoded_valid and value.get("result_marker") == ZW101_CONTROL_RESULT_MARKER
                    and value.get("operation") == command):
                return ZW101Result(
                    operation=value["operation"], status=value["status"],
                    module_status=value["module_status"],
                    fingerprint_id=value["fingerprint_id"], score=value["score"],
                    response_ms=int((time.monotonic() - started) * 1000))
            if (data.decoded_valid and data.raw_length >= 2
                    and value.get("operation") == command and value.get("status") != 0):
                return ZW101Result(
                    operation=command, status=value["status"],
                    response_ms=int((time.monotonic() - started) * 1000))
        time.sleep(0.010)
    raise TimeoutError(errno.ETIMEDOUT, "ZW101 operation timed out")


def zw101_status_name(status: int) -> str:
    names = ("ok", "busy", "invalid-param", "timeout", "protocol-error",
             "uart-overflow", "module-error")
    return names[status] if 0 <= status < len(names) else "unknown"
