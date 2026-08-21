"""VL53L0X time-of-flight distance sensor driver.

Copyright (c) 2017-2022 Pololu Corporation.
Copyright (c) 2016 STMicroelectronics International N.V. All rights reserved.

This module derives its initialization and ranging algorithms from Pololu's
VL53L0X Arduino library and the ST VL53L0X API. The applicable license notices
are retained in the repository's ``THIRD_PARTY_LICENSES`` file.
"""


from enum import Enum
from math import isfinite
from time import monotonic

from dara.core._error_helpers import wrap_error_as
from dara.device.ds import DSData, DSError, DS, ds_drivers
from dara.peripheral.i2c import I2C


class VL53L0XError(DSError):
    """Raised when a VL53L0X operation cannot be completed."""

class VL53L0XData(DSData):
    """Distance from one VL53L0X sample."""

class VL53L0XVcselPeriodType(str, Enum):
    """Ranging sequence step whose laser pulse period is configured."""

    PRE_RANGE = "pre_range"
    FINAL_RANGE = "final_range"


class _SequenceEnables:
    """Enabled ranging sequence steps."""


    def __init__(
        self,
        tcc,
        dss,
        msrc,
        pre_range,
        final_range,
    ):
        """Initialize enabled ranging sequence steps."""
        self.tcc = tcc
        self.dss = dss
        self.msrc = msrc
        self.pre_range = pre_range
        self.final_range = final_range


class _SequenceTimeouts:
    """Decoded ranging sequence timeouts."""


    def __init__(
        self,
        pre_period,
        final_period,
        msrc_mclks,
        pre_mclks,
        final_mclks,
        msrc_us,
        pre_us,
        final_us,
    ):
        """Initialize decoded ranging sequence timeouts."""
        self.pre_period = pre_period
        self.final_period = final_period
        self.msrc_mclks = msrc_mclks
        self.pre_mclks = pre_mclks
        self.final_mclks = final_mclks
        self.msrc_us = msrc_us
        self.pre_us = pre_us
        self.final_us = final_us


