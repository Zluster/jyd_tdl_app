#!/usr/bin/env python3
"""Command-line equivalent of sensor_ota_tool."""

from __future__ import annotations

import argparse
import errno
import sys

from sensor_ota import (SENSOR_OTA_SLOT_A, SENSOR_OTA_SLOT_B, OtaDeviceError,
                        SensorOta, SensorOtaStatus, ota_protocol_selftest)


def number(text: str) -> int:
    return int(text, 0)


def slot(text: str) -> int:
    value = text.upper()
    if value not in ("A", "B"):
        raise argparse.ArgumentTypeError("slot must be A or B")
    return SENSOR_OTA_SLOT_A if value == "A" else SENSOR_OTA_SLOT_B


def slot_name(value: int) -> str:
    return "A" if value == SENSOR_OTA_SLOT_A else "B" if value == SENSOR_OTA_SLOT_B else "none"


def print_status(status: SensorOtaStatus) -> None:
    print(f"active_slot={slot_name(status.active_slot)} "
          f"confirmed_slot={slot_name(status.confirmed_slot)} "
          f"pending_slot={slot_name(status.pending_slot)}")
    print(f"download_slot={slot_name(status.download_slot)} "
          f"next_offset={status.next_offset} slot_size={status.slot_size} "
          f"max_boot_attempts={status.max_boot_attempts}")
    print(f"active_version={status.active_version} "
          f"confirmed_version={status.confirmed_version} "
          f"download_version={status.image_version}")


def progress(sent: int, total: int) -> None:
    percent = 0 if total == 0 else sent * 100 // total
    print(f"\r[OTA] {sent}/{total} bytes ({percent}%)", end="\n" if sent == total else "", flush=True)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="GD32 cascaded UART OTA tool")
    sub = root.add_subparsers(dest="command", required=True)
    sub.add_parser("selftest")

    def serial_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--device", default="/dev/ttyS2")
        command.add_argument("--baud", type=number, default=115200)
        command.add_argument("--debug", action="store_true")

    def sensor_options(command: argparse.ArgumentParser) -> None:
        serial_options(command)
        command.add_argument("--type", type=number, required=True, dest="sensor_type")
        command.add_argument("--number", type=number, required=True)

    status = sub.add_parser("status")
    sensor_options(status)

    upgrade = sub.add_parser("upgrade")
    sensor_options(upgrade)
    upgrade.add_argument("--slot-a", required=True)
    upgrade.add_argument("--slot-b", required=True)
    upgrade.add_argument("--version", type=number, default=1)
    upgrade.add_argument("--stop-after", type=number, default=0)

    one = sub.add_parser("upgrade-one")
    sensor_options(one)
    one.add_argument("--slot", type=slot, required=True)
    one.add_argument("--image", required=True)
    one.add_argument("--version", type=number, default=1)
    one.add_argument("--stop-after", type=number, default=0)

    recovery = sub.add_parser("recovery")
    serial_options(recovery)
    recovery.add_argument("--slot", type=slot, required=True)
    recovery.add_argument("--image", required=True)
    recovery.add_argument("--version", type=number, default=1)
    return root


def main() -> int:
    args = parser().parse_args()
    if args.command == "selftest":
        passed = ota_protocol_selftest()
        print(f"OTA protocol selftest: {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1
    if hasattr(args, "sensor_type") and not 0 <= args.sensor_type <= 0xFF:
        parser().error("--type must be 0..255")
    if hasattr(args, "number") and not 1 <= args.number <= 0xFF:
        parser().error("--number must be 1..255")
    try:
        with SensorOta(args.device, args.baud, args.debug) as ota:
            sensor_type = getattr(args, "sensor_type", 0)
            sensor_number = getattr(args, "number", 0)
            print(f"[OTA] device={args.device} baud={args.baud} "
                  f"sensor=0x{sensor_type:02X} number={sensor_number}")
            if args.command == "status":
                print_status(ota.get_status(sensor_type, sensor_number))
                return 0
            if args.command == "upgrade":
                ota.upgrade_ab(sensor_type, sensor_number, args.slot_a, args.slot_b,
                               args.version, args.stop_after, progress)
            elif args.command == "upgrade-one":
                ota.upgrade_file(sensor_type, sensor_number, args.slot, args.image,
                                 args.version, args.stop_after, progress)
            else:
                print("[OTA] recovery mode requires a direct UART connection.")
                ota.recovery_upgrade_file(args.slot, args.image, progress,
                                          version=args.version)
        print("[OTA] transfer verified; device is rebooting into the pending slot.")
        return 0
    except OtaDeviceError as exc:
        print(str(exc), file=sys.stderr)
    except (OSError, TimeoutError) as exc:
        message = exc.strerror or str(exc)
        print(f"OTA failed: {message} ({-(exc.errno or errno.EIO)})", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
