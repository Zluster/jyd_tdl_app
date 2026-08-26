"""Jydbus device classes built on one shared JydbusApi connection."""

from __future__ import annotations

import errno
import threading
from typing import TYPE_CHECKING, ClassVar

from jydbus_uart import (PAJ7620_AUTO_UPLOAD_INTERVAL_MS, JYDBUS_TYPE_AHT10,
                         JYDBUS_TYPE_BMP390, JYDBUS_TYPE_BUTTON_PB1,
                         JYDBUS_TYPE_JOYSTICK, JYDBUS_TYPE_KNOB_SWITCH_ADC,
                         JYDBUS_TYPE_MAX30102, JYDBUS_TYPE_MFRC522,
                         JYDBUS_TYPE_PAJ7620U2, JYDBUS_TYPE_PHOTORESISTOR_ADC,
                         JYDBUS_TYPE_SOIL_MOISTURE_ADC, JYDBUS_TYPE_VL53L0X,
                         JYDBUS_TYPE_WATER_LEVEL_ADC, JYDBUS_TYPE_WS2812B,
                         JYDBUS_TYPE_ZSPD4003, JYDBUS_TYPE_ZW101,
                         JYDBUS_UART_COMMAND_QUERY_SENSOR, JydbusData,
                         jydbus_name)
from zw101_control import (ZW101_CONTROL_CLEAR_DATABASE, ZW101_CONTROL_DELETE,
                           ZW101_CONTROL_ENROLL, ZW101_CONTROL_MATCH,
                           ZW101_CONTROL_DEFAULT_TIMEOUT_MS, Zw101ControlResult,
                           zw101_control_execute)

if TYPE_CHECKING:
    from jydbus_api import JydbusApi


class JydbusDevice:
    """Base class for a device node sharing an existing JydbusApi."""

    SENSOR_TYPE: ClassVar[int]
    DEFAULT_UPLOAD_INTERVAL_MS: ClassVar[int] = 1000

    def __init__(self, api: JydbusApi, sensor_number: int = 1) -> None:
        if not 1 <= sensor_number <= 8:
            raise OSError(errno.EINVAL, "sensor number must be 1..8")
        self.api = api
        self.sensor_number = sensor_number

    @property
    def sensor_type(self) -> int:
        return self.SENSOR_TYPE

    @property
    def name(self) -> str:
        return jydbus_name(self.SENSOR_TYPE)

    def query(self) -> int:
        """Send a query without waiting for its response."""
        return self.api.query(self.SENSOR_TYPE, self.sensor_number)

    def query_data(self) -> JydbusData:
        """Send a query and wait for a new response."""
        result = self.api.command(JYDBUS_UART_COMMAND_QUERY_SENSOR,
                                  self.SENSOR_TYPE, self.sensor_number)
        if result.status != 0:
            code = -result.status if result.status < 0 else result.status
            raise OSError(code, f"{self.name} query failed")
        if not result.data_valid or result.data is None:
            raise TimeoutError(errno.ETIMEDOUT,
                               f"{self.name} node {self.sensor_number} timed out")
        return result.data

    def read(self) -> JydbusData:
        """Read the latest response already stored in the receive cache."""
        return self.api.read(self.SENSOR_TYPE, self.sensor_number)

    @staticmethod
    def _decoded_value(data: JydbusData) -> dict[str, object]:
        if not data.decoded_valid:
            raise OSError(errno.EPROTO, "sensor payload could not be decoded")
        return dict(data.value)

    def query_value(self) -> dict[str, object]:
        return self._decoded_value(self.query_data())

    def read_value(self) -> dict[str, object]:
        return self._decoded_value(self.read())

    def write(self, value: int) -> int:
        return self.api.write(self.SENSOR_TYPE, self.sensor_number, value)

    def set_auto_upload(self, enabled: bool,
                        interval_ms: int | None = None) -> int:
        if not enabled:
            interval = 0
        elif interval_ms is None:
            interval = self.DEFAULT_UPLOAD_INTERVAL_MS
        else:
            interval = interval_ms
        return self.api.set_auto_upload(self.SENSOR_TYPE, self.sensor_number,
                                        enabled, interval)


