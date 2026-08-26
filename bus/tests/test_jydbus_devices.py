"""Hardware-independent tests for the Jydbus device classes."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

import jydbus_devices
import jydbus_uart
from jydbus_api import JydbusApi, jydbusApi


class FakeApi:
    def __init__(self) -> None:
        self.calls: list[tuple[object, ...]] = []
        self.data = jydbus_uart.JydbusData(
            sensor_type=jydbus_uart.JYDBUS_TYPE_BMP390,
            sensor_number=1, decoded_valid=True,
            value={"temperature_c": 23.5, "pressure_pa": 101325.0})

    def query(self, sensor_type: int, sensor_number: int) -> int:
        self.calls.append(("query", sensor_type, sensor_number))
        return 0

    def read(self, sensor_type: int, sensor_number: int) -> jydbus_uart.JydbusData:
        self.calls.append(("read", sensor_type, sensor_number))
        return self.data

    def write(self, sensor_type: int, sensor_number: int, value: int) -> int:
        self.calls.append(("write", sensor_type, sensor_number, value))
        return 0

    def command(self, command: int, sensor_type: int, sensor_number: int):
        self.calls.append(("command", command, sensor_type, sensor_number))
        return SimpleNamespace(status=0, data_valid=True, data=self.data)

    def set_auto_upload(self, sensor_type: int, sensor_number: int,
                        enabled: bool, interval_ms: int) -> int:
        self.calls.append(("auto", sensor_type, sensor_number,
                           enabled, interval_ms))
        return 0

    def set_ws2812b_pixel(self, led_index: int, color: int,
                          sensor_number: int) -> int:
        self.calls.append(("pixel", led_index, color, sensor_number))
        return 0

    def set_ws2812b_frame(self, colors: object, sensor_number: int) -> int:
        self.calls.append(("frame", colors, sensor_number))
        return 0


class JydbusDeviceTests(unittest.TestCase):
    def test_every_sensor_type_has_a_class(self) -> None:
        expected = {
            jydbus_uart.JYDBUS_TYPE_AHT10, jydbus_uart.JYDBUS_TYPE_BMP390,
            jydbus_uart.JYDBUS_TYPE_MAX30102, jydbus_uart.JYDBUS_TYPE_VL53L0X,
            jydbus_uart.JYDBUS_TYPE_MFRC522, jydbus_uart.JYDBUS_TYPE_WS2812B,
            jydbus_uart.JYDBUS_TYPE_ZW101, jydbus_uart.JYDBUS_TYPE_BUTTON_PB1,
            jydbus_uart.JYDBUS_TYPE_JOYSTICK,
            jydbus_uart.JYDBUS_TYPE_PHOTORESISTOR_ADC,
            jydbus_uart.JYDBUS_TYPE_WATER_LEVEL_ADC,
            jydbus_uart.JYDBUS_TYPE_SOIL_MOISTURE_ADC,
            jydbus_uart.JYDBUS_TYPE_ZSPD4003,
            jydbus_uart.JYDBUS_TYPE_KNOB_SWITCH_ADC,
            jydbus_uart.JYDBUS_TYPE_PAJ7620U2,
        }
        self.assertEqual(set(jydbus_devices.JYDBUS_CLASS_BY_TYPE), expected)

    def test_common_query_read_and_upload_methods(self) -> None:
        api = FakeApi()
        sensor = jydbus_devices.JydbusBMP390(api, 1)
        self.assertEqual(sensor.query_value()["pressure_pa"], 101325.0)
        self.assertEqual(sensor.read_value()["temperature_c"], 23.5)
        sensor.set_auto_upload(True)
        sensor.set_auto_upload(False, 500)
        self.assertIn(("auto", jydbus_uart.JYDBUS_TYPE_BMP390, 1,
                       True, 1000), api.calls)
        self.assertIn(("auto", jydbus_uart.JYDBUS_TYPE_BMP390, 1,
                       False, 0), api.calls)

    def test_factory_and_ws2812b_methods(self) -> None:
        api = FakeApi()
        sensor = jydbus_devices.create_jydbus(
            api, jydbus_uart.JYDBUS_TYPE_WS2812B, 2)
        self.assertIsInstance(sensor, jydbus_devices.JydbusWS2812B)
        sensor.set_pixel(7, 0x123456)
        colors = [0] * 128
        sensor.set_frame(colors)
        self.assertEqual(api.calls[0], ("pixel", 7, 0x123456, 2))
        self.assertEqual(api.calls[1], ("frame", colors, 2))

        real_api = object.__new__(JydbusApi)
        via_api = real_api.jydbus(jydbus_uart.JYDBUS_TYPE_WS2812B, 2)
        self.assertIsInstance(via_api, jydbus_devices.JydbusWS2812B)
        self.assertIs(via_api.api, real_api)
        self.assertIs(jydbusApi, JydbusApi)

    def test_paj7620_uses_event_upload_interval(self) -> None:
        api = FakeApi()
        jydbus_devices.JydbusPAJ7620U2(api).set_auto_upload(True)
        self.assertEqual(api.calls[-1],
                         ("auto", jydbus_uart.JYDBUS_TYPE_PAJ7620U2,
                          1, True, 100))

    def test_invalid_type_and_number_are_rejected(self) -> None:
        api = FakeApi()
        with self.assertRaises(OSError):
            jydbus_devices.create_jydbus(api, 0xFF)
        with self.assertRaises(OSError):
            jydbus_devices.JydbusAHT10(api, 0)


if __name__ == "__main__":
    unittest.main()
