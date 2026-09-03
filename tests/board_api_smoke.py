#!/usr/bin/env python3
"""CV184x Dara board API smoke test.

The default run is read-only.  ``--active`` additionally probes I2C and
reads detected onboard devices.  It never changes GPIO, PWM, UART, SPI,
watchdog, Wi-Fi mode, or Bluetooth state.
"""

import argparse
import logging


def report(name, callback):
    try:
        value = callback()
    except Exception as error:
        print(f"FAIL {name}: {type(error).__name__}: {error}")
        return False
    print(f"OK   {name}: {value}")
    return True


def test_core():
    from dara.core.registry import Registry
    from dara.core import log

    registry = Registry("smoke registry")

    @registry.register("sample", source="board-smoke")
    class Sample:
        pass

    assert registry.get("sample") is Sample
    assert isinstance(registry.create("sample"), Sample)
    log.set_level(log.LogLevel.INFO)
    log.info("core log smoke test")
    return "registry/log"


def test_peripheral(active):
    from dara.peripheral.pinmap import PinMap

    PinMap.init()
    print(
        "pinmap="
        f"pins={len(PinMap.get_pins())}; gpio={sorted(PinMap.gpio)}; i2c={sorted(PinMap.i2c)}; "
        f"spi={sorted(PinMap.spi)}; uart={sorted(PinMap.uart)}"
    )
    if not active:
        return "PinMap metadata only (add --active to probe I2C/ADC)"

    from dara.peripheral.adc import ADC
    from dara.peripheral.i2c import I2C

    for name in sorted(PinMap.i2c):
        def scan(name=name):
            with I2C(name) as bus:
                return [f"0x{address:02x}" for address in bus.scan()]

        report(f"I2C {name} scan", scan)

    for name in sorted(PinMap.adc):
        def read_adc(name=name):
            with ADC(name) as adc:
                return {"raw": adc.read_raw(), "volts": round(adc.read_vol(), 4)}

        report(f"ADC {name}", read_adc)
    return "active peripheral probe complete"


def test_devices(active):
    if not active:
        return "skipped (add --active; device initialization can configure sensors)"

    from dara.device.axp2101 import AXP2101
    from dara.device.mmc56x3 import MMC56X3
    from dara.device.qmi8658 import QMI8658
    from dara.peripheral.i2c import I2C

    with I2C("I2C0") as bus:
        found = set(bus.scan())
        print("I2C0 detected:", ", ".join(f"0x{address:02x}" for address in sorted(found)))
        if 0x34 in found:
            report("AXP2101", lambda: AXP2101(bus).battery_voltage())
        else:
            print("SKIP AXP2101: address 0x34 not detected")
        if 0x6B in found:
            def read_qmi():
                with QMI8658(bus) as imu:
                    sample = imu.read_all()
                    return {"acc": sample.acc, "gyro": sample.gyro, "temperature": sample.temperature}

            report("QMI8658", read_qmi)
        else:
            print("SKIP QMI8658: address 0x6b not detected")
        if 0x30 in found:
            def read_mmc():
                with MMC56X3(bus) as magnetometer:
                    sample = magnetometer.read_all()
                    return {"mag": sample.mag, "temperature": sample.temperature}

            report("MMC56X3", read_mmc)
        else:
            print("SKIP MMC56X3: address 0x30 not detected")
    return "detected-device reads complete"


def test_iot():
    from dara.iot.wifi import Wifi

    report("Wi-Fi status", lambda: Wifi().status())
    try:
        from dara.iot.bluetooth import list_adapters
    except ImportError as error:
        print(f"SKIP Bluetooth: optional dbus-fast dependency is unavailable ({error})")
    else:
        report("Bluetooth adapters", list_adapters)
    return "read-only connectivity checks complete"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--group",
        choices=("all", "core", "peripheral", "device", "iot"),
        default="all",
    )
    parser.add_argument(
        "--active",
        action="store_true",
        help="allow I2C/ADC probing and detected sensor reads",
    )
    args = parser.parse_args()
    logging.getLogger().setLevel(logging.INFO)

    tests = {
        "core": lambda: test_core(),
        "peripheral": lambda: test_peripheral(args.active),
        "device": lambda: test_devices(args.active),
        "iot": lambda: test_iot(),
    }
    selected = tests if args.group == "all" else {args.group: tests[args.group]}
    failures = [name for name, test in selected.items() if not report(name, test)]
    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()
