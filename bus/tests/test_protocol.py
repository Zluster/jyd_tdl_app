"""Hardware-independent protocol regression tests."""

from __future__ import annotations

import copy
import struct
import sys
import threading
import unittest
from pathlib import Path

# Support both direct execution and unittest discovery from the project root.
PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

import sensor_ota
import sensor_uart


def receiver() -> sensor_uart.SensorUart:
    uart = object.__new__(sensor_uart.SensorUart)
    uart._cache_lock = threading.Lock()
    uart._cache = {}
    uart._scan_cache = []
    uart._stats = sensor_uart.SensorUartStats()
    uart._sequence = 0
    return uart


def response(sensor_type: int, sensor_number: int, payload: bytes,
             frame_type: int = sensor_uart.SENSOR_FRAME_TYPE_DATA) -> bytes:
    return sensor_uart.SensorUart.build_frame(frame_type, sensor_type,
                                              sensor_number, payload)


class SensorProtocolTests(unittest.TestCase):
    def test_crc_and_frame(self) -> None:
        self.assertEqual(sensor_uart.crc16_modbus(b"123456789"), 0x4B37)
        frame = response(sensor_uart.SENSOR_TYPE_BUTTON_PB1, 1, b"\x00")
        self.assertEqual(frame[0], 0x55)
        self.assertEqual(frame[-1], 0xAA)
        self.assertEqual(len(frame), struct.unpack_from("<H", frame, 1)[0] + 9)

    def test_float_and_adc_decode(self) -> None:
        uart = receiver()
        frame = response(sensor_uart.SENSOR_TYPE_AHT10, 1,
                         struct.pack("<ff", 23.5, 61.25))
        uart._process_frame(frame, 1_234_567)
        data = copy.deepcopy(uart._cache[(sensor_uart.SENSOR_TYPE_AHT10, 1)])
        self.assertTrue(data.decoded_valid)
        self.assertAlmostEqual(data.value["temperature_c"], 23.5)
        self.assertAlmostEqual(data.value["humidity_percent"], 61.25)
        frame = response(sensor_uart.SENSOR_TYPE_PHOTORESISTOR_ADC, 1,
                         b"\x00\x00\x34\x12")
        uart._process_frame(frame, 1_234_568)
        self.assertEqual(uart._cache[(sensor_uart.SENSOR_TYPE_PHOTORESISTOR_ADC, 1)].value["adc"],
                         0x1234)

    def test_zw101_result_and_scan(self) -> None:
        uart = receiver()
        payload = struct.pack("<BBBHHB", 2, 0, 0, 7, 85, 0xA5)
        uart._process_frame(response(sensor_uart.SENSOR_TYPE_ZW101, 1, payload), 2000)
        value = uart._cache[(sensor_uart.SENSOR_TYPE_ZW101, 1)].value
        self.assertEqual((value["fingerprint_id"], value["score"], value["result_marker"]),
                         (7, 85, 0xA5))
        uart._process_frame(response(0x0A, 2, b"KKKK", sensor_uart.SENSOR_FRAME_TYPE_SCAN), 3000)
        self.assertEqual(uart._scan_cache, [(0x0A, 2)])

    def test_bad_crc_is_counted(self) -> None:
        uart = receiver()
        frame = bytearray(response(sensor_uart.SENSOR_TYPE_BUTTON_PB1, 1, b"\x01"))
        frame[-2] ^= 1
        uart._process_frame(bytes(frame), 1000)
        self.assertEqual(uart._stats.crc_errors, 1)
        self.assertFalse(uart._cache)

    def test_paj7620u2_decode_and_event_upload_default(self) -> None:
        uart = receiver()
        payload = bytes((sensor_uart.SENSOR_PAJ7620U2_GESTURE_LEFT,
                         0x04, 0x00, sensor_uart.SENSOR_PAJ7620U2_STATUS_OK))
        uart._process_frame(response(sensor_uart.SENSOR_TYPE_PAJ7620U2, 1,
                                     payload), 1_234_569)
        data = uart._cache[(sensor_uart.SENSOR_TYPE_PAJ7620U2, 1)]
        self.assertTrue(data.decoded_valid)
        self.assertEqual(data.value["gesture"],
                         sensor_uart.SENSOR_PAJ7620U2_GESTURE_LEFT)
        self.assertEqual(data.value["gesture_flags"], 0x04)
        self.assertEqual(sensor_uart.PAJ7620_AUTO_UPLOAD_INTERVAL_MS, 100)

    def test_ws2812b_frame_is_split_into_ordered_chunks(self) -> None:
        uart = object.__new__(sensor_uart.SensorUart)
        uart._ws2812b_lock = threading.Lock()
        uart._ws2812b_transaction_id = 0
        requests: list[tuple[bytes, int, int | None]] = []

        def request(sensor_number: int, payload: bytes, command: int,
                    transaction_id: int | None = None,
                    expected_argument0: int | None = None,
                    expected_argument1: int | None = None) -> dict[str, int]:
            requests.append((payload, command, transaction_id))
            return {"command": command, "status": 0,
                    "argument0": transaction_id or 0}

        uart._ws2812b_request = request
        colors = [index * 0x010203 for index in range(sensor_uart.WS2812B_LED_COUNT)]
        self.assertEqual(uart.set_ws2812b_frame(1, colors), 0)

        self.assertEqual(len(requests), 18)
        self.assertEqual(requests[0][0], bytes((0x02, 0x01)))
        for chunk_number in range(16):
            payload, command, transaction_id = requests[chunk_number + 1]
            self.assertEqual(command, sensor_uart.WS2812B_COMMAND_FRAME_CHUNK)
            self.assertEqual(transaction_id, 1)
            self.assertEqual(payload[:4], bytes((0x03, 0x01, chunk_number * 8, 8)))
            expected = b"".join(bytes(((color >> 16) & 0xFF,
                                         (color >> 8) & 0xFF,
                                         color & 0xFF))
                               for color in colors[chunk_number * 8:chunk_number * 8 + 8])
            self.assertEqual(payload[4:], expected)
        self.assertEqual(requests[-1][0], bytes((0x04, 0x01)))

    def test_ws2812b_frame_rejects_invalid_input(self) -> None:
        uart = object.__new__(sensor_uart.SensorUart)
        uart._ws2812b_lock = threading.Lock()
        with self.assertRaises(OSError):
            uart.set_ws2812b_frame(1, [0] * 127)
        with self.assertRaises(OSError):
            uart.set_ws2812b_pixel(1, 128, 0)
        with self.assertRaises(OSError):
            uart.set_ws2812b_pixel(1, 0, 0x1000000)


