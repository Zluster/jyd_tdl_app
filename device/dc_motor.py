"""Single-direction-pin DC motor control through board motor identifiers."""

import atexit
import time
import weakref

from dara.core._error_helpers import wrap_error_as
from dara.peripheral.gpio import GPIO, GPIODirection
from dara.peripheral.pinmap import PinMap
from dara.peripheral.pwm import PWM


class MotorError(OSError):
    """Raised when a motor operation cannot be completed."""


_OPEN_MOTORS = weakref.WeakSet()


def _close_motors():
    for motor in list(_OPEN_MOTORS):
        try:
            motor.close()
        except Exception:
            pass


atexit.register(_close_motors)


class Motor:
    """One ``DIR + PWM`` motor selected as ``Motor(1)`` or ``Motor(2)``.

    The board configuration supplies the two physical pins for each motor.
    They feed the two inputs of the board's H-bridge: one is a direction GPIO
    and the other is a PWM signal.  Positive speed is forward; set
    ``forward_high=False`` when the connected driver uses the opposite
    direction polarity.
    """

    _MIN_DRIVE = 0.50

    def __init__(self, number, *, forward_high=True, auto_open=True):
        if isinstance(number, bool) or not isinstance(number, int) or number not in (1, 2):
            raise ValueError("motor number must be 1 or 2")
        if not isinstance(forward_high, bool):
            raise ValueError("forward_high must be a boolean")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self.number = number
        self._forward_high = forward_high
        self._direction = GPIO(
            "MOTOR%d_DIR" % number,
            GPIODirection.OUT,
            initial=False,
            auto_open=False,
        )
        self._pwm = PWM(
            "MOTOR%d_PWM" % number,
            duty=0.0,
            enable=False,
            auto_open=False,
        )
        self._pwm_idle_id = "MOTOR%d_PWM_IDLE" % number
        self._running_direction = None
        if auto_open:
            self.open()

    @wrap_error_as(MotorError, "Motor open failed", catch=OSError)
    def open(self):
        """Open both outputs in a stopped state."""
        try:
            self._direction.open()
            self._pwm.open()
            self.stop()
            _OPEN_MOTORS.add(self)
        except OSError:
            self.close()
            raise

    @wrap_error_as(MotorError, "Motor close failed", catch=OSError)
    def close(self):
        """Stop, latch both bridge inputs low, and release both resources."""
        try:
            if self._pwm.is_opened:
                self.stop()
        finally:
            try:
                if self._direction.is_opened:
                    try:
                        self._set_pwm_idle(False)
                    finally:
                        self._direction.low()
            finally:
                self._running_direction = None
                try:
                    self._pwm.close()
                finally:
                    self._direction.close()
                    _OPEN_MOTORS.discard(self)

    def __del__(self):
        """Best-effort safety stop when a motor object is discarded."""
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        self.close()

    @property
    def is_opened(self):
        """Return whether both the direction GPIO and PWM are open."""
        return self._direction.is_opened and self._pwm.is_opened

    @wrap_error_as(MotorError, "Motor stop failed", catch=OSError)
    def stop(self):
        """Brake the H-bridge and leave the PWM pin at its safe idle level."""
        direction_high = self._direction.read()
        self._pwm.set_duty(1.0 if direction_high else 0.0)
        # The two motor signals are H-bridge inputs.  Equal levels brake:
        # DIR=1 needs PWM=1, while DIR=0 needs PWM=0.  A fixed low PWM level
        # would instead command full speed whenever DIR is high.
        # Latch GPIO idle before disable, whose output level is undefined.
        self._set_pwm_idle(direction_high)
        self._pwm.disable()
        self._running_direction = None

    @wrap_error_as(MotorError, "Motor forward failed", catch=OSError)
    def forward(self, speed=1.0):
        """Run forward at ``speed`` from 0.0 through 1.0."""
        self._run(True, speed)

    @wrap_error_as(MotorError, "Motor backward failed", catch=OSError)
    def backward(self, speed=1.0):
        """Run backward at ``speed`` from 0.0 through 1.0."""
        self._run(False, speed)

    def set_speed(self, speed):
        """Run at signed ``speed`` from -1.0 through 1.0, or stop at zero."""
        self._validate_speed(speed, -1.0, 1.0)
        if speed > 0:
            self.forward(speed)
        elif speed < 0:
            self.backward(-speed)
        else:
            self.stop()

    def _run(self, forward, speed):
        self._validate_speed(speed, 0.0, 1.0)
        if speed == 0:
            self.stop()
            return
        direction_high = forward == self._forward_high
        if self._running_direction == direction_high:
            self._pwm.set_duty(self._pwm_duty(direction_high, speed))
            return
        try:
            self.stop()
            self._set_pwm_idle(direction_high)
            self._direction.write(direction_high)
            time.sleep(0.002)
            self._pwm.set_duty(self._pwm_duty(direction_high, speed))
            # Enable while the pin is still GPIO, then connect a running PWM.
            self._pwm.enable()
            PinMap.set_pin_function(self._pwm.info.pin, self._pwm.info.pin_func)
            self._running_direction = direction_high
        except BaseException:
            self.close()
            raise

    @classmethod
    def _pwm_duty(cls, direction_high, speed):
        drive = cls._MIN_DRIVE + (1.0 - cls._MIN_DRIVE) * speed
        return 1.0 - drive if direction_high else drive

    def _set_pwm_idle(self, high):
        """Select GPIO mode, latch the H-bridge's brake level, and release."""
        # Do not retain a GPIO line request while PWM is active: on CV184x it
        # can prevent the next alternate-function PWM drive from reaching the
        # pin.  The GPIO output latch retains the selected idle level after
        # the request is closed.
        with GPIO(
            self._pwm_idle_id,
            GPIODirection.OUT,
            initial=high,
        ) as pwm_idle:
            pwm_idle.write(high)

    @staticmethod
    def _validate_speed(speed, minimum, maximum):
        if (
            isinstance(speed, bool)
            or not isinstance(speed, (int, float))
            or not minimum <= speed <= maximum
        ):
            raise ValueError(
                "speed must be a number from %s through %s" % (minimum, maximum)
            )


