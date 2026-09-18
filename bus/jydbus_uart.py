"""Linux UART driver for the cascaded GD32 Jydbus protocol."""

from __future__ import annotations

import copy
import errno
import os
import select
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any

if __package__:
    from .serial_port import SerialPort
else:
    from serial_port import SerialPort

# Match GD32's UART_RX_FRAME_MAX_PAYLOAD. Fingerprint parameter diagnostics
# contain 16 metadata bytes + 17 response bytes (33 total), and up to 69 bytes.
JYDBUS_UART_MAX_PAYLOAD = 255
JYDBUS_UART_MAX_TX_PAYLOAD = 196
JYDBUS_UART_JYDBUS_NUMBER_MIN = 1
JYDBUS_UART_JYDBUS_NUMBER_MAX = 8
JYDBUS_UART_SUPPORTED_TYPE_COUNT = 16
JYDBUS_UART_CACHE_CAPACITY = JYDBUS_UART_SUPPORTED_TYPE_COUNT * JYDBUS_UART_JYDBUS_NUMBER_MAX
JYDBUS_UART_HISTORY_CAPACITY = 32

JYDBUS_FRAME_TYPE_QUERY = 0x01
JYDBUS_FRAME_TYPE_DATA = 0x02
JYDBUS_FRAME_TYPE_SCAN = 0x03
JYDBUS_FRAME_TYPE_CONFIG = 0x04

JYDBUS_TYPE_AHT10 = 0x02
JYDBUS_TYPE_BMP390 = 0x03
JYDBUS_TYPE_MAX30102 = 0x04
JYDBUS_TYPE_VL53L0X = 0x05
JYDBUS_TYPE_MFRC522 = 0x06
JYDBUS_TYPE_WS2812B = 0x07
JYDBUS_TYPE_ZW101 = 0x08
JYDBUS_TYPE_BUTTON_PB1 = 0x09
JYDBUS_TYPE_JOYSTICK = 0x0A
JYDBUS_TYPE_PHOTORESISTOR_ADC = 0x0F
JYDBUS_TYPE_WATER_LEVEL_ADC = 0x11
JYDBUS_TYPE_SOIL_MOISTURE_ADC = 0x12
JYDBUS_TYPE_ZSPD4003 = 0x13
JYDBUS_TYPE_KNOB_SWITCH_ADC = 0x14
JYDBUS_TYPE_PAJ7620U2 = 0x15
JYDBUS_TYPE_FAN = 0x17

JYDBUS_ZSPD4003_STATUS_OK = 0
JYDBUS_ZSPD4003_STATUS_WARMING_UP = 1
JYDBUS_ZSPD4003_STATUS_NO_FINGER = 2
JYDBUS_ZSPD4003_STATUS_I2C_ERROR = 3
JYDBUS_ZSPD4003_STATUS_BAD_SIGNAL = 4

JYDBUS_PAJ7620U2_GESTURE_NONE = 0
JYDBUS_PAJ7620U2_GESTURE_UP = 1
JYDBUS_PAJ7620U2_GESTURE_DOWN = 2
JYDBUS_PAJ7620U2_GESTURE_LEFT = 3
JYDBUS_PAJ7620U2_GESTURE_RIGHT = 4
JYDBUS_PAJ7620U2_GESTURE_FORWARD = 5
JYDBUS_PAJ7620U2_GESTURE_BACKWARD = 6
JYDBUS_PAJ7620U2_GESTURE_CLOCKWISE = 7
JYDBUS_PAJ7620U2_GESTURE_COUNTERCLOCKWISE = 8
JYDBUS_PAJ7620U2_GESTURE_WAVE = 9

JYDBUS_PAJ7620U2_STATUS_OK = 0
JYDBUS_PAJ7620U2_STATUS_INITIALIZING = 1
JYDBUS_PAJ7620U2_STATUS_I2C_ERROR = 2
JYDBUS_PAJ7620U2_STATUS_DEVICE_NOT_FOUND = 3
JYDBUS_PAJ7620U2_STATUS_CONFIGURATION_ERROR = 4
JYDBUS_PAJ7620U2_STATUS_SUSPENDED = 5
JYDBUS_NUMBER_FIRST = 1

JYDBUS_UART_COMMAND_SCAN = 1
JYDBUS_UART_COMMAND_QUERY_ALL = 2
JYDBUS_UART_COMMAND_ENABLE_AUTO_UPLOAD = 3
JYDBUS_UART_COMMAND_DISABLE_AUTO_UPLOAD = 4
JYDBUS_UART_COMMAND_SET_WS2812B_RED = 5
JYDBUS_UART_COMMAND_READ_SENSOR = 6
JYDBUS_UART_COMMAND_WRITE_SENSOR = 7
JYDBUS_UART_COMMAND_QUERY_SENSOR = 8

