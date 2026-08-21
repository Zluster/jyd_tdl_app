"""PWM access to outputs declared in the board pin map."""



from ._periphery.pwm import PWM as _PeripheryPWM
from ._periphery.pwm import PWMError as _PeripheryPWMError

from dara.core._error_helpers import wrap_error_as

from .pinmap import PinMap


class PWMError(OSError):
    """Raised when a PWM operation cannot be completed."""


class PWM:
    """A PWM output selected by its board configuration identifier."""


    def __init__(
        self,
        id,
        freq = None,
        duty = None,
        enable = None,
        *,
        auto_open = True,
    ):
        """Create a PWM output and optionally open its mapped channel."""
        self.id = id
        self.info = PinMap.get_pwm(str(id))
        self._freq = self.info.freq if freq is None else freq
        self._duty = self.info.duty_cycle if duty is None else duty
        self._enable = self.info.enable if enable is None else enable
        if (
            isinstance(self._freq, bool)
            or not isinstance(self._freq, (int, float))
            or self._freq <= 0
        ):
            raise ValueError("freq must be a positive number or None")
        if (
            isinstance(self._duty, bool)
            or not isinstance(self._duty, (int, float))
            or not 0 <= self._duty <= 1
        ):
            raise ValueError("duty must be a number from 0.0 through 1.0 or None")
        if not isinstance(self._enable, bool):
            raise ValueError("enable must be a bool or None")
        self._periphery_instance = None
        if auto_open:
            self.open()

    @wrap_error_as(PWMError, "PWM open failed", catch=_PeripheryPWMError)
    def open(self):
        """Open or re-open the mapped PWM channel."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise PWMError("PWM init command failed") from error
            if return_code:
                raise PWMError(
                    f"PWM init command failed with exit status {return_code}"
                )
        if self.info.pin is not None and self.info.pin_func is not None:
            PinMap.set_pin_function(self.info.pin, self.info.pin_func)
        self._periphery_instance = _PeripheryPWM(self.info.chip, self.info.num)
        try:
            self.set_freq(self._freq)
            self.set_duty(self._duty)
            if self._enable:
                self.enable()
            else:
                self.disable()
        except Exception:
            self.close()
            raise

    @wrap_error_as(PWMError, "PWM close failed", catch=_PeripheryPWMError)
    def close(self):
        """Close the mapped PWM channel."""
        if self._periphery_instance is not None:
            self._periphery_instance.close()
            self._periphery_instance = None

    def __enter__(self):
        """Open the PWM channel if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the PWM channel when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the mapped PWM channel is open."""
        return self._periphery_instance is not None

    @property
    @wrap_error_as(PWMError, "PWM is_enabled failed", catch=_PeripheryPWMError)
    def is_enabled(self):
        """Return whether PWM output is enabled."""
        return self._backend.enabled

    @wrap_error_as(PWMError, "PWM enable failed", catch=_PeripheryPWMError)
    def enable(self):
        """Enable PWM output."""
        self._backend.enable()

    @wrap_error_as(PWMError, "PWM disable failed", catch=_PeripheryPWMError)
    def disable(self):
        """Disable PWM output."""
        self._backend.disable()

    @wrap_error_as(PWMError, "PWM freq failed", catch=_PeripheryPWMError)
    def get_freq(self):
        """Read the output frequency in Hertz."""
        return self._backend.frequency

    @wrap_error_as(PWMError, "PWM freq failed", catch=_PeripheryPWMError)
    def set_freq(self, value):
        """Set the output frequency in Hertz."""
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
            raise ValueError("frequency must be a positive number")
        self._backend.frequency = value

    @wrap_error_as(PWMError, "PWM duty failed", catch=_PeripheryPWMError)
    def get_duty(self):
        """Read the output duty cycle as a ratio from 0.0 to 1.0."""
        return self._backend.duty_cycle

    @wrap_error_as(PWMError, "PWM duty failed", catch=_PeripheryPWMError)
    def set_duty(self, value):
        """Set the output duty cycle as a ratio from 0.0 to 1.0."""
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not 0 <= value <= 1
        ):
            raise ValueError("duty_cycle must be a number from 0.0 through 1.0")
        self._backend.duty_cycle = value

    @property
    def _backend(self):
        """Return the open PWM backend."""
        if self._periphery_instance is None:
            raise PWMError("PWM is not open")
        return self._periphery_instance