class Wheel:
    """The two-motor wheel assembly made from ``Motor(1)`` and ``Motor(2)``."""

    def __init__(self, *, left_forward_high=True, right_forward_high=True,
                 auto_open=True):
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")
        self._left = Motor(
            1, forward_high=left_forward_high, auto_open=False,
        )
        self._right = Motor(
            2, forward_high=right_forward_high, auto_open=False,
        )
        if auto_open:
            self.open()

    @property
    def left(self):
        """Return the left ``Motor(1)`` instance."""
        return self._left

    @property
    def right(self):
        """Return the right ``Motor(2)`` instance."""
        return self._right

    @property
    def is_opened(self):
        return self._left.is_opened and self._right.is_opened

    @wrap_error_as(MotorError, "Wheel open failed", catch=OSError)
    def open(self):
        """Open both motors in a stopped state."""
        try:
            self._left.open()
            self._right.open()
        except OSError:
            self.close()
            raise

    @wrap_error_as(MotorError, "Wheel close failed", catch=OSError)
    def close(self):
        """Stop and release both motors."""
        try:
            self._left.close()
        finally:
            self._right.close()

    def __enter__(self):
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        self.close()

    def __del__(self):
        """Best-effort safety stop when a wheel object is discarded."""
        try:
            self.close()
        except Exception:
            pass

    @wrap_error_as(MotorError, "Wheel speed update failed", catch=OSError)
    def set_speed(self, left, right):
        """Set independent signed speeds for motor 1 and motor 2."""
        Motor._validate_speed(left, -1.0, 1.0)
        Motor._validate_speed(right, -1.0, 1.0)
        try:
            self._left.set_speed(left)
            self._right.set_speed(right)
        except OSError:
            try:
                self.stop()
            except OSError:
                pass
            raise

    def forward(self, speed=1.0):
        """Drive both motors forward at the same speed."""
        self.set_speed(speed, speed)

    def backward(self, speed=1.0):
        """Drive both motors backward at the same speed."""
        self.set_speed(-speed, -speed)

    def turn_left(self, speed=1.0):
        """Turn left while moving forward or backward from signed speed."""
        Motor._validate_speed(speed, -1.0, 1.0)
        if speed >= 0:
            self.set_speed(0.0, speed)
        else:
            self.set_speed(speed, 0.0)

    def turn_right(self, speed=1.0):
        """Turn right while moving forward or backward from signed speed."""
        Motor._validate_speed(speed, -1.0, 1.0)
        if speed >= 0:
            self.set_speed(speed, 0.0)
        else:
            self.set_speed(0.0, speed)

    @wrap_error_as(MotorError, "Wheel stop failed", catch=OSError)
    def stop(self):
        """Stop both motors, attempting motor 2 even if motor 1 fails."""
        try:
            self._left.stop()
        finally:
            self._right.stop()


# Kept only as an import alias for callers which used the old module name.
DCMotor = Motor
DCMotorError = MotorError
