"""Typed sensor devices sharing one Jydbus bus connection."""

from __future__ import annotations

import errno
import threading
from typing import TYPE_CHECKING, ClassVar

if __package__:
    from .jydbus_uart import (PAJ7620_AUTO_UPLOAD_INTERVAL_MS,
                              JYDBUS_TYPE_AHT10, JYDBUS_TYPE_BMP390,
                              JYDBUS_TYPE_BUTTON_PB1, JYDBUS_TYPE_FAN,
                              JYDBUS_TYPE_JOYSTICK,
                              JYDBUS_TYPE_KNOB_SWITCH_ADC,
                              JYDBUS_TYPE_MAX30102, JYDBUS_TYPE_MFRC522,
                              JYDBUS_TYPE_PAJ7620U2,
                              JYDBUS_TYPE_PHOTORESISTOR_ADC,
                              JYDBUS_TYPE_SOIL_MOISTURE_ADC,
                              JYDBUS_TYPE_VL53L0X, JYDBUS_TYPE_WATER_LEVEL_ADC,
                              JYDBUS_TYPE_WS2812B, JYDBUS_TYPE_ZSPD4003,
                              JYDBUS_TYPE_ZW101,
                              JYDBUS_UART_COMMAND_QUERY_SENSOR, JydbusData,
                              jydbus_name)
    from .zw101_control import (ZW101_CONTROL_CLEAR_DATABASE,
                                ZW101_CONTROL_DELETE, ZW101_CONTROL_ENROLL,
                                ZW101_CONTROL_MATCH,
                                ZW101_CONTROL_DEFAULT_TIMEOUT_MS, ZW101Result,
                                run_zw101_command)
else:
    from jydbus_uart import (PAJ7620_AUTO_UPLOAD_INTERVAL_MS,
                             JYDBUS_TYPE_AHT10, JYDBUS_TYPE_BMP390,
                             JYDBUS_TYPE_BUTTON_PB1, JYDBUS_TYPE_FAN,
                             JYDBUS_TYPE_JOYSTICK,
                             JYDBUS_TYPE_KNOB_SWITCH_ADC,
                             JYDBUS_TYPE_MAX30102, JYDBUS_TYPE_MFRC522,
                             JYDBUS_TYPE_PAJ7620U2,
                             JYDBUS_TYPE_PHOTORESISTOR_ADC,
                             JYDBUS_TYPE_SOIL_MOISTURE_ADC,
                             JYDBUS_TYPE_VL53L0X, JYDBUS_TYPE_WATER_LEVEL_ADC,
                             JYDBUS_TYPE_WS2812B, JYDBUS_TYPE_ZSPD4003,
                             JYDBUS_TYPE_ZW101,
                             JYDBUS_UART_COMMAND_QUERY_SENSOR, JydbusData,
                             jydbus_name)
    from zw101_control import (ZW101_CONTROL_CLEAR_DATABASE,
                               ZW101_CONTROL_DELETE, ZW101_CONTROL_ENROLL,
                               ZW101_CONTROL_MATCH,
                               ZW101_CONTROL_DEFAULT_TIMEOUT_MS, ZW101Result,
                               run_zw101_command)

if TYPE_CHECKING:
    if __package__:
        from .jydbus_bus import JydBus
    else:
        from jydbus_bus import JydBus