WS2812B_LED_COUNT = 128
WS2812B_FRAME_CHUNK_LEDS = 64
WS2812B_COMMAND_SET_PIXEL = 0x01
WS2812B_COMMAND_FRAME_BEGIN = 0x02
WS2812B_COMMAND_FRAME_CHUNK = 0x03
WS2812B_COMMAND_FRAME_COMMIT = 0x04
WS2812B_COMMAND_GET_STATUS = 0x05
WS2812B_STATUS_OK = 0x00
MFRC522_COMMAND_READ_BLOCK = 0x01
MFRC522_COMMAND_WRITE_BLOCK = 0x02
MFRC522_COMMAND_STATUS_OK = 0x00
MFRC522_COMMAND_STATUS_NO_CARD = 0x01
MFRC522_COMMAND_STATUS_ERROR = 0x02
MFRC522_COMMAND_STATUS_INVALID = 0x03
MFRC522_CLASSIC_1K_LAST_BLOCK = 63

FRAME_HEADER = 0x55
FRAME_TAIL = 0xAA
# header + 2-byte payload length + type + sensor type + sensor number + CRC16 + tail
FRAME_OVERHEAD = 9
RX_FRAME_TIMEOUT = 0.030
QUERY_RESPONSE_TIMEOUT = 0.300
QUERY_HOP_TIMEOUT = 0.004
AUTO_UPLOAD_INTERVAL_MS = 1000
PAJ7620_AUTO_UPLOAD_INTERVAL_MS = 100
CONFIG_FRAME_GAP = AUTO_UPLOAD_INTERVAL_MS / JYDBUS_UART_JYDBUS_NUMBER_MAX / 1000.0
WS2812B_REQUEST_RETRIES = 3
WS2812B_FRAME_RETRIES = 3
# A 196-byte payload occupies about 17.8 ms at 115200 baud. Each GD32 node
# forwards it only after reception, so the next frame must not arrive while the
# receive flag is still owned by the main loop.
WS2812B_FRAME_GAP = 0.025
WS2812B_COMMAND_GAP = 0.002

COMMAND_TARGETS = (
    JYDBUS_TYPE_PHOTORESISTOR_ADC, JYDBUS_TYPE_AHT10, JYDBUS_TYPE_BMP390,
    JYDBUS_TYPE_MAX30102, JYDBUS_TYPE_VL53L0X, JYDBUS_TYPE_MFRC522,
    JYDBUS_TYPE_BUTTON_PB1, JYDBUS_TYPE_JOYSTICK, JYDBUS_TYPE_WATER_LEVEL_ADC,
    JYDBUS_TYPE_SOIL_MOISTURE_ADC, JYDBUS_TYPE_ZSPD4003,
    JYDBUS_TYPE_KNOB_SWITCH_ADC, JYDBUS_TYPE_PAJ7620U2, JYDBUS_TYPE_FAN,
)


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


@dataclass
class JydbusData:
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
class JydbusUartStats:
    valid_frames: int = 0
    crc_errors: int = 0
    format_errors: int = 0
    query_echoes: int = 0
    legacy_tails: int = 0
    rx_errors: int = 0
    last_rx_error: str = ""
    last_format_reason: str = ""
    last_format_frame: bytes = b""


@dataclass
class JydbusUartCommandStep:
    sensor_type: int = 0
    sensor_number: int = 0
    send_result: int = 0
    response_received: bool = False
    response_ms: int = 0
    response_us: int = 0


@dataclass
class JydbusUartCommandResult:
    command: int = 0
    status: int = 0
    sensor_type: int = 0
    sensor_number: int = 0
    value: int = 0
    data_valid: bool = False
    data: JydbusData | None = None
    steps: list[JydbusUartCommandStep] = field(default_factory=list)