class JydbusAHT10(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_AHT10


class JydbusBMP390(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_BMP390


class JydbusMAX30102(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_MAX30102


class JydbusVL53L0X(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_VL53L0X


class JydbusMFRC522(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_MFRC522


class JydbusWS2812B(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_WS2812B

    def set_pixel(self, led_index: int, color: int) -> int:
        return self.api.set_ws2812b_pixel(led_index, color, self.sensor_number)

    def set_frame(self, colors: object) -> int:
        return self.api.set_ws2812b_frame(colors, self.sensor_number)


class JydbusZW101(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_ZW101

    def __init__(self, api: JydbusApi, sensor_number: int = 1) -> None:
        super().__init__(api, sensor_number)
        self.timeout_ms = ZW101_CONTROL_DEFAULT_TIMEOUT_MS
        self._operation_lock = threading.Lock()

    def set_timeout(self, timeout_ms: int) -> None:
        if timeout_ms <= 0:
            raise OSError(errno.EINVAL, "timeout must be positive")
        with self._operation_lock:
            self.timeout_ms = timeout_ms

    def _execute(self, command: int,
                 fingerprint_id: int = 0) -> Zw101ControlResult:
        with self._operation_lock:
            result = zw101_control_execute(self.api.uart, self.sensor_number,
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

    def enroll(self, fingerprint_id: int) -> Zw101ControlResult:
        return self._execute(ZW101_CONTROL_ENROLL, fingerprint_id)

    def match(self) -> Zw101ControlResult:
        return self._execute(ZW101_CONTROL_MATCH)

    def delete(self, fingerprint_id: int) -> Zw101ControlResult:
        return self._execute(ZW101_CONTROL_DELETE, fingerprint_id)

    def clear(self) -> Zw101ControlResult:
        return self._execute(ZW101_CONTROL_CLEAR_DATABASE)


class JydbusButtonPB1(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_BUTTON_PB1


class JydbusJoystick(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_JOYSTICK


class JydbusPhotoresistorADC(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_PHOTORESISTOR_ADC


class JydbusWaterLevelADC(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_WATER_LEVEL_ADC


class JydbusSoilMoistureADC(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_SOIL_MOISTURE_ADC


class JydbusZSPD4003(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_ZSPD4003


class JydbusKnobSwitchADC(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_KNOB_SWITCH_ADC


class JydbusPAJ7620U2(JydbusDevice):
    SENSOR_TYPE = JYDBUS_TYPE_PAJ7620U2
    DEFAULT_UPLOAD_INTERVAL_MS = PAJ7620_AUTO_UPLOAD_INTERVAL_MS


JYDBUS_CLASS_BY_TYPE: dict[int, type[JydbusDevice]] = {
    sensor_class.SENSOR_TYPE: sensor_class
    for sensor_class in (
        JydbusAHT10, JydbusBMP390, JydbusMAX30102, JydbusVL53L0X,
        JydbusMFRC522, JydbusWS2812B, JydbusZW101, JydbusButtonPB1,
        JydbusJoystick, JydbusPhotoresistorADC, JydbusWaterLevelADC,
        JydbusSoilMoistureADC, JydbusZSPD4003, JydbusKnobSwitchADC,
        JydbusPAJ7620U2,
    )
}


def create_jydbus(api: JydbusApi, sensor_type: int,
                  sensor_number: int = 1) -> JydbusDevice:
    """Create the Jydbus class registered for a protocol device type."""
    try:
        jydbus_class = JYDBUS_CLASS_BY_TYPE[sensor_type]
    except KeyError as exc:
        raise OSError(errno.EINVAL,
                      f"unsupported sensor type: 0x{sensor_type:02X}") from exc
    return jydbus_class(api, sensor_number)
