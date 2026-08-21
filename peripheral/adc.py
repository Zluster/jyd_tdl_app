"""Linux IIO ADC access for inputs declared in the board pin map."""



from dara.core._error_helpers import wrap_error_as

from .pinmap import PinMap


class ADCError(OSError):
    """Raised when an ADC operation cannot be completed."""


class ADC:
    """An ADC input exposed through the Linux IIO sysfs interface."""

    def __init__(
        self,
        id,
        resolution = None,
        vref = None,
        *,
        auto_open = True,
    ):
        """Create an ADC input and optionally open its mapped IIO value file."""
        self.id = id
        self.info = PinMap.get_adc(id)
        self.resolution = self.info.resolution if resolution is None else resolution
        self.vref = self.info.vref if vref is None else vref
        if (
            isinstance(self.resolution, bool)
            or not isinstance(self.resolution, int)
            or self.resolution <= 0
        ):
            raise ValueError("resolution must be a positive integer or None")
        if (
            isinstance(self.vref, bool)
            or not isinstance(self.vref, (int, float))
            or self.vref <= 0
        ):
            raise ValueError("vref must be a positive number or None")
        self._value_file = None
        if auto_open:
            self.open()

    @wrap_error_as(ADCError, "ADC open failed", catch=OSError)
    def open(self):
        """Open or re-open the mapped IIO raw-value file."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise ADCError("ADC init command failed") from error
            if return_code:
                raise ADCError(
                    f"ADC init command failed with exit status {return_code}"
                )
        if self.info.pin is not None and self.info.pin_func is not None:
            PinMap.set_pin_function(self.info.pin, self.info.pin_func)
        self._value_file = open(self.info.sysfs, encoding="ascii")

    @wrap_error_as(ADCError, "ADC close failed", catch=OSError)
    def close(self):
        """Close the mapped IIO raw-value file."""
        if self._value_file is not None:
            value_file, self._value_file = self._value_file, None
            value_file.close()

    def __enter__(self):
        """Open the ADC if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the ADC when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the mapped IIO raw-value file is open."""
        return self._value_file is not None

    @wrap_error_as(ADCError, "ADC read failed", catch=(OSError, ValueError))
    def read_raw(self):
        """Return the current unscaled ADC sample."""
        if self._value_file is None:
            raise ADCError("ADC is not open")
        self._value_file.seek(0)
        return int(self._value_file.read().strip())

    def read_vol(self):
        """Return the current ADC sample converted to volts."""
        return self.read_raw() * self.vref / ((1 << self.resolution) - 1)
