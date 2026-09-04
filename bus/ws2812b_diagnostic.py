#!/usr/bin/env python3
"""Diagnose WS2812B command responses over the Jydbus UART."""

from __future__ import annotations

import argparse

if __package__:
    from .devices import WS2812BPanel
    from .jydbus_bus import JydBus
else:
    from devices import WS2812BPanel
    from jydbus_bus import JydBus


def parse_integer(text: str) -> int:
    return int(text, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Set one WS2812B pixel and print UART diagnostics."
    )
    parser.add_argument("--device", default="/dev/ttyS2")
    parser.add_argument("--baud", type=parse_integer, default=115200)
    parser.add_argument("--sensor-number", type=parse_integer, default=1)
    parser.add_argument("--led-index", type=parse_integer, default=0)
    parser.add_argument("--color", type=parse_integer, default=0xFF0000)
    return parser


def print_diagnostics(bus: JydBus) -> None:
    stats = bus.uart.get_stats()
    print("rx_thread_alive:", bus.uart._thread.is_alive())
    print("stats:", stats)
    if stats.last_format_reason:
        print("last_format_reason:", stats.last_format_reason)
        print("last_format_frame:", stats.last_format_frame.hex(" "))

    cached = bus.read_all_cached()
    if not cached:
        print("cached_frames: none")
        return

    print("cached_frames:")
    for data in cached:
        print(
            f"  type=0x{data.sensor_type:02X}",
            f"number={data.sensor_number}",
            f"raw={data.raw.hex(' ')}",
            f"value={data.value}",
        )


def main() -> int:
    args = build_parser().parse_args()

    with JydBus(args.device, args.baud) as bus:
        panel = WS2812BPanel(bus, args.sensor_number)
        try:
            panel.set_pixel_color(args.led_index, args.color)
        except Exception as exc:
            print(f"WS2812B command failed: {type(exc).__name__}: {exc}")
            print_diagnostics(bus)
            return 1

        print(
            "WS2812B command succeeded:",
            f"sensor={args.sensor_number}",
            f"led={args.led_index}",
            f"color=0x{args.color:06X}",
        )
        print_diagnostics(bus)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
