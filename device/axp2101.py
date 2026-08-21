"""AXP2101 power management unit driver."""


from os import sync

from dara.core._error_helpers import wrap_error_as
from dara.device.pmu import PMU, PMUError, PMUPowerChannel, pmu_drivers
from dara.peripheral.i2c import I2C


class AXP2101Error(PMUError):
    """Raised when an AXP2101 operation cannot be completed."""


@pmu_drivers.register("axp2101")
class AXP2101(PMU):
    """An AXP2101 power management unit connected over I2C."""

    _STATUS1 = 0x00
    _STATUS2 = 0x01
    _VERSION = 0x03
    _COMMON_CONFIG = 0x10
    _CHARGE_GAUGE_WATCHDOG = 0x18
    _BATTERY_VOLTAGE_HIGH = 0x34
    _BATTERY_VOLTAGE_LOW = 0x35
    _DCDC_CONTROL = 0x80
    _LDO_CONTROL0 = 0x90
    _LDO_CONTROL1 = 0x91
    _BATTERY_PERCENT = 0xA4
    _DEVICE_IDS = {0x47, 0x4A}

    _POWER_BITS = {
        PMUPowerChannel.DCDC1: (_DCDC_CONTROL, 0),
        PMUPowerChannel.DCDC2: (_DCDC_CONTROL, 1),
        PMUPowerChannel.DCDC3: (_DCDC_CONTROL, 2),
        PMUPowerChannel.DCDC4: (_DCDC_CONTROL, 3),
        PMUPowerChannel.DCDC5: (_DCDC_CONTROL, 4),
        PMUPowerChannel.ALDO1: (_LDO_CONTROL0, 0),
        PMUPowerChannel.ALDO2: (_LDO_CONTROL0, 1),
        PMUPowerChannel.ALDO3: (_LDO_CONTROL0, 2),
        PMUPowerChannel.ALDO4: (_LDO_CONTROL0, 3),
        PMUPowerChannel.BLDO1: (_LDO_CONTROL0, 4),
        PMUPowerChannel.BLDO2: (_LDO_CONTROL0, 5),
        PMUPowerChannel.DLDO1: (_LDO_CONTROL0, 7),
        PMUPowerChannel.DLDO2: (_LDO_CONTROL1, 0),
        PMUPowerChannel.VBACKUP: (_CHARGE_GAUGE_WATCHDOG, 2),
    }

    def __init__(
        self,
        i2c,
        addr = 0x34,
        *,
        auto_open = True,
    ):
        """Create an AXP2101 on a caller-owned bus and optionally verify it."""
        if not isinstance(i2c, I2C):
            raise ValueError("i2c must be an I2C instance")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")
        if not isinstance(addr, int) or isinstance(addr, bool) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        self._i2c = i2c
        self.addr = addr
        self._active = False
        if auto_open:
            self.open()

    @wrap_error_as(AXP2101Error, "AXP2101 open failed", catch=OSError)
    def open(self):
        """Verify the AXP2101 chip identifier on its open I2C bus."""
        self.close()
        if not self._i2c.is_opened:
            raise AXP2101Error("AXP2101 I2C bus is not open")
        self._active = True
        try:
            if self._read(self._VERSION) & 0xCF not in self._DEVICE_IDS:
                raise AXP2101Error("unexpected AXP2101 device ID")
        except Exception:
            self._active = False
            raise

    @wrap_error_as(AXP2101Error, "AXP2101 close failed", catch=OSError)
    def close(self):
        """Deactivate the PMU without changing outputs or closing its bus."""
        self._active = False

    def __enter__(self):
        """Open the PMU if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the PMU when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the PMU is active on an open I2C bus."""
        return self._active and self._i2c.is_opened

    @wrap_error_as(AXP2101Error, "AXP2101 voltage read failed", catch=OSError)
    def battery_voltage(self):
        """Return the battery voltage in millivolts."""
        high = self._read(self._BATTERY_VOLTAGE_HIGH)
        low = self._read(self._BATTERY_VOLTAGE_LOW)
        return (high & 0x1F) << 8 | low

    @wrap_error_as(AXP2101Error, "AXP2101 percentage read failed", catch=OSError)
    def battery_percent(self):
        """Return battery percentage, or ``-1`` when no battery is connected."""
        if not self._read(self._STATUS1) & 0x08:
            return -1
        return self._read(self._BATTERY_PERCENT)

    @wrap_error_as(AXP2101Error, "AXP2101 charging status read failed", catch=OSError)
    def is_charging(self):
        """Return whether the battery charging-status bit is set."""
        return bool(self._read(self._STATUS2) & 0x20)

    @wrap_error_as(AXP2101Error, "AXP2101 power channel update failed", catch=OSError)
    def set_power(self, channel, enable):
        """Enable or disable one supported AXP2101 power channel."""
        if not isinstance(channel, PMUPowerChannel):
            raise ValueError("channel must be a PMUPowerChannel value")
        if not isinstance(enable, bool):
            raise ValueError("enable must be a boolean")
        register, bit = self._POWER_BITS[channel]
        value = self._read(register)
        self._write(register, value | 1 << bit if enable else value & ~(1 << bit))

    @wrap_error_as(AXP2101Error, "AXP2101 poweroff failed", catch=OSError)
    def poweroff(self):
        """Synchronize filesystems and assert the software-poweroff bit."""
        config = self._read(self._COMMON_CONFIG)
        sync()
        self._write(self._COMMON_CONFIG, config | 0x01)

    def reboot(self):
        """Report that the AXP2101 has no hardware reboot operation."""
        raise NotImplementedError("AXP2101 does not support reboot")

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise AXP2101Error("AXP2101 is not open")
        return self._i2c

    def _read(self, register):
        """Read one AXP2101 register."""
        return self._bus.read_mem_byte(self.addr, register)

    def _write(self, register, value):
        """Write one AXP2101 register."""
        self._bus.write_mem_byte(self.addr, register, value)
