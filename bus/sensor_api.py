"""Compatibility imports; new code should use jydbus_api."""

from jydbus_api import (JydbusApi, jydbusApi, jydbus_api_close,
                        jydbus_api_open, jydbus_api_set_ws2812b_frame,
                        jydbus_api_set_ws2812b_pixel)

SensorApi = JydbusApi
sensor_api_open = jydbus_api_open
sensor_api_close = jydbus_api_close
sensor_api_set_ws2812b_pixel = jydbus_api_set_ws2812b_pixel
sensor_api_set_ws2812b_frame = jydbus_api_set_ws2812b_frame

__all__ = (
    "JydbusApi", "jydbusApi", "SensorApi", "jydbus_api_open",
    "jydbus_api_close", "jydbus_api_set_ws2812b_pixel",
    "jydbus_api_set_ws2812b_frame", "sensor_api_open", "sensor_api_close",
    "sensor_api_set_ws2812b_pixel", "sensor_api_set_ws2812b_frame",
)
