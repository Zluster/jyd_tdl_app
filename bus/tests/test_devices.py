"""Hardware-independent tests for typed sensor devices."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

import devices
import jydbus_uart
from jydbus_bus import JydBus


class FakeBus:
    def __init__(self) -> None:
        self.calls: list[tuple[object, ...]] = []
        self.uart = SimpleNamespace(
            set_ws2812b_pixel_color=self._set_pixel,
            display_ws2812b_frame=self._display_frame)
        self.data = jydbus_uart.JydbusData(
            sensor_type=jydbus_uart.JYDBUS_TYPE_BMP390,
            sensor_number=1, decoded_valid=True,
            value={"temperature_c": 23.5, "pressure_pa": 101325.0})

    def _set_pixel(self, sensor_number: int, led_index: int, color: int) -> int:
        self.calls.append(("pixel", sensor_number, led_index, color))
        return 0

    def _display_frame(self, sensor_number: int, colors: object) -> int:
        self.calls.append(("frame", sensor_number, colors))
        return 0

    def request_sensor(self, sensor_type: int, sensor_number: int) -> int:
        self.calls.append(("request", sensor_type, sensor_number))
        return 0

    def read_cached(self, sensor_type: int,
                    sensor_number: int) -> jydbus_uart.JydbusData:
        self.calls.append(("cached", sensor_type, sensor_number))
        return self.data

    def write_sensor_value(self, sensor_type: int, sensor_number: int,
                           value: int) -> int:
        self.calls.append(("write", sensor_type, sensor_number, value))
        return 0

    def run_command(self, command: int, sensor_type: int, sensor_number: int):
        self.calls.append(("command", command, sensor_type, sensor_number))
        return SimpleNamespace(status=0, data_valid=True, data=self.data)

    def configure_auto_upload(self, sensor_type: int, sensor_number: int,
                              enabled: bool, interval_ms: int) -> int:
        self.calls.append(("auto", sensor_type, sensor_number,
                           enabled, interval_ms))
        return 0


class DeviceTests(unittest.TestCase):
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
        self.assertEqual(set(devices.DEVICE_CLASS_BY_TYPE), expected)

    def test_sensor_specific_measurement_and_upload_names(self) -> None:
        bus = FakeBus()
        bmp390 = devices.BMP390Sensor(bus, 1)
        self.assertEqual(bmp390.measure_temperature_pressure()["pressure_pa"],
                         101325.0)
        self.assertEqual(bmp390.read_cached_value()["temperature_c"], 23.5)
        bmp390.enable_auto_upload()
        bmp390.disable_auto_upload()
        self.assertIn(("auto", jydbus_uart.JYDBUS_TYPE_BMP390, 1,
                       True, 1000), bus.calls)
        self.assertIn(("auto", jydbus_uart.JYDBUS_TYPE_BMP390, 1,
                       False, 0), bus.calls)

    def test_factory_and_ws2812b_names(self) -> None:
        bus = FakeBus()
        panel = devices.create_device(bus, jydbus_uart.JYDBUS_TYPE_WS2812B, 2)
        self.assertIsInstance(panel, devices.WS2812BPanel)
        panel.set_pixel_color(7, 0x123456)
        colors = [0] * 128
        panel.display_frame(colors)
        self.assertEqual(bus.calls[0], ("pixel", 2, 7, 0x123456))
        self.assertEqual(bus.calls[1], ("frame", 2, colors))

        real_bus = object.__new__(JydBus)
        via_bus = real_bus.create_device(jydbus_uart.JYDBUS_TYPE_WS2812B, 2)
        self.assertIsInstance(via_bus, devices.WS2812BPanel)
        self.assertIs(via_bus.bus, real_bus)

    def test_gesture_sensor_uses_event_upload_interval(self) -> None:
        bus = FakeBus()
        devices.PAJ7620U2GestureSensor(bus).enable_auto_upload()
        self.assertEqual(bus.calls[-1],
                         ("auto", jydbus_uart.JYDBUS_TYPE_PAJ7620U2,
                          1, True, 100))

    def test_invalid_type_and_number_are_rejected(self) -> None:
        bus = FakeBus()
        with self.assertRaises(OSError):
            devices.create_device(bus, 0xFF)
        with self.assertRaises(OSError):
            devices.AHT10Sensor(bus, 0)


if __name__ == "__main__":
    unittest.main()
