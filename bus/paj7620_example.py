#!/usr/bin/env python3
"""Read PAJ7620U2 gesture events from the GD32 UART protocol."""

from __future__ import annotations

import argparse
import errno
import sys
import time

from sensor_api import SensorApi
from sensor_uart import (PAJ7620_AUTO_UPLOAD_INTERVAL_MS,
                         SENSOR_PAJ7620U2_GESTURE_NONE,
                         SENSOR_TYPE_PAJ7620U2, paj7620_gesture_name,
                         paj7620_status_name)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="/dev/ttyS2")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--number", type=int, default=1)
    args = parser.parse_args()

    with SensorApi(args.device, args.baud) as api:
        api.set_auto_upload(SENSOR_TYPE_PAJ7620U2, args.number, True,
                            PAJ7620_AUTO_UPLOAD_INTERVAL_MS)
        print(f"PAJ7620U2 listening on {args.device} at {args.baud} 8N1; "
              "press Ctrl+C to stop.")
        last_sequence = 0
        last_gesture = SENSOR_PAJ7620U2_GESTURE_NONE
        last_status: int | None = None
        try:
            while True:
                try:
                    data = api.read(SENSOR_TYPE_PAJ7620U2, args.number)
                except OSError as exc:
                    if exc.errno != errno.ENODATA:
                        raise
                    time.sleep(0.02)
                    continue

                if data.sequence == last_sequence:
                    time.sleep(0.02)
                    continue
                last_sequence = data.sequence
                if not data.decoded_valid:
                    print(f"invalid PAJ7620U2 payload: {data.raw.hex(' ')}")
                    continue

                value = data.value
                if value["status"] != last_status:
                    print(f"status={paj7620_status_name(value['status'])}"
                          f"({value['status']})")
                    last_status = value["status"]
                if value["gesture"] == SENSOR_PAJ7620U2_GESTURE_NONE:
                    last_gesture = SENSOR_PAJ7620U2_GESTURE_NONE
                elif value["gesture"] != last_gesture:
                    print(f"gesture={paj7620_gesture_name(value['gesture'])}"
                          f"({value['gesture']}) "
                          f"flags_43=0x{value['gesture_flags']:02X} "
                          f"flags_44=0x{value['wave_flags']:02X}")
                    last_gesture = value["gesture"]
        except KeyboardInterrupt:
            pass
        finally:
            try:
                api.set_auto_upload(SENSOR_TYPE_PAJ7620U2, args.number, False, 0)
            except OSError as exc:
                print(f"disable auto upload failed: {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