class OtaProtocolTests(unittest.TestCase):
    def test_selftest(self) -> None:
        self.assertTrue(sensor_ota.sensor_ota_selftest())
        frame = sensor_ota.build_frame(sensor_ota.OTA_FRAME_REQUEST, 0x0A, 1,
                                       bytes(256))
        self.assertEqual((frame[1], frame[2]), (0, 1))
        self.assertEqual(len(frame), 265)

    def test_bundled_images_match_slots(self) -> None:
        sensor_ota.validate_image((PROJECT_DIR / "Project_OTA_Slot_A.bin").read_bytes(),
                                  sensor_ota.SENSOR_OTA_SLOT_A)
        sensor_ota.validate_image((PROJECT_DIR / "Project_OTA_Slot_B.bin").read_bytes(),
                                  sensor_ota.SENSOR_OTA_SLOT_B)

    def test_raw_crc_scope(self) -> None:
        header = bytearray(struct.pack("<IBBHIII", sensor_ota.RAW_MAGIC,
                                       sensor_ota.RAW_CMD_DATA, 1, 3,
                                       0x100, 0, 0))
        payload = b"abc"
        expected = sensor_ota.crc32(bytes(header[4:16]) + payload)
        self.assertEqual(sensor_ota.SensorOta._raw_crc(header, payload), expected)

    def test_status_transaction(self) -> None:
        class FakePort:
            def __init__(self) -> None:
                self.written = b""

            def write_all(self, data: bytes, timeout: float = 0.5,
                          drain: bool = False) -> None:
                self.written = data

        ota = object.__new__(sensor_ota.SensorOta)
        ota.port = FakePort()
        ota.debug = False
        reply = bytes((sensor_ota.OTA_CMD_STATUS, 0, 1, 1, 0xFF, 0xFF))
        reply += struct.pack("<IIIII", 128, 4096, 0x12345678, 7,
                             sensor_ota.OTA_SLOT_SIZE)
        reply += b"\x03"
        ota._read_frame = lambda timeout: sensor_ota.build_frame(
            sensor_ota.OTA_FRAME_RESPONSE, 0x09, 1, reply)
        status = ota.get_status(0x09, 1)
        self.assertEqual((status.active_slot, status.next_offset,
                          status.image_version, status.max_boot_attempts),
                         (1, 128, 7, 3))
        request = ota.port.written
        self.assertEqual((request[3], request[4], request[5], request[6]),
                         (sensor_ota.OTA_FRAME_REQUEST, 0x09, 1,
                          sensor_ota.OTA_CMD_STATUS))


if __name__ == "__main__":
    unittest.main()
