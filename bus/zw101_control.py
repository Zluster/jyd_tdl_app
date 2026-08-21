"""Low-level ZW101 control over the cascaded sensor UART."""

from __future__ import annotations

import errno
import time
from dataclasses import dataclass

from sensor_uart import (SENSOR_FRAME_TYPE_QUERY, SENSOR_TYPE_ZW101,
                         SensorUart)

ZW101_CONTROL_DEFAULT_TIMEOUT_MS = 35000
ZW101_CONTROL_MAX_TEMPLATE_ID = 49
ZW101_CONTROL_RESULT_MARKER = 0xA5

ZW101_CONTROL_ENROLL = 1
ZW101_CONTROL_MATCH = 2
ZW101_CONTROL_DELETE = 3
ZW101_CONTROL_CLEAR_DATABASE = 4


@dataclass
class Zw101ControlResult:
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


def zw101_control_send(uart: SensorUart, sensor_number: int,
                       command: int, fingerprint_id: int = 0) -> int:
    _validate(sensor_number, command, fingerprint_id)
    return uart.write(SENSOR_FRAME_TYPE_QUERY, SENSOR_TYPE_ZW101, sensor_number,
                      bytes((command, fingerprint_id, 0, 0)))


def zw101_control_execute(uart: SensorUart, sensor_number: int, command: int,
                          fingerprint_id: int = 0,
                          timeout_ms: int = ZW101_CONTROL_DEFAULT_TIMEOUT_MS) -> Zw101ControlResult:
    if timeout_ms <= 0:
        raise OSError(errno.EINVAL, "timeout must be positive")
    _validate(sensor_number, command, fingerprint_id)
    try:
        previous_sequence = uart.read(SENSOR_TYPE_ZW101, sensor_number).sequence
    except OSError:
        previous_sequence = 0
    zw101_control_send(uart, sensor_number, command, fingerprint_id)
    started = time.monotonic()
    deadline = started + timeout_ms / 1000.0
    while time.monotonic() < deadline:
        try:
            data = uart.read(SENSOR_TYPE_ZW101, sensor_number)
        except OSError:
            data = None
        if data is not None and data.sequence != previous_sequence:
            previous_sequence = data.sequence
            value = data.value
            if (data.decoded_valid and value.get("result_marker") == ZW101_CONTROL_RESULT_MARKER
                    and value.get("operation") == command):
                return Zw101ControlResult(
                    operation=value["operation"], status=value["status"],
                    module_status=value["module_status"],
                    fingerprint_id=value["fingerprint_id"], score=value["score"],
                    response_ms=int((time.monotonic() - started) * 1000))
            if (data.decoded_valid and data.raw_length >= 2
                    and value.get("operation") == command and value.get("status") != 0):
                return Zw101ControlResult(operation=command, status=value["status"],
                                          response_ms=int((time.monotonic() - started) * 1000))
        time.sleep(0.010)
    raise TimeoutError(errno.ETIMEDOUT, "ZW101 operation timed out")


def zw101_control_enroll(uart: SensorUart, sensor_number: int,
                         fingerprint_id: int) -> int:
    return zw101_control_send(uart, sensor_number, ZW101_CONTROL_ENROLL, fingerprint_id)


def zw101_control_match(uart: SensorUart, sensor_number: int,
                        timeout_ms: int = ZW101_CONTROL_DEFAULT_TIMEOUT_MS) -> Zw101ControlResult:
    return zw101_control_execute(uart, sensor_number, ZW101_CONTROL_MATCH, 0, timeout_ms)


def zw101_control_delete(uart: SensorUart, sensor_number: int,
                         fingerprint_id: int) -> int:
    return zw101_control_send(uart, sensor_number, ZW101_CONTROL_DELETE, fingerprint_id)


def zw101_control_clear_database(uart: SensorUart, sensor_number: int) -> int:
    return zw101_control_send(uart, sensor_number, ZW101_CONTROL_CLEAR_DATABASE, 0)
