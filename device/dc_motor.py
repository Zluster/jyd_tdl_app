"""Bidirectional DC motor control through GPIO and PWM outputs."""


from dara.core._error_helpers import wrap_error_as
from dara.peripheral.gpio import GPIO, GPIODirection
from dara.peripheral.pwm import PWM


class DCMotorError(OSError):
    """Raised when a DC motor operation cannot be completed."""


class DCMotor:
    """A DC motor driven by two direction pins and one PWM channel."""

    def __init__(self, in1, in2, pwm, *, auto_open = True):
        """Create a DC motor from board identifiers and optionally activate it."""
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")
        self._in1 = GPIO(in1, GPIODirection.OUT, auto_open=False)
        self._in2 = GPIO(in2, GPIODirection.OUT, auto_open=False)
        self._pwm = PWM(pwm, duty=0.0, enable=True, auto_open=False)
        if auto_open:
            self.open()

    @wrap_error_as(DCMotorError, "DC motor open failed", catch=OSError)
    def open(self):
        """Open the motor outputs and stop the motor."""
        try:
            self._in1.open()
            self._in2.open()
            self._pwm.open()
            self.stop()
        except OSError:
            self.close()
            raise

    @wrap_error_as(DCMotorError, "DC motor close failed", catch=OSError)
    def close(self):
        """Stop the motor and close its GPIO and PWM outputs."""
        try:
            if self.is_opened:
                self.stop()
        finally:
            self._in1.close()
            self._in2.close()
            self._pwm.close()

    def __enter__(self):
        """Open the motor if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the motor when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether all motor outputs are open."""
        return self._in1.is_opened and self._in2.is_opened and self._pwm.is_opened

    @wrap_error_as(DCMotorError, "DC motor stop failed", catch=OSError)
    def stop(self):
        """Stop the motor and allow it to coast."""
        self._pwm.set_duty(0.0)
        self._in1.low()
        self._in2.low()

    @wrap_error_as(DCMotorError, "DC motor brake failed", catch=OSError)
    def brake(self):
        """Stop the motor using electrical braking."""
        self._pwm.set_duty(0.0)
        self._in1.high()
        self._in2.high()
        self._pwm.set_duty(1.0)

    @wrap_error_as(DCMotorError, "DC motor forward failed", catch=OSError)
    def forward(self, speed):
        """Run forward at ``speed`` from 0.0 through 1.0."""
        self._validate_speed(speed, 0.0, 1.0)
        self._pwm.set_duty(0.0)
        self._in1.high()
        self._in2.low()
        self._pwm.set_duty(speed)

    @wrap_error_as(DCMotorError, "DC motor backward failed", catch=OSError)
    def backward(self, speed):
        """Run backward at ``speed`` from 0.0 through 1.0."""
        self._validate_speed(speed, 0.0, 1.0)
        self._pwm.set_duty(0.0)
        self._in1.low()
        self._in2.high()
        self._pwm.set_duty(speed)

    def set_speed(self, speed):
        """Run at signed ``speed`` from -1.0 backward through 1.0 forward."""
        self._validate_speed(speed, -1.0, 1.0)
        if speed > 0:
            self.forward(speed)
        elif speed < 0:
            self.backward(-speed)
        else:
            self.stop()

    @staticmethod
    def _validate_speed(speed, minimum, maximum):
        """Reject nonnumeric speeds outside the allowed range."""
        if (
            isinstance(speed, bool)
            or not isinstance(speed, (int, float))
            or not minimum <= speed <= maximum
        ):
            raise ValueError(
                f"speed must be a number from {minimum} through {maximum}"
            )
