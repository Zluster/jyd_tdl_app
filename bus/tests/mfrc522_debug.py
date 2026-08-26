#!/usr/bin/env python3
"""Linux 端 MFRC522 串口调试工具。"""

from __future__ import annotations

import argparse
import errno
import sys
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from jydbus_api import jydbusApi
from jydbus_uart import (JYDBUS_TYPE_MFRC522,
                         JYDBUS_UART_COMMAND_QUERY_SENSOR,
                         JYDBUS_UART_COMMAND_SCAN,
                         JydbusData)

DEFAULT_DEVICE = "/dev/ttyS2"
DEFAULT_BAUD_RATE = 115200


def integer(text: str) -> int:
    """解析十进制或 0x 开头的整数。"""
    return int(text, 0)


def format_bytes(value: object) -> str:
    """把字节数据格式化为十六进制字符串。"""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex(" ").upper()
    return "--"


def print_card(data: JydbusData) -> None:
    """打印一帧 MFRC522 数据。"""
    if not data.decoded_valid:
        print(f"节点 {data.sensor_number}: 数据无法解析，raw={format_bytes(data.raw)}")
        return

    uid = format_bytes(data.value.get("uid"))
    tag_type = format_bytes(data.value.get("tag_type"))
    present = bool(data.value.get("present"))
    status = int(data.value.get("status", int(present)))
    version = int(data.value.get("version", 0))

    if status == 3 or version in (0x00, 0xFF):
        print(f"节点 {data.sensor_number}: RC522 I2C通信失败, "
              f"VersionReg=0x{version:02X}")
        print("请检查模块I2C模式焊盘、地址、PB6/PB7接线和3.3V供电")
        return

    if status == 2:
        print(f"节点 {data.sensor_number}: 检测到卡，但UID校验失败, "
              f"卡类型={tag_type}, RC522版本=0x{version:02X}")
        return

    state = "检测到卡" if present else "无卡"
    print(f"节点 {data.sensor_number}: {state}, UID={uid}, "
          f"卡类型={tag_type}, RC522版本=0x{version:02X}")


def scan_mfrc522(api: jydbusApi) -> list[int]:
    """扫描总线并返回全部 MFRC522 节点号。"""
    result = api.command(JYDBUS_UART_COMMAND_SCAN)
    if result.status != 0:
        raise OSError(-result.status, "扫描命令发送失败")

    nodes = sorted({step.sensor_number for step in result.steps
                    if step.sensor_type == JYDBUS_TYPE_MFRC522})
    if nodes:
        print("发现 MFRC522 节点: " + ", ".join(map(str, nodes)))
    else:
        print("未发现 MFRC522 节点")
    return nodes


def query_once(api: jydbusApi, sensor_number: int) -> JydbusData:
    """主动查询指定节点并返回最新数据。"""
    result = api.command(JYDBUS_UART_COMMAND_QUERY_SENSOR,
                         JYDBUS_TYPE_MFRC522, sensor_number)
    if result.status != 0:
        raise OSError(-result.status, "查询命令发送失败")
    if not result.data_valid or result.data is None:
        raise TimeoutError(errno.ETIMEDOUT,
                           f"MFRC522 节点 {sensor_number} 无响应")
    print_card(result.data)
    return result.data


def listen(api: jydbusApi, sensor_number: int, interval_ms: int) -> None:
    """开启自动上报并持续打印指定节点的新数据。"""
    api.set_auto_upload(JYDBUS_TYPE_MFRC522, sensor_number, True, interval_ms)
    print(f"监听 MFRC522 节点 {sensor_number}，按 Ctrl+C 结束")

    try:
        last_sequence = query_once(api, sensor_number).sequence
        while True:
            try:
                data = api.read(JYDBUS_TYPE_MFRC522, sensor_number)
            except OSError as exc:
                if exc.errno != errno.ENODATA:
                    raise
            else:
                if data.sequence != last_sequence:
                    last_sequence = data.sequence
                    print_card(data)
            time.sleep(0.02)
    finally:
        api.set_auto_upload(JYDBUS_TYPE_MFRC522, sensor_number, False, 0)


def parse_args() -> argparse.Namespace:
    """解析命令行参数。"""
    parser = argparse.ArgumentParser(description="MFRC522 串口调试工具")
    parser.add_argument("--device", default=DEFAULT_DEVICE,
                        help="串口设备，默认 /dev/ttyS2")
    parser.add_argument("--baud", type=integer, default=DEFAULT_BAUD_RATE,
                        help="波特率，默认 115200")
    parser.add_argument("--number", type=integer, choices=range(1, 9), default=1,
                        metavar="1..8", help="MFRC522 节点号，默认 1")
    parser.add_argument("--interval", type=integer, default=200,
                        help="连续监听的上报周期，单位 ms，默认 200")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--scan", action="store_true", help="扫描 MFRC522 节点")
    mode.add_argument("--once", action="store_true", help="主动查询一次（默认）")
    mode.add_argument("--listen", action="store_true", help="连续监听刷卡数据")
    args = parser.parse_args()
    if not 1 <= args.interval <= 65535:
        parser.error("--interval 必须在 1..65535 之间")
    return args


def main() -> int:
    """打开串口并执行所选调试功能。"""
    args = parse_args()
    try:
        with jydbusApi(args.device, args.baud) as api:
            if args.scan:
                scan_mfrc522(api)
            elif args.listen:
                listen(api, args.number, args.interval)
            else:
                query_once(api, args.number)
        return 0
    except KeyboardInterrupt:
        print("\n监听已停止")
        return 0
    except (OSError, TimeoutError) as exc:
        print(f"错误: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
