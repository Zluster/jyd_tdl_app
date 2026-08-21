"""Linux UART driver for the cascaded GD32 sensor protocol."""

from __future__ import annotations

import copy
import errno
import os
import select
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Any

from serial_port import SerialPort

SENSOR_UART_MAX_PAYLOAD = 32
SENSOR_UART_SENSOR_NUMBER_MIN = 1
SENSOR_UART_SENSOR_NUMBER_MAX = 8
SENSOR_UART_SUPPORTED_TYPE_COUNT = 15
SENSOR_UART_CACHE_CAPACITY = SENSOR_UART_SUPPORTED_TYPE_COUNT * SENSOR_UART_SENSOR_NUMBER_MAX

SENSOR_FRAME_TYPE_QUERY = 0x01
SENSOR_FRAME_TYPE_DATA = 0x02
SENSOR_FRAME_TYPE_SCAN = 0x03
SENSOR_FRAME_TYPE_CONFIG = 0x04

SENSOR_TYPE_AHT10 = 0x02
SENSOR_TYPE_BMP390 = 0x03
SENSOR_TYPE_MAX30102 = 0x04
SENSOR_TYPE_VL53L0X = 0x05
SENSOR_TYPE_MFRC522 = 0x06
SENSOR_TYPE_WS2812B = 0x07
SENSOR_TYPE_ZW101 = 0x08
SENSOR_TYPE_BUTTON_PB1 = 0x09
SENSOR_TYPE_JOYSTICK = 0x0A
SENSOR_TYPE_PHOTORESISTOR_ADC = 0x0F
SENSOR_TYPE_WATER_LEVEL_ADC = 0x11
SENSOR_TYPE_SOIL_MOISTURE_ADC = 0x12
SENSOR_TYPE_ZSPD4003 = 0x13
SENSOR_TYPE_KNOB_SWITCH_ADC = 0x14
SENSOR_TYPE_PAJ7620U2 = 0x15

SENSOR_ZSPD4003_STATUS_OK = 0
SENSOR_ZSPD4003_STATUS_WARMING_UP = 1
SENSOR_ZSPD4003_STATUS_NO_FINGER = 2
SENSOR_ZSPD4003_STATUS_I2C_ERROR = 3
SENSOR_ZSPD4003_STATUS_BAD_SIGNAL = 4

SENSOR_PAJ7620U2_GESTURE_NONE = 0
SENSOR_PAJ7620U2_GESTURE_UP = 1
SENSOR_PAJ7620U2_GESTURE_DOWN = 2
SENSOR_PAJ7620U2_GESTURE_LEFT = 3
SENSOR_PAJ7620U2_GESTURE_RIGHT = 4
SENSOR_PAJ7620U2_GESTURE_FORWARD = 5
SENSOR_PAJ7620U2_GESTURE_BACKWARD = 6
SENSOR_PAJ7620U2_GESTURE_CLOCKWISE = 7
SENSOR_PAJ7620U2_GESTURE_COUNTERCLOCKWISE = 8
SENSOR_PAJ7620U2_GESTURE_WAVE = 9

SENSOR_PAJ7620U2_STATUS_OK = 0
SENSOR_PAJ7620U2_STATUS_INITIALIZING = 1
SENSOR_PAJ7620U2_STATUS_I2C_ERROR = 2
SENSOR_PAJ7620U2_STATUS_DEVICE_NOT_FOUND = 3
SENSOR_PAJ7620U2_STATUS_CONFIGURATION_ERROR = 4
SENSOR_PAJ7620U2_STATUS_SUSPENDED = 5
SENSOR_NUMBER_FIRST = 1

SENSOR_UART_COMMAND_SCAN = 1
SENSOR_UART_COMMAND_QUERY_ALL = 2
SENSOR_UART_COMMAND_ENABLE_AUTO_UPLOAD = 3
SENSOR_UART_COMMAND_DISABLE_AUTO_UPLOAD = 4
SENSOR_UART_COMMAND_SET_WS2812B_RED = 5
SENSOR_UART_COMMAND_READ_SENSOR = 6
SENSOR_UART_COMMAND_WRITE_SENSOR = 7
SENSOR_UART_COMMAND_QUERY_SENSOR = 8

