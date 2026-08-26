"""Compatibility imports; new code should use jydbus_uart."""

from jydbus_uart import *  # noqa: F401,F403
from jydbus_uart import (JydbusData, JydbusUart, JydbusUartCommandResult,
                         JydbusUartCommandStep, JydbusUartStats, jydbus_name,
                         jydbusData, jydbusUart,
                         jydbus_query, jydbus_read, jydbus_uart_close,
                         jydbus_uart_open, jydbus_write)

# Preserve every former SENSOR_* constant for existing applications.
for _name, _value in tuple(globals().items()):
    if _name.startswith("JYDBUS_"):
        globals()["SENSOR_" + _name[len("JYDBUS_"):]] = _value

SensorData = JydbusData
SensorUart = JydbusUart
SensorUartStats = JydbusUartStats
SensorUartCommandStep = JydbusUartCommandStep
SensorUartCommandResult = JydbusUartCommandResult
sensor_name = jydbus_name
sensor_uart_open = jydbus_uart_open
sensor_uart_close = jydbus_uart_close
sensor_read = jydbus_read
sensor_query = jydbus_query
sensor_write = jydbus_write

del _name, _value