@ds_drivers.register("vl53l0x")
class VL53L0X(DS):
    """A VL53L0X time-of-flight distance sensor connected over I2C."""

    _SYSRANGE_START = 0x00
    _SYSTEM_SEQUENCE_CONFIG = 0x01
    _SYSTEM_INTERMEASUREMENT_PERIOD = 0x04
    _SYSTEM_INTERRUPT_CONFIG_GPIO = 0x0A
    _SYSTEM_INTERRUPT_CLEAR = 0x0B
    _RESULT_INTERRUPT_STATUS = 0x13
    _RESULT_RANGE_STATUS = 0x14
    _FINAL_RATE_LIMIT = 0x44
    _MSRC_TIMEOUT = 0x46
    _PRE_VALID_PHASE_LOW = 0x56
    _PRE_VALID_PHASE_HIGH = 0x57
    _PRE_VCSEL_PERIOD = 0x50
    _PRE_TIMEOUT = 0x51
    _MSRC_CONFIG_CONTROL = 0x60
    _FINAL_VALID_PHASE_LOW = 0x47
    _FINAL_VALID_PHASE_HIGH = 0x48
    _FINAL_VCSEL_PERIOD = 0x70
    _FINAL_TIMEOUT = 0x71
    _GPIO_HV_MUX_ACTIVE_HIGH = 0x84
    _I2C_ADDRESS = 0x8A
    _EXTSUP_HV = 0x89
    _MODEL_ID = 0xC0
    _OSC_CALIBRATE_VAL = 0xF8
    _GLOBAL_VCSEL_WIDTH = 0x32
    _SPAD_ENABLES = 0xB0
    _ALGO_PHASECAL = 0x30

    _TUNING_SETTINGS = (
        (0xFF, 0x01), (0x00, 0x00), (0xFF, 0x00), (0x09, 0x00),
        (0x10, 0x00), (0x11, 0x00), (0x24, 0x01), (0x25, 0xFF),
        (0x75, 0x00), (0xFF, 0x01), (0x4E, 0x2C), (0x48, 0x00),
        (0x30, 0x20), (0xFF, 0x00), (0x30, 0x09), (0x54, 0x00),
        (0x31, 0x04), (0x32, 0x03), (0x40, 0x83), (0x46, 0x25),
        (0x60, 0x00), (0x27, 0x00), (0x50, 0x06), (0x51, 0x00),
        (0x52, 0x96), (0x56, 0x08), (0x57, 0x30), (0x61, 0x00),
        (0x62, 0x00), (0x64, 0x00), (0x65, 0x00), (0x66, 0xA0),
        (0xFF, 0x01), (0x22, 0x32), (0x47, 0x14), (0x49, 0xFF),
        (0x4A, 0x00), (0xFF, 0x00), (0x7A, 0x0A), (0x7B, 0x00),
        (0x78, 0x21), (0xFF, 0x01), (0x23, 0x34), (0x42, 0x00),
        (0x44, 0xFF), (0x45, 0x26), (0x46, 0x05), (0x40, 0x40),
        (0x0E, 0x06), (0x20, 0x1A), (0x43, 0x40), (0xFF, 0x00),
        (0x34, 0x03), (0x35, 0x44), (0xFF, 0x01), (0x31, 0x04),
        (0x4B, 0x09), (0x4C, 0x05), (0x4D, 0x04), (0xFF, 0x00),
        (0x44, 0x00), (0x45, 0x20), (0x47, 0x08), (0x48, 0x28),
        (0x67, 0x00), (0x70, 0x04), (0x71, 0x01), (0x72, 0xFE),
        (0x76, 0x00), (0x77, 0x00), (0xFF, 0x01), (0x0D, 0x01),
        (0xFF, 0x00), (0x80, 0x01), (0x01, 0xF8), (0xFF, 0x01),
        (0x8E, 0x01), (0x00, 0x01), (0xFF, 0x00), (0x80, 0x00),
    )

    def __init__(
        self,
        i2c,
        addr = 0x29,
        io_2v8 = False,
        *,
        timeout_ms = 0,
        auto_open = True,
    ):
        """Create a VL53L0X on a caller-owned bus and optionally initialize it."""
        if not isinstance(i2c, I2C):
            raise ValueError("i2c must be an I2C instance")
        if isinstance(addr, bool) or not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        if not isinstance(io_2v8, bool):
            raise ValueError("io_2v8 must be a boolean")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")
        self._i2c = i2c
        self._addr = addr
        self._io_2v8 = io_2v8
        self._active = False
        self._continuous = False
        self._stop_variable = 0
        self._measurement_timing_budget_us = 0
        self._timeout_ms = 0
        self.set_timeout(timeout_ms)
        if auto_open:
            self.open()

    def __enter__(self):
        """Open the sensor if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the sensor when leaving a ``with`` statement."""
        self.close()

    @property
    def addr(self):
        """Return the fixed 7-bit I2C address selected at construction."""
        return self._addr

    @wrap_error_as(VL53L0XError, "VL53L0X open failed", catch=OSError)
    def open(self):
        """Verify, initialize, tune, and calibrate the sensor."""
        self.close()
        if not self._i2c.is_opened:
            raise VL53L0XError("VL53L0X I2C bus is not open")
        self._active = True
        try:
            if self._read_reg(self._MODEL_ID) != 0xEE:
                raise VL53L0XError("unexpected VL53L0X model ID")
            if self._io_2v8:
                self._write_reg(self._EXTSUP_HV, self._read_reg(self._EXTSUP_HV) | 1)
            self._write_reg(0x88, 0)
            self._write_stop_variable_sequence(read=True)
            self._write_reg(
                self._MSRC_CONFIG_CONTROL,
                self._read_reg(self._MSRC_CONFIG_CONTROL) | 0x12,
            )
            self.set_signal_rate_limit(0.25)
            self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0xFF)
            spad_count, aperture = self._get_spad_info()
            spad_map = bytearray(self._read_multi(self._SPAD_ENABLES, 6))
            self._write_reg(0xFF, 1)
            self._write_reg(0x4F, 0)
            self._write_reg(0x4E, 0x2C)
            self._write_reg(0xFF, 0)
            self._write_reg(0xB6, 0xB4)
            first = 12 if aperture else 0
            enabled = 0
            for index in range(48):
                if index < first or enabled == spad_count:
                    spad_map[index // 8] &= ~(1 << (index % 8))
                elif spad_map[index // 8] & (1 << (index % 8)):
                    enabled += 1
            self._write_multi(self._SPAD_ENABLES, spad_map)
            for register, value in self._TUNING_SETTINGS:
                self._write_reg(register, value)
            self._write_reg(self._SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04)
            self._write_reg(
                self._GPIO_HV_MUX_ACTIVE_HIGH,
                self._read_reg(self._GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10,
            )
            self._write_reg(self._SYSTEM_INTERRUPT_CLEAR, 1)
            budget = self.get_measurement_timing_budget()
            self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0xE8)
            self.set_measurement_timing_budget(budget)
            self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0x01)
            self._perform_single_ref_calibration(0x40)
            self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0x02)
            self._perform_single_ref_calibration(0)
            self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0xE8)
        except Exception:
            self._active = False
            self._continuous = False
            raise

    @wrap_error_as(VL53L0XError, "VL53L0X close failed", catch=OSError)
    def close(self):
        """Stop continuous ranging without closing the caller-owned I2C bus."""
        if not self._active:
            return
        try:
            if self._continuous:
                self.stop_continuous()
        finally:
            self._continuous = False
            self._active = False

    @property
    def is_opened(self):
        """Return whether the sensor is active on an open I2C bus."""
        return self._active and self._i2c.is_opened

    def set_timeout(self, timeout_ms):
        """Set the polling timeout in milliseconds; zero disables timeouts."""
        if (
            isinstance(timeout_ms, bool)
            or not isinstance(timeout_ms, int)
            or not 0 <= timeout_ms <= 0xFFFF
        ):
            raise ValueError("timeout_ms must be an integer from 0 through 65535")
        self._timeout_ms = timeout_ms

    def get_timeout(self):
        """Return the polling timeout in milliseconds, or zero when disabled."""
        return self._timeout_ms

    def set_signal_rate_limit(self, limit_mcps):
        """Set the final-range return signal limit in mega counts per second."""
        if (
            isinstance(limit_mcps, bool)
            or not isinstance(limit_mcps, (int, float))
            or not isfinite(limit_mcps)
            or not 0 <= limit_mcps <= 511.99
        ):
            raise ValueError("limit_mcps must be finite and between 0 and 511.99")
        self._write_reg16_bit(self._FINAL_RATE_LIMIT, int(limit_mcps * 128))

    def get_signal_rate_limit(self):
        """Return the final-range signal limit in mega counts per second."""
        return self._read_reg16_bit(self._FINAL_RATE_LIMIT) / 128.0

    def set_measurement_timing_budget(self, budget_us):
        """Set the measurement timing budget in microseconds."""
        if (
            isinstance(budget_us, bool)
            or not isinstance(budget_us, int)
            or not 20_000 <= budget_us <= 0xFFFFFFFF
        ):
            raise ValueError("budget_us must be an integer from 20000 through 4294967295")
        enables = self._get_sequence_enables()
        timeouts = self._get_sequence_timeouts(enables)
        used = 1910 + 960
        if enables.tcc:
            used += timeouts.msrc_us + 590
        if enables.dss:
            used += 2 * (timeouts.msrc_us + 690)
        elif enables.msrc:
            used += timeouts.msrc_us + 660
        if enables.pre_range:
            used += timeouts.pre_us + 660
        if enables.final_range:
            used += 550
            if used > budget_us:
                raise ValueError("budget_us is too small for the enabled sequence")
            final_mclks = self._us_to_mclks(budget_us - used, timeouts.final_period)
            if enables.pre_range:
                final_mclks += timeouts.pre_mclks
            self._write_reg16_bit(self._FINAL_TIMEOUT, self._encode_timeout(final_mclks))
        self._measurement_timing_budget_us = budget_us

    def get_measurement_timing_budget(self):
        """Return the effective measurement timing budget in microseconds."""
        enables = self._get_sequence_enables()
        timeouts = self._get_sequence_timeouts(enables)
        budget = 1910 + 960
        if enables.tcc:
            budget += timeouts.msrc_us + 590
        if enables.dss:
            budget += 2 * (timeouts.msrc_us + 690)
        elif enables.msrc:
            budget += timeouts.msrc_us + 660
        if enables.pre_range:
            budget += timeouts.pre_us + 660
        if enables.final_range:
            budget += timeouts.final_us + 550
        self._measurement_timing_budget_us = budget
        return budget

    def set_vcsel_pulse_period(
        self, period_type, period_pclks
    ):
        """Set a pre-range or final-range VCSEL pulse period in PCLKs."""
        if not isinstance(period_type, VL53L0XVcselPeriodType):
            raise ValueError("period_type must be a VL53L0XVcselPeriodType value")
        if isinstance(period_pclks, bool) or not isinstance(period_pclks, int):
            raise ValueError("period_pclks must be an integer")
        valid = (12, 14, 16, 18) if period_type is VL53L0XVcselPeriodType.PRE_RANGE else (8, 10, 12, 14)
        if period_pclks not in valid:
            raise ValueError(f"period_pclks must be one of {valid}")
        enables = self._get_sequence_enables()
        timeouts = self._get_sequence_timeouts(enables)
        encoded = period_pclks // 2 - 1
        if period_type is VL53L0XVcselPeriodType.PRE_RANGE:
            self._write_reg(self._PRE_VALID_PHASE_HIGH, {12: 0x18, 14: 0x30, 16: 0x40, 18: 0x50}[period_pclks])
            self._write_reg(self._PRE_VALID_PHASE_LOW, 0x08)
            self._write_reg(self._PRE_VCSEL_PERIOD, encoded)
            self._write_reg16_bit(self._PRE_TIMEOUT, self._encode_timeout(self._us_to_mclks(timeouts.pre_us, period_pclks)))
            msrc = self._us_to_mclks(timeouts.msrc_us, period_pclks)
            self._write_reg(self._MSRC_TIMEOUT, 255 if msrc > 256 else max(0, msrc - 1))
        else:
            high, width, phase_timeout, phase_limit = {
                8: (0x10, 0x02, 0x0C, 0x30),
                10: (0x28, 0x03, 0x09, 0x20),
                12: (0x38, 0x03, 0x08, 0x20),
                14: (0x48, 0x03, 0x07, 0x20),
            }[period_pclks]
            self._write_reg(self._FINAL_VALID_PHASE_HIGH, high)
            self._write_reg(self._FINAL_VALID_PHASE_LOW, 0x08)
            self._write_reg(self._GLOBAL_VCSEL_WIDTH, width)
            self._write_reg(self._ALGO_PHASECAL, phase_timeout)
            self._write_reg(0xFF, 1)
            self._write_reg(self._ALGO_PHASECAL, phase_limit)
            self._write_reg(0xFF, 0)
            self._write_reg(self._FINAL_VCSEL_PERIOD, encoded)
            final = self._us_to_mclks(timeouts.final_us, period_pclks)
            if enables.pre_range:
                final += timeouts.pre_mclks
            self._write_reg16_bit(self._FINAL_TIMEOUT, self._encode_timeout(final))
        self.set_measurement_timing_budget(self._measurement_timing_budget_us)
        sequence = self._read_reg(self._SYSTEM_SEQUENCE_CONFIG)
        self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, 0x02)
        self._perform_single_ref_calibration(0)
        self._write_reg(self._SYSTEM_SEQUENCE_CONFIG, sequence)

    def get_vcsel_pulse_period(self, period_type):
        """Return a pre-range or final-range VCSEL pulse period in PCLKs."""
        if not isinstance(period_type, VL53L0XVcselPeriodType):
            raise ValueError("period_type must be a VL53L0XVcselPeriodType value")
        register = self._PRE_VCSEL_PERIOD if period_type is VL53L0XVcselPeriodType.PRE_RANGE else self._FINAL_VCSEL_PERIOD
        return (self._read_reg(register) + 1) * 2

    def start_continuous(self, period_ms = 0):
        """Start back-to-back or timed continuous ranging."""
        if (
            isinstance(period_ms, bool)
            or not isinstance(period_ms, int)
            or not 0 <= period_ms <= 0xFFFFFFFF
        ):
            raise ValueError("period_ms must be an integer from 0 through 4294967295")
        self._write_stop_variable_sequence(read=False)
        if period_ms:
            oscillator = self._read_reg16_bit(self._OSC_CALIBRATE_VAL)
            programmed = period_ms * oscillator if oscillator else period_ms
            if programmed > 0xFFFFFFFF:
                raise ValueError("calibrated period does not fit in 32 bits")
            self._write_reg32_bit(self._SYSTEM_INTERMEASUREMENT_PERIOD, programmed)
            self._write_reg(self._SYSRANGE_START, 0x04)
        else:
            self._write_reg(self._SYSRANGE_START, 0x02)
        self._continuous = True

    def stop_continuous(self):
        """Stop continuous ranging and restore single-shot state."""
        self._write_reg(self._SYSRANGE_START, 0x01)
        self._write_reg(0xFF, 0x01)
        self._write_reg(0x00, 0x00)
        self._write_reg(0x91, 0x00)
        self._write_reg(0x00, 0x01)
        self._write_reg(0xFF, 0x00)
        self._continuous = False

    def read_range_continuous(self):
        """Read the next continuous range sample in centimeters."""
        self._wait_for(lambda: self._read_reg(self._RESULT_INTERRUPT_STATUS) & 0x07, "range measurement")
        millimeters = self._read_reg16_bit(self._RESULT_RANGE_STATUS + 10)
        self._write_reg(self._SYSTEM_INTERRUPT_CLEAR, 0x01)
        return millimeters / 10.0

    def read_range_single(self):
        """Perform one range measurement and return centimeters."""
        self._write_stop_variable_sequence(read=False)
        self._write_reg(self._SYSRANGE_START, 0x01)
        self._wait_for(lambda: not self._read_reg(self._SYSRANGE_START) & 0x01, "single-shot start")
        return self.read_range_continuous()

    def read_distance(self):
        """Return the next continuous or a new single-shot distance in centimeters."""
        return self.read_range_continuous() if self._continuous else self.read_range_single()
    
    def read_all(self):
        """Return distance from one sample."""
        return VL53L0XData(self.read_distance())

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise VL53L0XError("VL53L0X is not open")
        return self._i2c

    def _write_integer(self, register, value, width):
        """Validate and write an unsigned big-endian integer."""
        if isinstance(register, bool) or not isinstance(register, int) or not 0 <= register <= 0xFF:
            raise ValueError("register must be an unsigned 8-bit integer")
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << (width * 8):
            raise ValueError(f"value must be an unsigned {width * 8}-bit integer")
        self._write_multi(register, value.to_bytes(width, "big"))

    def _write_stop_variable_sequence(self, *, read):
        """Enter the private page and acquire or restore the stop variable."""
        self._write_reg(0x80, 1)
        self._write_reg(0xFF, 1)
        self._write_reg(0x00, 0)
        if read:
            self._stop_variable = self._read_reg(0x91)
        else:
            self._write_reg(0x91, self._stop_variable)
        self._write_reg(0x00, 1)
        self._write_reg(0xFF, 0)
        self._write_reg(0x80, 0)

    def _get_spad_info(self):
        """Read the factory-programmed reference SPAD count and type."""
        self._write_reg(0x80, 1)
        self._write_reg(0xFF, 1)
        self._write_reg(0x00, 0)
        self._write_reg(0xFF, 6)
        self._write_reg(0x83, self._read_reg(0x83) | 4)
        self._write_reg(0xFF, 7)
        self._write_reg(0x81, 1)
        self._write_reg(0x80, 1)
        self._write_reg(0x94, 0x6B)
        self._write_reg(0x83, 0)
        self._wait_for(lambda: self._read_reg(0x83) != 0, "SPAD information")
        self._write_reg(0x83, 1)
        value = self._read_reg(0x92)
        self._write_reg(0x81, 0)
        self._write_reg(0xFF, 6)
        self._write_reg(0x83, self._read_reg(0x83) & ~4)
        self._write_reg(0xFF, 1)
        self._write_reg(0x00, 1)
        self._write_reg(0xFF, 0)
        self._write_reg(0x80, 0)
        return value & 0x7F, bool(value & 0x80)

    def _get_sequence_enables(self):
        """Decode the enabled ranging sequence steps."""
        config = self._read_reg(self._SYSTEM_SEQUENCE_CONFIG)
        return _SequenceEnables(bool(config & 0x10), bool(config & 0x08), bool(config & 0x04), bool(config & 0x40), bool(config & 0x80))

    def _get_sequence_timeouts(self, enables):
        """Decode all ranging sequence timeout registers."""
        pre_period = (self._read_reg(self._PRE_VCSEL_PERIOD) + 1) * 2
        msrc_mclks = self._read_reg(self._MSRC_TIMEOUT) + 1
        pre_mclks = self._decode_timeout(self._read_reg16_bit(self._PRE_TIMEOUT))
        final_period = (self._read_reg(self._FINAL_VCSEL_PERIOD) + 1) * 2
        final_mclks = self._decode_timeout(self._read_reg16_bit(self._FINAL_TIMEOUT))
        if enables.pre_range:
            final_mclks = max(0, final_mclks - pre_mclks)
        return _SequenceTimeouts(
            pre_period, final_period, msrc_mclks, pre_mclks, final_mclks,
            self._mclks_to_us(msrc_mclks, pre_period),
            self._mclks_to_us(pre_mclks, pre_period),
            self._mclks_to_us(final_mclks, final_period),
        )

    @staticmethod
    def _decode_timeout(value):
        """Decode a sequence timeout register into macro clock periods."""
        return ((value & 0xFF) << (value >> 8)) + 1

    @staticmethod
    def _encode_timeout(timeout_mclks):
        """Encode macro clock periods for a sequence timeout register."""
        if timeout_mclks <= 0:
            return 0
        mantissa = timeout_mclks - 1
        exponent = 0
        while mantissa > 0xFF:
            mantissa >>= 1
            exponent += 1
        return (exponent << 8) | mantissa

    @staticmethod
    def _macro_period_ns(period_pclks):
        """Return one ranging macro period in nanoseconds."""
        return (2304 * period_pclks * 1655 + 500) // 1000

    @classmethod
    def _mclks_to_us(cls, timeout_mclks, period_pclks):
        """Convert macro clock periods to microseconds."""
        return (timeout_mclks * cls._macro_period_ns(period_pclks) + 500) // 1000

    @classmethod
    def _us_to_mclks(cls, timeout_us, period_pclks):
        """Convert microseconds to macro clock periods."""
        macro = cls._macro_period_ns(period_pclks)
        return (timeout_us * 1000 + macro // 2) // macro

    def _perform_single_ref_calibration(self, vhv_init):
        """Perform one VHV or phase reference calibration."""
        self._write_reg(self._SYSRANGE_START, 0x01 | vhv_init)
        self._wait_for(lambda: self._read_reg(self._RESULT_INTERRUPT_STATUS) & 0x07, "reference calibration")
        self._write_reg(self._SYSTEM_INTERRUPT_CLEAR, 1)
        self._write_reg(self._SYSRANGE_START, 0)

    def _wait_for(self, predicate, operation):
        """Poll a sensor predicate and raise when the configured timeout expires."""
        deadline = monotonic() + self._timeout_ms / 1000 if self._timeout_ms else None
        while not predicate():
            if deadline is not None and monotonic() >= deadline:
                raise VL53L0XError(f"{operation} timed out")
            
    @wrap_error_as(VL53L0XError, "VL53L0X register write failed", catch=OSError)
    def _write_reg(self, register, value):
        """Write an 8-bit sensor register."""
        if isinstance(register, bool) or not isinstance(register, int) or not 0 <= register <= 0xFF:
            raise ValueError("register must be an unsigned 8-bit integer")
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFF:
            raise ValueError("value must be an unsigned 8-bit integer")
        if register == self._I2C_ADDRESS:
            raise ValueError("the VL53L0X I2C address register is read-only")
        self._bus.write_mem_byte(self._addr, register, value)

    def _write_reg16_bit(self, register, value):
        """Write a big-endian 16-bit sensor register value."""
        self._write_integer(register, value, 2)

    def _write_reg32_bit(self, register, value):
        """Write a big-endian 32-bit sensor register value."""
        self._write_integer(register, value, 4)

    @wrap_error_as(VL53L0XError, "VL53L0X register read failed", catch=OSError)
    def _read_reg(self, register):
        """Read an 8-bit sensor register."""
        if isinstance(register, bool) or not isinstance(register, int) or not 0 <= register <= 0xFF:
            raise ValueError("register must be an unsigned 8-bit integer")
        return self._bus.read_mem_byte(self._addr, register)

    def _read_reg16_bit(self, register):
        """Read a big-endian 16-bit sensor register value."""
        return int.from_bytes(self._read_multi(register, 2), "big")

    def _read_reg32_bit(self, register):
        """Read a big-endian 32-bit sensor register value."""
        return int.from_bytes(self._read_multi(register, 4), "big")

    @wrap_error_as(VL53L0XError, "VL53L0X register write failed", catch=OSError)
    def _write_multi(self, register, data):
        """Write bytes to consecutive sensor registers."""
        if isinstance(register, bool) or not isinstance(register, int) or not 0 <= register <= 0xFF:
            raise ValueError("register must be an unsigned 8-bit integer")
        if not isinstance(data, (bytes, bytearray)):
            raise ValueError("data must be bytes or bytearray")
        if register + len(data) > 0x100:
            raise ValueError("register span must fit in the 8-bit register map")
        if register <= self._I2C_ADDRESS < register + len(data):
            raise ValueError("the VL53L0X I2C address register is read-only")
        self._bus.write_mem(self._addr, register, bytes(data))

    @wrap_error_as(VL53L0XError, "VL53L0X register read failed", catch=OSError)
    def _read_multi(self, register, count):
        """Read bytes from consecutive sensor registers."""
        if isinstance(register, bool) or not isinstance(register, int) or not 0 <= register <= 0xFF:
            raise ValueError("register must be an unsigned 8-bit integer")
        if isinstance(count, bool) or not isinstance(count, int) or count < 0:
            raise ValueError("count must be a non-negative integer")
        if register + count > 0x100:
            raise ValueError("register span must fit in the 8-bit register map")
        return self._bus.read_mem(self._addr, register, count)