FRAME_HEADER = 0x55
FRAME_TAIL = 0xAA
FRAME_OVERHEAD = 8
RX_FRAME_TIMEOUT = 0.030
QUERY_RESPONSE_TIMEOUT = 0.300
QUERY_HOP_TIMEOUT = 0.004
AUTO_UPLOAD_INTERVAL_MS = 1000
PAJ7620_AUTO_UPLOAD_INTERVAL_MS = 100
CONFIG_FRAME_GAP = AUTO_UPLOAD_INTERVAL_MS / SENSOR_UART_SENSOR_NUMBER_MAX / 1000.0

COMMAND_TARGETS = (
    SENSOR_TYPE_PHOTORESISTOR_ADC, SENSOR_TYPE_AHT10, SENSOR_TYPE_BMP390,
    SENSOR_TYPE_MAX30102, SENSOR_TYPE_VL53L0X, SENSOR_TYPE_MFRC522,
    SENSOR_TYPE_BUTTON_PB1, SENSOR_TYPE_JOYSTICK, SENSOR_TYPE_WATER_LEVEL_ADC,
    SENSOR_TYPE_SOIL_MOISTURE_ADC, SENSOR_TYPE_ZSPD4003,
    SENSOR_TYPE_KNOB_SWITCH_ADC, SENSOR_TYPE_PAJ7620U2,
)


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


@dataclass
class SensorData:
    frame_type: int = 0
    sensor_type: int = 0
    sensor_number: int = 0
    frame_tail: int = 0
    received_crc: int = 0
    raw: bytes = b""
    decoded_valid: bool = False
    value: dict[str, Any] = field(default_factory=dict)
    sequence: int = 0
    updated_monotonic_ms: int = 0
    received_monotonic_us: int = 0

    @property
    def raw_length(self) -> int:
        return len(self.raw)


@dataclass
class SensorUartStats:
    valid_frames: int = 0
    crc_errors: int = 0
    format_errors: int = 0
    query_echoes: int = 0
    legacy_tails: int = 0


@dataclass
class SensorUartCommandStep:
    sensor_type: int = 0
    sensor_number: int = 0
    send_result: int = 0
    response_received: bool = False
    response_ms: int = 0
    response_us: int = 0


@dataclass
class SensorUartCommandResult:
    command: int = 0
    status: int = 0
    sensor_type: int = 0
    sensor_number: int = 0
    value: int = 0
    data_valid: bool = False
    data: SensorData | None = None
    steps: list[SensorUartCommandStep] = field(default_factory=list)