def _decode(sensor_type: int, data: bytes) -> tuple[bool, dict[str, Any]]:
    try:
        if sensor_type == JYDBUS_TYPE_PHOTORESISTOR_ADC and len(data) >= 4:
            return True, {"adc": struct.unpack_from("<H", data, 2)[0]}
        if sensor_type == JYDBUS_TYPE_AHT10 and len(data) >= 8:
            temperature, humidity = struct.unpack_from("<ff", data)
            return True, {"temperature_c": temperature, "humidity_percent": humidity}
        if sensor_type == JYDBUS_TYPE_BMP390 and len(data) >= 8:
            temperature, pressure = struct.unpack_from("<ff", data)
            return True, {"temperature_c": temperature, "pressure_pa": pressure}
        if sensor_type == JYDBUS_TYPE_MAX30102 and len(data) >= 8:
            return True, {"heart_rate_bpm": struct.unpack_from("<H", data)[0],
                          "spo2_percent": struct.unpack_from("<H", data, 4)[0]}
        if sensor_type == JYDBUS_TYPE_VL53L0X and len(data) >= 4:
            return True, {"distance_mm": struct.unpack_from("<I", data)[0]}
        if sensor_type == JYDBUS_TYPE_MFRC522:
            if (len(data) >= 20 and
                    data[0] in (MFRC522_COMMAND_READ_BLOCK,
                                MFRC522_COMMAND_WRITE_BLOCK)):
                return True, {"operation": data[0], "status": data[1],
                              "block": data[2], "block_data": data[4:20]}
            if len(data) >= 8:
                return True, {"uid": data[:4], "tag_type": data[4:6],
                              "present": data[6] in (1, 2),
                              "status": data[6], "version": data[7]}
        if sensor_type == JYDBUS_TYPE_WS2812B and len(data) >= 4:
            value = {"ws2812b_ack": struct.unpack_from("<I", data)[0]}
            if len(data) >= 8:
                value.update(command=data[0], status=data[1], argument0=data[2],
                             argument1=data[3], value=struct.unpack_from("<I", data, 4)[0])
            return True, value
        if sensor_type == JYDBUS_TYPE_ZW101 and len(data) >= 2:
            value = {"operation": data[0], "status": data[1], "module_status": 0,
                     "fingerprint_id": 0, "score": 0, "result_marker": 0}
            if len(data) >= 8:
                value.update(module_status=data[2],
                             fingerprint_id=struct.unpack_from("<H", data, 3)[0],
                             score=struct.unpack_from("<H", data, 5)[0],
                             result_marker=data[7])
            if ((len(data) == 16 and data[8] == 1) or
                    (len(data) >= 16 and data[0] == 5 and data[8] == 2 and
                     data[11] <= 53 and len(data) == 16 + data[11])):
                value.update(diagnostic_version=data[8], driver_phase=data[9],
                             module_command=data[10], ack_length=data[11],
                             ack=tuple(data[12:15]), enroll_progress=data[15])
                if data[8] == 2:
                    value["module_response"] = tuple(data[16:])
            return True, value
        if sensor_type == JYDBUS_TYPE_BUTTON_PB1 and len(data) >= 1:
            return True, {"button_level": 1 if data[0] else 0}
        if sensor_type == JYDBUS_TYPE_JOYSTICK and len(data) >= 4:
            x_adc, y_adc = struct.unpack_from("<HH", data)
            return True, {"x_adc": x_adc, "y_adc": y_adc}
        if sensor_type == JYDBUS_TYPE_WATER_LEVEL_ADC and len(data) >= 2:
            return True, {"water_level_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == JYDBUS_TYPE_SOIL_MOISTURE_ADC and len(data) >= 2:
            return True, {"soil_moisture_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == JYDBUS_TYPE_ZSPD4003 and len(data) >= 4:
            return True, {"heart_rate_bpm": data[0], "spo2_percent": data[1],
                          "status": data[2], "signal_quality": data[3]}
        if sensor_type == JYDBUS_TYPE_KNOB_SWITCH_ADC and len(data) >= 2:
            return True, {"knob_switch_adc": struct.unpack_from("<H", data)[0]}
        if sensor_type == JYDBUS_TYPE_PAJ7620U2 and len(data) >= 4:
            return True, {"gesture": data[0], "gesture_flags": data[1],
                          "wave_flags": data[2], "status": data[3]}
        if sensor_type == JYDBUS_TYPE_FAN and len(data) >= 4:
            duty_percent = min(struct.unpack_from("<I", data)[0], 100)
            return True, {"enabled": duty_percent != 0,
                          "state": 1 if duty_percent else 0,
                          "duty_percent": duty_percent}
    except struct.error:
        pass
    return False, {}


class JydbusUart:
    def __init__(self, device: str, baud_rate: int = 115200) -> None:
        self.port = SerialPort(device, baud_rate)
        self._tx_lock = threading.Lock()
        self._mfrc522_lock = threading.Lock()
        self._ws2812b_lock = threading.Lock()
        self._cache_lock = threading.Lock()
        self._cache: dict[tuple[int, int], JydbusData] = {}
        self._history: dict[tuple[int, int], deque[JydbusData]] = {}
        self._scan_cache: list[tuple[int, int]] = []
        self._stats = JydbusUartStats()
        self._rx_error: OSError | ValueError | None = None
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

    def __enter__(self) -> "JydbusUart":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @staticmethod
    def build_frame(frame_type: int, sensor_type: int, sensor_number: int,
                    payload: bytes = b"") -> bytes:
        if len(payload) > JYDBUS_UART_MAX_TX_PAYLOAD:
            raise OSError(errno.EINVAL, "payload exceeds transmit limit")
        body = struct.pack("<HBBB", len(payload), frame_type & 0xFF,
                           sensor_type & 0xFF, sensor_number & 0xFF) + payload
        crc = crc16_modbus(body)
        return bytes((FRAME_HEADER,)) + body + struct.pack("<H", crc) + bytes((FRAME_TAIL,))

    def _write_timed(self, frame_type: int, sensor_type: int, sensor_number: int,
                     payload: bytes = b"", drain: bool = False) -> int:
        frame = self.build_frame(frame_type, sensor_type, sensor_number, payload)
        with self._tx_lock:
            started_us = time.monotonic_ns() // 1000
            self.port.write_all(frame, drain=drain)
        return started_us

    def write(self, frame_type: int, sensor_type: int, sensor_number: int,
              payload: bytes = b"") -> int:
        self._write_timed(frame_type, sensor_type, sensor_number, payload)
        return 0

    def request_sensor(self, sensor_type: int, sensor_number: int) -> int:
        self._validate_number(sensor_number)
        if sensor_type == JYDBUS_TYPE_WS2812B:
            payload = bytearray((WS2812B_COMMAND_GET_STATUS,))
        else:
            payload = bytearray(4)
        if sensor_type not in (JYDBUS_TYPE_WS2812B, JYDBUS_TYPE_ZW101):
            payload[3] = 0x72
        self.write(JYDBUS_FRAME_TYPE_QUERY, sensor_type, sensor_number, payload)
        return 0

    def _mfrc522_block_command(self, sensor_number: int, command: int,
                               block: int, block_data: bytes = b"") -> bytes:
        self._validate_number(sensor_number)
        if (not isinstance(block, int) or isinstance(block, bool) or
                not 0 <= block <= MFRC522_CLASSIC_1K_LAST_BLOCK):
            raise OSError(errno.EINVAL, "MFRC522 block must be 0..63")
        if command == MFRC522_COMMAND_WRITE_BLOCK:
            if block == 0 or (block + 1) % 4 == 0:
                raise OSError(errno.EINVAL,
                              "MFRC522 manufacturer and sector trailer blocks "
                              "cannot be written")
            if len(block_data) != 16:
                raise OSError(errno.EINVAL,
                              "MFRC522 block data must be exactly 16 bytes")
        payload = bytes((command, block)) + block_data

        with self._mfrc522_lock:
            try:
                previous = self.read_cached(JYDBUS_TYPE_MFRC522,
                                            sensor_number).sequence
            except OSError:
                previous = 0
            started = self._write_timed(JYDBUS_FRAME_TYPE_QUERY,
                                        JYDBUS_TYPE_MFRC522,
                                        sensor_number, payload, drain=True)
            deadline = (started / 1_000_000 + QUERY_RESPONSE_TIMEOUT +
                        sensor_number * QUERY_HOP_TIMEOUT)
            while time.monotonic() < deadline:
                try:
                    response = self.read_cached(JYDBUS_TYPE_MFRC522,
                                                sensor_number)
                except OSError:
                    pass
                else:
                    value = response.value
                    if (response.sequence != previous and
                            response.received_monotonic_us >= started and
                            response.decoded_valid and
                            value.get("operation") == command and
                            value.get("block") == block):
                        status = int(value["status"])
                        if status == MFRC522_COMMAND_STATUS_OK:
                            return bytes(value["block_data"])
                        if status == MFRC522_COMMAND_STATUS_NO_CARD:
                            raise OSError(errno.ENODEV,
                                          "no compatible IC card present")
                        if status == MFRC522_COMMAND_STATUS_INVALID:
                            raise OSError(errno.EINVAL,
                                          "invalid MFRC522 block command")
                        raise OSError(errno.EIO,
                                      "MFRC522 authentication or block I/O failed")
                time.sleep(0.001)
        raise TimeoutError(errno.ETIMEDOUT,
                           f"MFRC522 node {sensor_number} timed out")

    def mfrc522_read_block(self, sensor_number: int, block: int) -> bytes:
        return self._mfrc522_block_command(
            sensor_number, MFRC522_COMMAND_READ_BLOCK, block)

    def mfrc522_write_block(self, sensor_number: int, block: int,
                            block_data: bytes) -> bytes:
        return self._mfrc522_block_command(
            sensor_number, MFRC522_COMMAND_WRITE_BLOCK, block,
            bytes(block_data))

    def scan(self) -> int:
        self.write(JYDBUS_FRAME_TYPE_SCAN, 0, 0, b"<<<<")
        return 0

    def write_sensor_value(self, sensor_type: int, sensor_number: int,
                           value: int) -> int:
        self._validate_number(sensor_number)
        self.write(JYDBUS_FRAME_TYPE_QUERY, sensor_type, sensor_number,
                   struct.pack("<I", value & 0xFFFFFFFF))
        return 0

    def set_ws2812b_pixel_color(self, sensor_number: int, led_index: int,
                                color: int) -> int:
        self._validate_number(sensor_number)
        self._validate_ws2812b_color(color)
        if not 0 <= led_index < WS2812B_LED_COUNT:
            raise OSError(errno.EINVAL, "WS2812B led index must be 0..127")
        payload = bytes((WS2812B_COMMAND_SET_PIXEL, led_index))
        payload += struct.pack("<I", color & 0x00FFFFFF)
        with self._ws2812b_lock:
            self._ws2812b_request(sensor_number, payload,
                                   WS2812B_COMMAND_SET_PIXEL,
                                   expected_argument0=led_index)
        return 0

    def display_ws2812b_frame(self, sensor_number: int, colors: Any) -> int:
        self._validate_number(sensor_number)
        color_values = list(colors)
        if len(color_values) != WS2812B_LED_COUNT:
            raise OSError(errno.EINVAL, "WS2812B frame must contain 128 colors")
        for color in color_values:
            self._validate_ws2812b_color(color)

        with self._ws2812b_lock:
            last_error: BaseException | None = None
            for _ in range(WS2812B_FRAME_RETRIES):
                transaction_id = (getattr(self, "_ws2812b_transaction_id", 0) + 1) & 0xFF
                self._ws2812b_transaction_id = transaction_id
                try:
                    self._display_ws2812b_frame_once(sensor_number,
                                                     color_values,
                                                     transaction_id)
                    return 0
                except (OSError, TimeoutError) as exc:
                    last_error = exc
            if last_error is not None:
                raise last_error
        return 0

    def _display_ws2812b_frame_once(self, sensor_number: int,
                                    colors: list[int],
                                    transaction_id: int) -> None:
        begin = bytes((WS2812B_COMMAND_FRAME_BEGIN, transaction_id))
        self._ws2812b_request(sensor_number, begin,
                              WS2812B_COMMAND_FRAME_BEGIN, transaction_id,
                              expected_argument0=transaction_id)

        # Stream two 64-LED chunks without per-chunk UART round trips. Every
        # chunk carries CRC16; COMMIT confirms all 128 colors were received.
        for start in range(0, WS2812B_LED_COUNT, WS2812B_FRAME_CHUNK_LEDS):
            chunk = colors[start:start + WS2812B_FRAME_CHUNK_LEDS]
            payload = bytes((WS2812B_COMMAND_FRAME_CHUNK, transaction_id,
                             start, len(chunk)))
            payload += b"".join(bytes(((color >> 16) & 0xFF,
                                       (color >> 8) & 0xFF,
                                       color & 0xFF)) for color in chunk)
            self._write_timed(JYDBUS_FRAME_TYPE_QUERY,
                              JYDBUS_TYPE_WS2812B,
                              sensor_number, payload, drain=True)
            time.sleep(WS2812B_FRAME_GAP)

        commit = bytes((WS2812B_COMMAND_FRAME_COMMIT, transaction_id))
        self._ws2812b_request(sensor_number, commit,
                              WS2812B_COMMAND_FRAME_COMMIT, transaction_id,
                              expected_argument0=transaction_id)

    @staticmethod
    def _validate_ws2812b_color(color: int) -> None:
        if not isinstance(color, int) or not 0 <= color <= 0x00FFFFFF:
            raise OSError(errno.EINVAL, "WS2812B color must be 0x000000..0xFFFFFF")

    def _ws2812b_request(self, sensor_number: int, payload: bytes,
                         command: int, transaction_id: int | None = None,
                         expected_argument0: int | None = None,
                         expected_argument1: int | None = None) -> dict[str, Any]:
        for _ in range(WS2812B_REQUEST_RETRIES):
            try:
                previous = self.read_cached(JYDBUS_TYPE_WS2812B,
                                            sensor_number).sequence
            except OSError:
                previous = 0
            started = self._write_timed(JYDBUS_FRAME_TYPE_QUERY,
                                        JYDBUS_TYPE_WS2812B,
                                        sensor_number, payload, drain=True)
            deadline = started / 1_000_000 + QUERY_RESPONSE_TIMEOUT + \
                sensor_number * QUERY_HOP_TIMEOUT
            while time.monotonic() < deadline:
                try:
                    data = self.read_cached(JYDBUS_TYPE_WS2812B,
                                            sensor_number)
                    if data.sequence != previous and data.received_monotonic_us >= started:
                        previous = data.sequence
                        value = data.value
                        if (value.get("command") == command and
                                (expected_argument0 is None or
                                 value.get("argument0") == expected_argument0) and
                                (expected_argument1 is None or
                                 value.get("argument1") == expected_argument1)):
                            status = value.get("status", WS2812B_STATUS_OK)
                            if status != WS2812B_STATUS_OK:
                                raise OSError(errno.EPROTO,
                                              f"WS2812B command 0x{command:02X} failed: "
                                              f"status 0x{status:02X}")
                            time.sleep(WS2812B_COMMAND_GAP)
                            return value
                except OSError as exc:
                    if exc.errno == errno.EPROTO:
                        raise
                time.sleep(0.001)
        raise TimeoutError(f"WS2812B command 0x{command:02X} response timeout")

    def configure_auto_upload(self, sensor_type: int, sensor_number: int,
                              enabled: bool, interval_ms: int) -> int:
        self._validate_number(sensor_number)
        if not 0 <= interval_ms <= 0xFFFF:
            raise OSError(errno.EINVAL, "auto-upload interval must be 0..65535 ms")
        payload = struct.pack("<BHB", int(enabled), interval_ms, 0)
        self.write(JYDBUS_FRAME_TYPE_CONFIG, sensor_type, sensor_number, payload)
        return 0

    def read_cached(self, sensor_type: int, sensor_number: int) -> JydbusData:
        self._validate_number(sensor_number)
        with self._cache_lock:
            data = self._cache.get((sensor_type, sensor_number))
            if data is None:
                raise OSError(errno.ENODATA, "no cached sensor data")
            return copy.deepcopy(data)

    def read_all_cached(self) -> list[JydbusData]:
        with self._cache_lock:
            return copy.deepcopy(list(self._cache.values()))

    def read_history(self, sensor_type: int, sensor_number: int,
                     after_sequence: int = 0) -> list[JydbusData]:
        """Return retained frames newer than ``after_sequence`` in RX order."""
        self._validate_number(sensor_number)
        with self._cache_lock:
            history = self._history.get((sensor_type, sensor_number), ())
            return copy.deepcopy(
                [data for data in history if data.sequence > after_sequence])

    def check_receiver(self) -> None:
        """Raise immediately when the background UART receiver has failed."""
        with self._cache_lock:
            error = self._rx_error
        if error is not None:
            code = getattr(error, "errno", None) or errno.EIO
            raise OSError(code, f"UART receive thread stopped: {error}") from error
        if self._running and not self._thread.is_alive():
            raise OSError(errno.EIO, "UART receive thread stopped unexpectedly")

    def get_stats(self) -> JydbusUartStats:
        with self._cache_lock:
            return copy.copy(self._stats)

    def execute_command(self, command: int, sensor_type: int = 0,
                        sensor_number: int = 0, value: int = 0) -> JydbusUartCommandResult:
        result = JydbusUartCommandResult(command=command, sensor_type=sensor_type,
                                         sensor_number=sensor_number, value=value)
        try:
            if command == JYDBUS_UART_COMMAND_SCAN:
                with self._cache_lock:
                    self._scan_cache.clear()
                self.scan()
                time.sleep(QUERY_RESPONSE_TIMEOUT)
                with self._cache_lock:
                    result.steps = [JydbusUartCommandStep(t, n, response_received=True)
                                    for t, n in self._scan_cache]
            elif command == JYDBUS_UART_COMMAND_QUERY_ALL:
                for target in COMMAND_TARGETS:
                    result.steps.append(self._query_and_wait(target, JYDBUS_NUMBER_FIRST,
                                                             all_query=True))
            elif command in (JYDBUS_UART_COMMAND_ENABLE_AUTO_UPLOAD,
                             JYDBUS_UART_COMMAND_DISABLE_AUTO_UPLOAD):
                enabled = command == JYDBUS_UART_COMMAND_ENABLE_AUTO_UPLOAD
                for target in COMMAND_TARGETS:
                    interval_ms = (PAJ7620_AUTO_UPLOAD_INTERVAL_MS
                                   if target == JYDBUS_TYPE_PAJ7620U2
                                   else AUTO_UPLOAD_INTERVAL_MS)
                    step = JydbusUartCommandStep(target, JYDBUS_UART_JYDBUS_NUMBER_MAX,
                                                 response_received=True)
                    for number in range(JYDBUS_UART_JYDBUS_NUMBER_MAX, 0, -1):
                        try:
                            self.configure_auto_upload(
                                target, number, enabled,
                                interval_ms if enabled else 0)
                        except OSError as exc:
                            if step.send_result == 0:
                                step.send_result = -int(exc.errno or errno.EIO)
                        time.sleep(CONFIG_FRAME_GAP)
                    result.steps.append(step)
                first_error = next((step.send_result for step in result.steps
                                    if step.send_result != 0), 0)
                result.status = first_error
            elif command == JYDBUS_UART_COMMAND_SET_WS2812B_RED:
                self.write_sensor_value(JYDBUS_TYPE_WS2812B,
                                        JYDBUS_NUMBER_FIRST, 0x00FF0000)
                result.steps.append(JydbusUartCommandStep(JYDBUS_TYPE_WS2812B,
                                                          JYDBUS_NUMBER_FIRST))
            elif command == JYDBUS_UART_COMMAND_READ_SENSOR:
                result.data = self.read_cached(sensor_type, sensor_number)
                result.data_valid = True
            elif command == JYDBUS_UART_COMMAND_WRITE_SENSOR:
                self.write_sensor_value(sensor_type, sensor_number, value)
            elif command == JYDBUS_UART_COMMAND_QUERY_SENSOR:
                step = self._query_and_wait(sensor_type, sensor_number)
                result.steps.append(step)
                if step.response_received:
                    result.data = self.read_cached(sensor_type, sensor_number)
                    result.data_valid = True
            else:
                raise OSError(errno.EINVAL, "invalid command")
        except OSError as exc:
            result.status = -int(exc.errno or errno.EIO)
        return result

    def _query_and_wait(self, sensor_type: int, sensor_number: int,
                        all_query: bool = False) -> JydbusUartCommandStep:
        step = JydbusUartCommandStep(sensor_type, sensor_number)
        try:
            previous = self.read_cached(sensor_type, sensor_number).sequence
        except OSError:
            previous = 0
        try:
            if all_query:
                started = self._write_timed(JYDBUS_FRAME_TYPE_QUERY, sensor_type,
                                            sensor_number, bytes(4))
            else:
                if sensor_type == JYDBUS_TYPE_WS2812B:
                    payload = bytearray((WS2812B_COMMAND_GET_STATUS,))
                else:
                    payload = bytearray(4)
                if sensor_type not in (JYDBUS_TYPE_WS2812B, JYDBUS_TYPE_ZW101):
                    payload[3] = 0x72
                started = self._write_timed(JYDBUS_FRAME_TYPE_QUERY, sensor_type,
                                            sensor_number, payload)
        except OSError as exc:
            step.send_result = -int(exc.errno or errno.EIO)
            return step
        deadline = started / 1_000_000 + QUERY_RESPONSE_TIMEOUT + sensor_number * QUERY_HOP_TIMEOUT
        while time.monotonic() < deadline:
            try:
                data = self.read_cached(sensor_type, sensor_number)
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
        if not JYDBUS_UART_JYDBUS_NUMBER_MIN <= sensor_number <= JYDBUS_UART_JYDBUS_NUMBER_MAX:
            raise OSError(errno.EINVAL, "sensor number must be 1..8")

    def _rx_main(self) -> None:
        while self._running:
            try:
                readable, _, exceptional = select.select([self.port.fd], [], [self.port.fd], 0.020)
                if exceptional:
                    raise OSError(errno.EIO, "UART reported an exceptional condition")
                self._discard_stale_frame(time.monotonic())
                if readable:
                    for value in self.port.read_available(256):
                        self._push_byte(value)
            except OSError as exc:
                if self._running and exc.errno in (errno.EINTR, errno.EAGAIN):
                    continue
                if self._running:
                    self._record_rx_error(exc)
                break
            except ValueError as exc:
                if self._running:
                    self._record_rx_error(exc)
                break

    def _record_rx_error(self, error: OSError | ValueError) -> None:
        with self._cache_lock:
            self._rx_error = error
            self._stats.rx_errors += 1
            self._stats.last_rx_error = str(error)

    def _discard_stale_frame(self, now: float) -> None:
        if self._frame and now - self._last_rx >= RX_FRAME_TIMEOUT:
            if self._recover_missing_tail(time.monotonic_ns() // 1000):
                self._reset_parser()
                return
            self._record_format_error("incomplete frame timeout",
                                      bytes(self._frame))
            self._reset_parser()

    def _recover_missing_tail(self, received_us: int) -> bool:
        if not self._expected or len(self._frame) != self._expected - 1:
            return False
        payload_length = struct.unpack_from("<H", self._frame, 1)[0]
        crc_index = payload_length + 6
        if len(self._frame) != crc_index + 2:
            return False
        received_crc = struct.unpack_from("<H", self._frame, crc_index)[0]
        if received_crc != crc16_modbus(self._frame[1:crc_index]):
            return False

        with self._cache_lock:
            self._stats.legacy_tails += 1
        self._process_frame(bytes(self._frame) + bytes((FRAME_TAIL,)), received_us)
        return True

    def _record_format_error(self, reason: str, frame: bytes) -> None:
        with self._cache_lock:
            self._stats.format_errors += 1
            self._stats.last_format_reason = reason
            self._stats.last_format_frame = frame

    def _reset_parser(self) -> None:
        self._frame.clear()
        self._expected = 0

    def _push_byte(self, value: int) -> None:
        self._last_rx = time.monotonic()
        if not self._frame:
            if value == FRAME_HEADER:
                self._frame.append(value)
            return
        if len(self._frame) >= JYDBUS_UART_MAX_PAYLOAD + FRAME_OVERHEAD:
            self._record_format_error("frame exceeds receive capacity",
                                      bytes(self._frame))
            self._reset_parser()
            if value == FRAME_HEADER:
                self._frame.append(value)
            return
        self._frame.append(value)
        if len(self._frame) == 3:
            payload_length = struct.unpack_from("<H", self._frame, 1)[0]
            if payload_length > JYDBUS_UART_MAX_PAYLOAD:
                self._record_format_error("payload length exceeds limit",
                                          bytes(self._frame))
                self._reset_parser()
                return
            self._expected = payload_length + FRAME_OVERHEAD
        if self._expected and len(self._frame) == self._expected:
            self._process_frame(bytes(self._frame), time.monotonic_ns() // 1000)
            self._reset_parser()

    def _process_frame(self, frame: bytes, received_us: int) -> None:
        if (len(frame) < FRAME_OVERHEAD or frame[0] != FRAME_HEADER):
            self._record_format_error("invalid header or short frame", frame)
            return
        payload_length = struct.unpack_from("<H", frame, 1)[0]
        if (payload_length > JYDBUS_UART_MAX_PAYLOAD or
                len(frame) != payload_length + FRAME_OVERHEAD):
            self._record_format_error("frame length mismatch", frame)
            return
        crc_index = payload_length + 6
        received_crc = struct.unpack_from("<H", frame, crc_index)[0]
        if received_crc != crc16_modbus(frame[1:crc_index]):
            with self._cache_lock:
                self._stats.crc_errors += 1
            return
        frame_type = frame[3]
        if frame_type == JYDBUS_FRAME_TYPE_QUERY:
            with self._cache_lock:
                self._stats.query_echoes += 1
            return
        if frame_type not in (JYDBUS_FRAME_TYPE_DATA, JYDBUS_FRAME_TYPE_SCAN):
            self._record_format_error("unsupported frame type", frame)
            return
        tail = frame[-1]
        legacy_tail = payload_length == 8 and tail == 0
        if tail != FRAME_TAIL and not legacy_tail:
            self._record_format_error("invalid frame tail", frame)
            return
        sensor_type, sensor_number = frame[4], frame[5]
        payload = frame[6:crc_index]
        with self._cache_lock:
            if legacy_tail:
                self._stats.legacy_tails += 1
            if frame_type == JYDBUS_FRAME_TYPE_SCAN:
                if len(payload) < 4 or payload[:4] != b"KKKK":
                    self._stats.format_errors += 1
                    self._stats.last_format_reason = "invalid scan payload"
                    self._stats.last_format_frame = frame
                    return
                self._stats.valid_frames += 1
                target = (sensor_type, sensor_number)
                if target not in self._scan_cache and len(self._scan_cache) < JYDBUS_UART_CACHE_CAPACITY:
                    self._scan_cache.append(target)
                return
            self._stats.valid_frames += 1
            if not JYDBUS_UART_JYDBUS_NUMBER_MIN <= sensor_number <= JYDBUS_UART_JYDBUS_NUMBER_MAX:
                return
            if (sensor_type, sensor_number) not in self._cache and len(self._cache) >= JYDBUS_UART_CACHE_CAPACITY:
                return
            valid, value = _decode(sensor_type, payload)
            self._sequence += 1
            target = (sensor_type, sensor_number)
            data = JydbusData(
                frame_type, sensor_type, sensor_number, tail, received_crc,
                payload, valid, value, self._sequence, received_us // 1000, received_us)
            self._cache[target] = data
            history = self._history.get(target)
            if history is None:
                history = deque(maxlen=JYDBUS_UART_HISTORY_CAPACITY)
                self._history[target] = history
            history.append(data)


def jydbus_name(sensor_type: int) -> str:
    return {
        JYDBUS_TYPE_AHT10: "AHT10", JYDBUS_TYPE_BMP390: "BMP390",
        JYDBUS_TYPE_MAX30102: "MAX30102", JYDBUS_TYPE_VL53L0X: "VL53L0X",
        JYDBUS_TYPE_MFRC522: "MFRC522", JYDBUS_TYPE_WS2812B: "WS2812B",
        JYDBUS_TYPE_ZW101: "ZW101", JYDBUS_TYPE_BUTTON_PB1: "BUTTON_PB1",
        JYDBUS_TYPE_JOYSTICK: "JOYSTICK",
        JYDBUS_TYPE_PHOTORESISTOR_ADC: "PHOTORESISTOR_ADC",
        JYDBUS_TYPE_WATER_LEVEL_ADC: "WATER_LEVEL_ADC",
        JYDBUS_TYPE_SOIL_MOISTURE_ADC: "SOIL_MOISTURE_ADC",
        JYDBUS_TYPE_ZSPD4003: "ZSPD4003",
        JYDBUS_TYPE_KNOB_SWITCH_ADC: "KNOB_SWITCH_ADC",
        JYDBUS_TYPE_PAJ7620U2: "PAJ7620U2",
        JYDBUS_TYPE_FAN: "FAN",
    }.get(sensor_type, "UNKNOWN")


def paj7620_gesture_name(gesture: int) -> str:
    return {
        JYDBUS_PAJ7620U2_GESTURE_NONE: "none",
        JYDBUS_PAJ7620U2_GESTURE_UP: "up",
        JYDBUS_PAJ7620U2_GESTURE_DOWN: "down",
        JYDBUS_PAJ7620U2_GESTURE_LEFT: "left",
        JYDBUS_PAJ7620U2_GESTURE_RIGHT: "right",
        JYDBUS_PAJ7620U2_GESTURE_FORWARD: "forward",
        JYDBUS_PAJ7620U2_GESTURE_BACKWARD: "backward",
        JYDBUS_PAJ7620U2_GESTURE_CLOCKWISE: "clockwise",
        JYDBUS_PAJ7620U2_GESTURE_COUNTERCLOCKWISE: "counterclockwise",
        JYDBUS_PAJ7620U2_GESTURE_WAVE: "wave",
    }.get(gesture, "unknown")


def paj7620_status_name(status: int) -> str:
    return {
        JYDBUS_PAJ7620U2_STATUS_OK: "ok",
        JYDBUS_PAJ7620U2_STATUS_INITIALIZING: "initializing",
        JYDBUS_PAJ7620U2_STATUS_I2C_ERROR: "i2c-error",
        JYDBUS_PAJ7620U2_STATUS_DEVICE_NOT_FOUND: "device-not-found",
        JYDBUS_PAJ7620U2_STATUS_CONFIGURATION_ERROR: "configuration-error",
        JYDBUS_PAJ7620U2_STATUS_SUSPENDED: "suspended",
    }.get(status, "unknown")


# Function-style compatibility helpers.
