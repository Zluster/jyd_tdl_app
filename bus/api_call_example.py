#!/usr/bin/env python3
"""Examples for calling the existing Linux UART Python sensor API."""

from __future__ import annotations

import argparse
import errno
import pprint
import time
from typing import Callable

from sensor_api import SensorApi
from sensor_uart import (SENSOR_UART_COMMAND_QUERY_SENSOR,
                         SENSOR_UART_COMMAND_SCAN, SensorData, sensor_name)

DEFAULT_DEVICE = "/dev/ttyS2"
DEFAULT_BAUD_RATE = 115200


def initialize(device: str = DEFAULT_DEVICE,
               baud_rate: int = DEFAULT_BAUD_RATE) -> SensorApi:
    """初始化：打开串口，SensorApi 内部会同时启动接收线程。"""
    return SensorApi(device, baud_rate)


def scanning(api: SensorApi) -> list[tuple[int, int]]:
    """扫描总线，返回 [(传感器类型, 节点编号), ...]。"""
    result = api.command(SENSOR_UART_COMMAND_SCAN)
    if result.status != 0:
        raise OSError(-result.status, "scanning failed")
    return [(step.sensor_type, step.sensor_number) for step in result.steps]


def getScanData(api: SensorApi) -> list[dict[str, int | bool | str]]:
    """获取完整扫描数据，包含类型、名称、节点号和应答信息。"""
    result = api.command(SENSOR_UART_COMMAND_SCAN)
    if result.status != 0:
        raise OSError(-result.status, "getScanData failed")
    return [
        {
            "sensor_type": step.sensor_type,
            "sensor_name": sensor_name(step.sensor_type),
            "sensor_number": step.sensor_number,
            "response_received": step.response_received,
            "response_ms": step.response_ms,
            "send_result": step.send_result,
        }
        for step in result.steps
    ]


get_scan_data = getScanData


def setUploadMode(api: SensorApi, sensor_type: int, sensor_number: int,
                  automatic: bool, interval_ms: int = 1000) -> None:
    """设置上传模式：automatic=True 自动上传，False 手动查询。"""
    api.set_auto_upload(sensor_type, sensor_number, automatic,
                        interval_ms if automatic else 0)


def getValue(api: SensorApi, sensor_type: int, sensor_number: int):
    """主动查询一次传感器，等待应答并返回解码后的值。"""
    result = api.command(SENSOR_UART_COMMAND_QUERY_SENSOR,
                         sensor_type, sensor_number)
    if result.status != 0:
        raise OSError(-result.status, "getValue failed")
    if not result.data_valid or result.data is None:
        raise TimeoutError(errno.ETIMEDOUT, "sensor response timed out")
    return result.data.value if result.data.decoded_valid else result.data.raw


def setValue(api: SensorApi, sensor_type: int, sensor_number: int,
             value: int) -> None:
    """向传感器发送一个 32 位配置值。"""
    api.write(sensor_type, sensor_number, value)


def setWs2812bPixel(api: SensorApi, led_index: int, color: int,
                    sensor_number: int = 1) -> None:
    """设置单颗 WS2812B 灯珠，颜色格式为 0x00RRGGBB。"""
    api.set_ws2812b_pixel(led_index, color, sensor_number)


def setWs2812bFrame(api: SensorApi, colors: object,
                    sensor_number: int = 1) -> None:
    """一次提交 128 颗 WS2812B 的颜色。"""
    api.set_ws2812b_frame(colors, sensor_number)


def read(api: SensorApi, sensor_type: int,
         sensor_number: int) -> SensorData | None:
    """读取接收线程缓存的数据；没有数据时返回 None。"""
    try:
        return api.read(sensor_type, sensor_number)
    except OSError as exc:
        if exc.errno == errno.ENODATA:
            return None
        raise


def listenning(api: SensorApi, callback: Callable[[SensorData], None],
               duration_s: float | None = None,
               poll_interval_s: float = 0.02) -> None:
    """监听总线缓存，有新数据时调用 callback；Ctrl+C 可结束。"""
    sequences: dict[tuple[int, int], int] = {}
    deadline = None if duration_s is None else time.monotonic() + duration_s
    while deadline is None or time.monotonic() < deadline:
        for data in api.uart.read_all():
            key = (data.sensor_type, data.sensor_number)
            if sequences.get(key) != data.sequence:
                sequences[key] = data.sequence
                callback(data)
        time.sleep(poll_interval_s)


def printData(data: SensorData) -> None:
    value = data.value if data.decoded_valid else data.raw.hex(" ")
    print(f"{sensor_name(data.sensor_type)} "
          f"type=0x{data.sensor_type:02X} number={data.sensor_number} "
          f"sequence={data.sequence}")
    pprint.pprint(value, sort_dicts=True)


def integer(text: str) -> int:
    return int(text, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Sensor API call example")
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--baud", type=integer, default=DEFAULT_BAUD_RATE)
    parser.add_argument("--type", type=integer, default=0x03)
    parser.add_argument("--number", type=integer, default=1)
    parser.add_argument("--mode", choices=("auto", "manual"), default="auto")
    parser.add_argument("--interval", type=integer, default=1000)
    parser.add_argument("--listen-seconds", type=float, default=10.0)
    parser.add_argument("--set-value", type=integer)
    args = parser.parse_args()

    api = initialize(args.device, args.baud)
    try:
        print("scanning:")
        for sensor_type, sensor_number in scanning(api):
            print(f"  {sensor_name(sensor_type)} "
                  f"type=0x{sensor_type:02X} number={sensor_number}")

        automatic = args.mode == "auto"
        setUploadMode(api, args.type, args.number, automatic, args.interval)

        if args.set_value is not None:
            setValue(api, args.type, args.number, args.set_value)

        print("getValue:")
        pprint.pprint(getValue(api, args.type, args.number), sort_dicts=True)

        cached = read(api, args.type, args.number)
        if cached is not None:
            print("read:")
            printData(cached)

        print("listenning:")
        listenning(api, printData, args.listen_seconds)

        if automatic:
            setUploadMode(api, args.type, args.number, False)
        return 0
    finally:
        api.close()


if __name__ == "__main__":
    raise SystemExit(main())