def _decode(sensor_type: int, data: bytes) -> tuple[bool, dict[str, Any]]:
    try:
        if sensor_type == SENSOR_TYPE_PHOTORESISTOR_ADC and len(data) >= 4:
            return True, {"adc": struct.unpack_from("<H", data, 2)[0]}
        if sensor_type == SENSOR_TYPE_AHT10 and len(data) >= 8:
            temperature, humidity = struct.unpack_from("<ff", data)
            return True, {"temperature_c": temperature, "humidity_percent": humidity}
        if sensor_type == SENSOR_TYPE_BMP390 and len(data) >= 8:
            temperature, pressure = struct.unpack_from("<ff", data)
            return True, {"temperature_c": temperature, "pressure_pa": pressure}
        if sensor_type == SENSOR_TYPE_MAX30102 and len(data) >= 8:
            return True, {"heart_rate_bpm": struct.unpack_from("<H", data)[0],
                          "spo2_percent": struct.unpack_from("<H", data, 4)[0]}
        if sensor_type == SENSOR_TYPE_VL53L0X and len(data) >= 4:
            return True, {"distance_mm": struct.unpack_from("<I", data)[0]}
        if sensor_type == SENSOR_TYPE_MFRC522 and len(data) >= 8:
            return True, {"uid": data[:4], "tag_type": data[4:6], "present": data[6] != 0}
        if sensor_type == SENSOR_TYPE_WS2812B and len(data) >= 4:
            return True, {"ws2812b_ack": struct.unpack_from("<I", data)[0]}
        if sensor_type == SENSOR_TYPE_ZW101 and len(data) >= 2:
            value = {"operation": data[0], "status": data[1], "module_status": 0,
                     "fingerprint_id": 0, "score": 0, "result_marker": 0}
            if len(data) >= 8:
                value.update(module_status=data[2],
                             fingerprint_id=struct.unpack_from("<H", data, 3)[0],
                             score=struct.unpack_from("<H", data, 5)[0],
                             result_marker=data[7])
            return True, value
        if sensor_type == SENSOR_TYPE_BUTTON_PB1 and len(data) >= 1:
            return True, {"button_level": 1 if data[0] else 0}
        if sensor_type == SENSOR_TYPE_JOYSTICK and len(data) >= 4:
            x_adc, y_adc = struct.unpack_from("<HH", data)
            return True, {"x_adc": x_adc, "y_adc": y_adc}
        if sensor_type == SENSOR_TYPE_WATER_LEVEL_ADC and len(data) >= 2:
            return True, {"water_level_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == SENSOR_TYPE_SOIL_MOISTURE_ADC and len(data) >= 2:
            return True, {"soil_moisture_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == SENSOR_TYPE_ZSPD4003 and len(data) >= 4:
            return True, {"heart_rate_bpm": data[0], "spo2_percent": data[1],
                          "status": data[2], "signal_quality": data[3]}
        if sensor_type == SENSOR_TYPE_KNOB_SWITCH_ADC and len(data) >= 2:
            return True, {"knob_switch_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == SENSOR_TYPE_PAJ7620U2 and len(data) >= 4:
            return True, {"gesture": data[0], "gesture_flags": data[1],
                          "wave_flags": data[2], "status": data[3]}
    except struct.error:
        pass
    return False, {}


class SensorUart:
    def __init__(self, device: str, baud_rate: int = 115200) -> None:
        self.port = SerialPort(device, baud_rate)
        self._tx_lock = threading.Lock()
        self._cache_lock = threading.Lock()
        self._cache: dict[tuple[int, int], SensorData] = {}
        self._scan_cache: list[tuple[int, int]] = []
        self._stats = SensorUartStats()
        self._frame = bytearray()
        self._expected = 0
        self._last_rx = 0.0
        self._sequence = 0
        self._running = True
        self._thread = threading.Thread(target=self._rx_main, name="sensor-uart-rx", daemon=True)
        self._thread.start()

    def close(self) -> None:
        if not self._running:
            return
        self._running = False
        self._thread.join(timeout=1.0)
        self.port.close()

    def __enter__(self) -> "SensorUart":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @staticmethod
    def build_frame(frame_type: int, sensor_type: int, sensor_number: int,
                    payload: bytes = b"") -> bytes:
        if len(payload) > SENSOR_UART_MAX_PAYLOAD:
            raise OSError(errno.EINVAL, "payload exceeds 32 bytes")
        body = bytes((len(payload), frame_type & 0xFF, sensor_type & 0xFF,
                      sensor_number & 0xFF)) + payload
        crc = crc16_modbus(body)
        return bytes((FRAME_HEADER,)) + body + struct.pack("<H", crc) + bytes((FRAME_TAIL,))

    def _write_timed(self, frame_type: int, sensor_type: int, sensor_number: int,
                     payload: bytes = b"") -> int:
        frame = self.build_frame(frame_type, sensor_type, sensor_number, payload)
        with self._tx_lock:
            started_us = time.monotonic_ns() // 1000
            self.port.write_all(frame)
        return started_us

    def write(self, frame_type: int, sensor_type: int, sensor_number: int,
              payload: bytes = b"") -> int:
        self._write_timed(frame_type, sensor_type, sensor_number, payload)
        return 0

    def query(self, sensor_type: int, sensor_number: int) -> int:
        self._validate_number(sensor_number)
        payload = bytearray(4)
        if sensor_type == SENSOR_TYPE_WS2812B:
            payload[1] = 0x55
        elif sensor_type != SENSOR_TYPE_ZW101:
            payload[3] = 0x72
        self.write(SENSOR_FRAME_TYPE_QUERY, sensor_type, sensor_number, payload)
        return 0

    def scan(self) -> int:
        self.write(SENSOR_FRAME_TYPE_SCAN, 0, 0, b"<<<<")
        return 0

    def set_value(self, sensor_type: int, sensor_number: int, value: int) -> int:
        self._validate_number(sensor_number)
        self.write(SENSOR_FRAME_TYPE_QUERY, sensor_type, sensor_number,
                   struct.pack("<I", value & 0xFFFFFFFF))
        return 0

    def set_auto_upload(self, sensor_type: int, sensor_number: int,
                        enabled: bool, interval_ms: int) -> int:
        self._validate_number(sensor_number)
        if not 0 <= interval_ms <= 0xFFFF:
            raise OSError(errno.EINVAL, "auto-upload interval must be 0..65535 ms")
        payload = struct.pack("<BHB", int(enabled), interval_ms, 0)
        self.write(SENSOR_FRAME_TYPE_CONFIG, sensor_type, sensor_number, payload)
        return 0

    def read(self, sensor_type: int, sensor_number: int) -> SensorData:
        self._validate_number(sensor_number)
        with self._cache_lock:
            data = self._cache.get((sensor_type, sensor_number))
            if data is None:
                raise OSError(errno.ENODATA, "no cached sensor data")
            return copy.deepcopy(data)

    def read_all(self) -> list[SensorData]:
        with self._cache_lock:
            return copy.deepcopy(list(self._cache.values()))

    def get_stats(self) -> SensorUartStats:
        with self._cache_lock:
            return copy.copy(self._stats)

    def execute_command(self, command: int, sensor_type: int = 0,
                        sensor_number: int = 0, value: int = 0) -> SensorUartCommandResult:
        result = SensorUartCommandResult(command=command, sensor_type=sensor_type,
                                         sensor_number=sensor_number, value=value)
        try:
            if command == SENSOR_UART_COMMAND_SCAN:
                with self._cache_lock:
                    self._scan_cache.clear()
                self.scan()
                time.sleep(QUERY_RESPONSE_TIMEOUT)
                with self._cache_lock:
                    result.steps = [SensorUartCommandStep(t, n, response_received=True)
                                    for t, n in self._scan_cache]
            elif command == SENSOR_UART_COMMAND_QUERY_ALL:
                for target in COMMAND_TARGETS:
                    result.steps.append(self._query_and_wait(target, SENSOR_NUMBER_FIRST,
                                                             all_query=True))
            elif command in (SENSOR_UART_COMMAND_ENABLE_AUTO_UPLOAD,
                             SENSOR_UART_COMMAND_DISABLE_AUTO_UPLOAD):
                enabled = command == SENSOR_UART_COMMAND_ENABLE_AUTO_UPLOAD
                for target in COMMAND_TARGETS:
                    interval_ms = (PAJ7620_AUTO_UPLOAD_INTERVAL_MS
                                   if target == SENSOR_TYPE_PAJ7620U2
                                   else AUTO_UPLOAD_INTERVAL_MS)
                    step = SensorUartCommandStep(target, SENSOR_UART_SENSOR_NUMBER_MAX,
                                                 response_received=True)
                    for number in range(SENSOR_UART_SENSOR_NUMBER_MAX, 0, -1):
                        try:
                            self.set_auto_upload(target, number, enabled,
                                                 interval_ms if enabled else 0)
                        except OSError as exc:
                            if step.send_result == 0:
                                step.send_result = -int(exc.errno or errno.EIO)
                        time.sleep(CONFIG_FRAME_GAP)
                    result.steps.append(step)
                first_error = next((step.send_result for step in result.steps
                                    if step.send_result != 0), 0)
                result.status = first_error
            elif command == SENSOR_UART_COMMAND_SET_WS2812B_RED:
                self.set_value(SENSOR_TYPE_WS2812B, SENSOR_NUMBER_FIRST, 0x00FF0000)
                result.steps.append(SensorUartCommandStep(SENSOR_TYPE_WS2812B,
                                                          SENSOR_NUMBER_FIRST))
            elif command == SENSOR_UART_COMMAND_READ_SENSOR:
                result.data = self.read(sensor_type, sensor_number)
                result.data_valid = True
            elif command == SENSOR_UART_COMMAND_WRITE_SENSOR:
                self.set_value(sensor_type, sensor_number, value)
            elif command == SENSOR_UART_COMMAND_QUERY_SENSOR:
                step = self._query_and_wait(sensor_type, sensor_number)
                result.steps.append(step)
                if step.response_received:
                    result.data = self.read(sensor_type, sensor_number)
                    result.data_valid = True
            else:
                raise OSError(errno.EINVAL, "invalid command")
        except OSError as exc:
            result.status = -int(exc.errno or errno.EIO)
        return result

    def _query_and_wait(self, sensor_type: int, sensor_number: int,
                        all_query: bool = False) -> SensorUartCommandStep:
        step = SensorUartCommandStep(sensor_type, sensor_number)
        try:
            previous = self.read(sensor_type, sensor_number).sequence
        except OSError:
            previous = 0
        try:
            if all_query:
                started = self._write_timed(SENSOR_FRAME_TYPE_QUERY, sensor_type,
                                            sensor_number, bytes(4))
            else:
                payload = bytearray(4)
                if sensor_type == SENSOR_TYPE_WS2812B:
                    payload[1] = 0x55
                elif sensor_type != SENSOR_TYPE_ZW101:
                    payload[3] = 0x72
                started = self._write_timed(SENSOR_FRAME_TYPE_QUERY, sensor_type,
                                            sensor_number, payload)
        except OSError as exc:
            step.send_result = -int(exc.errno or errno.EIO)
            return step
        deadline = started / 1_000_000 + QUERY_RESPONSE_TIMEOUT + sensor_number * QUERY_HOP_TIMEOUT
        while time.monotonic() < deadline:
            try:
                data = self.read(sensor_type, sensor_number)
                if data.sequence != previous and data.received_monotonic_us >= started:
                    step.response_received = True
                    step.response_us = data.received_monotonic_us - started
                    step.response_ms = step.response_us // 1000
                    break
            except OSError:
                pass
            time.sleep(0.001)
        return step

    @staticmethod
    def _validate_number(sensor_number: int) -> None:
        if not SENSOR_UART_SENSOR_NUMBER_MIN <= sensor_number <= SENSOR_UART_SENSOR_NUMBER_MAX:
            raise OSError(errno.EINVAL, "sensor number must be 1..8")

    def _rx_main(self) -> None:
        while self._running:
            try:
                readable, _, exceptional = select.select([self.port.fd], [], [self.port.fd], 0.020)
                if exceptional:
                    break
                if readable:
                    for value in self.port.read_available(256):
                        self._push_byte(value)
                if self._frame and time.monotonic() - self._last_rx >= RX_FRAME_TIMEOUT:
                    with self._cache_lock:
                        self._stats.format_errors += 1
                    self._reset_parser()
            except (OSError, ValueError):
                if self._running:
                    break

    def _reset_parser(self) -> None:
        self._frame.clear()
        self._expected = 0

    def _push_byte(self, value: int) -> None:
        self._last_rx = time.monotonic()
        if not self._frame:
            if value == FRAME_HEADER:
                self._frame.append(value)
            return
        if len(self._frame) >= SENSOR_UART_MAX_PAYLOAD + FRAME_OVERHEAD:
            with self._cache_lock:
                self._stats.format_errors += 1
            self._reset_parser()
            if value == FRAME_HEADER:
                self._frame.append(value)
            return
        self._frame.append(value)
        if len(self._frame) == 2:
            if self._frame[1] > SENSOR_UART_MAX_PAYLOAD:
                with self._cache_lock:
                    self._stats.format_errors += 1
                self._reset_parser()
                return
            self._expected = self._frame[1] + FRAME_OVERHEAD
        if self._expected and len(self._frame) == self._expected:
            self._process_frame(bytes(self._frame), time.monotonic_ns() // 1000)
            self._reset_parser()

    def _process_frame(self, frame: bytes, received_us: int) -> None:
        payload_length = frame[1]
        crc_index = payload_length + 5
        received_crc = struct.unpack_from("<H", frame, crc_index)[0]
        if received_crc != crc16_modbus(frame[1:crc_index]):
            with self._cache_lock:
                self._stats.crc_errors += 1
            return
        frame_type = frame[2]
        if frame_type == SENSOR_FRAME_TYPE_QUERY:
            with self._cache_lock:
                self._stats.query_echoes += 1
            return
        if frame_type not in (SENSOR_FRAME_TYPE_DATA, SENSOR_FRAME_TYPE_SCAN):
            with self._cache_lock:
                self._stats.format_errors += 1
            return
        tail = frame[-1]
        legacy_tail = payload_length == 8 and tail == 0
        if tail != FRAME_TAIL and not legacy_tail:
            with self._cache_lock:
                self._stats.format_errors += 1
            return
        sensor_type, sensor_number = frame[3], frame[4]
        payload = frame[5:crc_index]
        with self._cache_lock:
            if legacy_tail:
                self._stats.legacy_tails += 1
            if frame_type == SENSOR_FRAME_TYPE_SCAN:
                if len(payload) < 4 or payload[:4] != b"KKKK":
                    self._stats.format_errors += 1
                    return
                self._stats.valid_frames += 1
                target = (sensor_type, sensor_number)
                if target not in self._scan_cache and len(self._scan_cache) < SENSOR_UART_CACHE_CAPACITY:
                    self._scan_cache.append(target)
                return
            self._stats.valid_frames += 1
            if not SENSOR_UART_SENSOR_NUMBER_MIN <= sensor_number <= SENSOR_UART_SENSOR_NUMBER_MAX:
                return
            if (sensor_type, sensor_number) not in self._cache and len(self._cache) >= SENSOR_UART_CACHE_CAPACITY:
                return
            valid, value = _decode(sensor_type, payload)
            self._sequence += 1
            self._cache[(sensor_type, sensor_number)] = SensorData(
                frame_type, sensor_type, sensor_number, tail, received_crc,
                payload, valid, value, self._sequence, received_us // 1000, received_us)


def sensor_name(sensor_type: int) -> str:
    return {
        SENSOR_TYPE_AHT10: "AHT10", SENSOR_TYPE_BMP390: "BMP390",
        SENSOR_TYPE_MAX30102: "MAX30102", SENSOR_TYPE_VL53L0X: "VL53L0X",
        SENSOR_TYPE_MFRC522: "MFRC522", SENSOR_TYPE_WS2812B: "WS2812B",
        SENSOR_TYPE_ZW101: "ZW101", SENSOR_TYPE_BUTTON_PB1: "BUTTON_PB1",
        SENSOR_TYPE_JOYSTICK: "JOYSTICK",
        SENSOR_TYPE_PHOTORESISTOR_ADC: "PHOTORESISTOR_ADC",
        SENSOR_TYPE_WATER_LEVEL_ADC: "WATER_LEVEL_ADC",
        SENSOR_TYPE_SOIL_MOISTURE_ADC: "SOIL_MOISTURE_ADC",
        SENSOR_TYPE_ZSPD4003: "ZSPD4003",
        SENSOR_TYPE_KNOB_SWITCH_ADC: "KNOB_SWITCH_ADC",
        SENSOR_TYPE_PAJ7620U2: "PAJ7620U2",
    }.get(sensor_type, "UNKNOWN")


def paj7620_gesture_name(gesture: int) -> str:
    return {
        SENSOR_PAJ7620U2_GESTURE_NONE: "none",
        SENSOR_PAJ7620U2_GESTURE_UP: "up",
        SENSOR_PAJ7620U2_GESTURE_DOWN: "down",
        SENSOR_PAJ7620U2_GESTURE_LEFT: "left",
        SENSOR_PAJ7620U2_GESTURE_RIGHT: "right",
        SENSOR_PAJ7620U2_GESTURE_FORWARD: "forward",
        SENSOR_PAJ7620U2_GESTURE_BACKWARD: "backward",
        SENSOR_PAJ7620U2_GESTURE_CLOCKWISE: "clockwise",
        SENSOR_PAJ7620U2_GESTURE_COUNTERCLOCKWISE: "counterclockwise",
        SENSOR_PAJ7620U2_GESTURE_WAVE: "wave",
    }.get(gesture, "unknown")


def paj7620_status_name(status: int) -> str:
    return {
        SENSOR_PAJ7620U2_STATUS_OK: "ok",
        SENSOR_PAJ7620U2_STATUS_INITIALIZING: "initializing",
        SENSOR_PAJ7620U2_STATUS_I2C_ERROR: "i2c-error",
        SENSOR_PAJ7620U2_STATUS_DEVICE_NOT_FOUND: "device-not-found",
        SENSOR_PAJ7620U2_STATUS_CONFIGURATION_ERROR: "configuration-error",
        SENSOR_PAJ7620U2_STATUS_SUSPENDED: "suspended",
    }.get(status, "unknown")


# Function-style compatibility helpers.
def sensor_uart_open(device: str, baud_rate: int = 115200) -> SensorUart:
    return SensorUart(device, baud_rate)


def sensor_uart_close(uart: SensorUart) -> None:
    uart.close()


def sensor_read(uart: SensorUart, sensor_type: int, sensor_number: int) -> SensorData:
    return uart.read(sensor_type, sensor_number)


def sensor_query(uart: SensorUart, sensor_type: int, sensor_number: int) -> int:
    return uart.query(sensor_type, sensor_number)


def sensor_write(uart: SensorUart, sensor_type: int, sensor_number: int, value: int) -> int:
    return uart.set_value(sensor_type, sensor_number, value)