class SensorDevice:
    """Common operations for one addressed sensor node."""

    SENSOR_TYPE: ClassVar[int]
    DEFAULT_UPLOAD_INTERVAL_MS: ClassVar[int] = 1000

    def __init__(self, bus: JydBus, sensor_number: int = 1) -> None:
        if not 1 <= sensor_number <= 8:
            raise OSError(errno.EINVAL, "sensor number must be 1..8")
        self.bus = bus
        self.sensor_number = sensor_number

    @property
    def sensor_type(self) -> int:
        return self.SENSOR_TYPE

    @property
    def name(self) -> str:
        return jydbus_name(self.SENSOR_TYPE)

    def request_update(self) -> int:
        """Send a query without waiting for its response."""
        return self.bus.request_sensor(self.SENSOR_TYPE, self.sensor_number)

    def request_data(self) -> JydbusData:
        """Send a query and wait for a new response."""
        result = self.bus.run_command(JYDBUS_UART_COMMAND_QUERY_SENSOR,
                                      self.SENSOR_TYPE, self.sensor_number)
        if result.status != 0:
            code = -result.status if result.status < 0 else result.status
            raise OSError(code, f"{self.name} query failed")
        if not result.data_valid or result.data is None:
            raise TimeoutError(errno.ETIMEDOUT,
                               f"{self.name} node {self.sensor_number} timed out")
        return result.data

    def read_cached_data(self) -> JydbusData:
        return self.bus.read_cached(self.SENSOR_TYPE, self.sensor_number)

    @staticmethod
    def _decoded_value(data: JydbusData) -> dict[str, object]:
        if not data.decoded_valid:
            raise OSError(errno.EPROTO, "sensor payload could not be decoded")
        return dict(data.value)

    def request_value(self) -> dict[str, object]:
        return self._decoded_value(self.request_data())

    def read_cached_value(self) -> dict[str, object]:
        return self._decoded_value(self.read_cached_data())

    def write_value(self, value: int) -> int:
        return self.bus.write_sensor_value(self.SENSOR_TYPE,
                                           self.sensor_number, value)

    def enable_auto_upload(self, interval_ms: int | None = None) -> int:
        interval = (self.DEFAULT_UPLOAD_INTERVAL_MS
                    if interval_ms is None else interval_ms)
        return self.bus.configure_auto_upload(self.SENSOR_TYPE,
                                              self.sensor_number, True,
                                              interval)

    def disable_auto_upload(self) -> int:
        return self.bus.configure_auto_upload(self.SENSOR_TYPE,
                                              self.sensor_number, False, 0)


class AHT10Sensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_AHT10

    def measure_temperature_humidity(self) -> dict[str, object]:
        return self.request_value()


class BMP390Sensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_BMP390

    def measure_temperature_pressure(self) -> dict[str, object]:
        return self.request_value()


class MAX30102Sensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_MAX30102

    def measure_heart_rate_oxygen(self) -> dict[str, object]:
        return self.request_value()


class VL53L0XSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_VL53L0X

    def measure_distance(self) -> dict[str, object]:
        return self.request_value()


class MFRC522Reader(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_MFRC522

    def read_card(self) -> dict[str, object]:
        return self.request_value()


class WS2812BPanel(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_WS2812B

    def set_pixel_color(self, led_index: int, color: int) -> int:
        return self.bus.uart.set_ws2812b_pixel_color(self.sensor_number,
                                                    led_index, color)

    def display_frame(self, colors: object) -> int:
        return self.bus.uart.display_ws2812b_frame(self.sensor_number, colors)


class ZW101FingerprintSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_ZW101

    def __init__(self, bus: JydBus, sensor_number: int = 1) -> None:
        super().__init__(bus, sensor_number)
        self.timeout_ms = ZW101_CONTROL_DEFAULT_TIMEOUT_MS
        self._operation_lock = threading.Lock()

    def set_operation_timeout(self, timeout_ms: int) -> None:
        if timeout_ms <= 0:
            raise OSError(errno.EINVAL, "timeout must be positive")
        with self._operation_lock:
            self.timeout_ms = timeout_ms

    def _run_fingerprint_command(self, command: int,
                                 fingerprint_id: int = 0) -> ZW101Result:
        with self._operation_lock:
            result = run_zw101_command(self.bus.uart, self.sensor_number,
                                       command, fingerprint_id,
                                       self.timeout_ms)
        if result.status:
            status_errno = {1: errno.EBUSY, 2: errno.EINVAL,
                            3: errno.ETIMEDOUT, 4: errno.EPROTO,
                            5: errno.EOVERFLOW, 6: errno.EIO}
            names = ("ok", "busy", "invalid-param", "timeout",
                     "protocol-error", "uart-overflow", "module-error")
            message = (names[result.status] if result.status < len(names)
                       else "unknown")
            error = OSError(status_errno.get(result.status, errno.EIO), message)
            error.result = result  # type: ignore[attr-defined]
            raise error
        return result

    def enroll_fingerprint(self, fingerprint_id: int) -> ZW101Result:
        return self._run_fingerprint_command(ZW101_CONTROL_ENROLL,
                                             fingerprint_id)

    def match_fingerprint(self) -> ZW101Result:
        return self._run_fingerprint_command(ZW101_CONTROL_MATCH)

    def delete_fingerprint(self, fingerprint_id: int) -> ZW101Result:
        return self._run_fingerprint_command(ZW101_CONTROL_DELETE,
                                             fingerprint_id)

    def clear_fingerprints(self) -> ZW101Result:
        return self._run_fingerprint_command(ZW101_CONTROL_CLEAR_DATABASE)


class ButtonPB1Sensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_BUTTON_PB1

    def read_button(self) -> dict[str, object]:
        return self.request_value()


class JoystickSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_JOYSTICK

    def read_position(self) -> dict[str, object]:
        return self.request_value()


class PhotoresistorSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_PHOTORESISTOR_ADC

    def measure_light_level(self) -> dict[str, object]:
        return self.request_value()


class WaterLevelSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_WATER_LEVEL_ADC

    def measure_water_level(self) -> dict[str, object]:
        return self.request_value()


class SoilMoistureSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_SOIL_MOISTURE_ADC

    def measure_soil_moisture(self) -> dict[str, object]:
        return self.request_value()


class ZSPD4003Sensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_ZSPD4003

    def measure_heart_rate_oxygen(self) -> dict[str, object]:
        return self.request_value()


class KnobSwitchSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_KNOB_SWITCH_ADC

    def read_position(self) -> dict[str, object]:
        return self.request_value()


class PAJ7620U2GestureSensor(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_PAJ7620U2
    DEFAULT_UPLOAD_INTERVAL_MS = PAJ7620_AUTO_UPLOAD_INTERVAL_MS

    def read_gesture(self) -> dict[str, object]:
        return self.request_value()


class FanActuator(SensorDevice):
    SENSOR_TYPE = JYDBUS_TYPE_FAN

    def set_speed(self, duty_percent: int) -> int:
        if (not isinstance(duty_percent, int) or
                isinstance(duty_percent, bool) or
                not 0 <= duty_percent <= 100):
            raise OSError(errno.EINVAL, "fan duty cycle must be 0..100 percent")
        return self.write_value(duty_percent)

    def set_enabled(self, enabled: bool) -> int:
        if not isinstance(enabled, bool):
            raise OSError(errno.EINVAL, "fan enabled state must be bool")
        return self.set_speed(100 if enabled else 0)

    def turn_on(self) -> int:
        return self.set_enabled(True)

    def turn_off(self) -> int:
        return self.set_enabled(False)

    def read_state(self) -> dict[str, object]:
        return self.request_value()


DEVICE_CLASS_BY_TYPE: dict[int, type[SensorDevice]] = {
    device_class.SENSOR_TYPE: device_class
    for device_class in (
        AHT10Sensor, BMP390Sensor, MAX30102Sensor, VL53L0XSensor,
        MFRC522Reader, WS2812BPanel, ZW101FingerprintSensor,
        ButtonPB1Sensor, JoystickSensor, PhotoresistorSensor,
        WaterLevelSensor, SoilMoistureSensor, ZSPD4003Sensor,
        KnobSwitchSensor, PAJ7620U2GestureSensor, FanActuator,
    )
}


def create_device(bus: JydBus, sensor_type: int,
                  sensor_number: int = 1) -> SensorDevice:
    try:
        device_class = DEVICE_CLASS_BY_TYPE[sensor_type]
    except KeyError as exc:
        raise OSError(errno.EINVAL,
                      f"unsupported sensor type: 0x{sensor_type:02X}") from exc
    return device_class(bus, sensor_number)
