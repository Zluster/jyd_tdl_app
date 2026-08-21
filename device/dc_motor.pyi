# ruff: noqa: E402
"""Bidirectional DC motor control through GPIO and PWM outputs."""

class DCMotorError(OSError):
    """Raised when a DC motor operation cannot be completed."""
    ...


class DCMotor:
    """A DC motor driven by two direction pins and one PWM channel."""
    def __init__(self, in1: int | str, in2: int | str, pwm: int | str, *, auto_open: bool = ...) -> None:
        """Create a DC motor from board identifiers and optionally activate it."""
        ...

    @staticmethod
    def _validate_speed(speed: float, minimum: float, maximum: float) -> None: ...

    def open(self) -> None:
        """Open the motor outputs and stop the motor."""
        ...

    def close(self) -> None:
        """Stop the motor and close its GPIO and PWM outputs."""
        ...

    def __enter__(self) -> DCMotor:
        """Open the motor if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the motor when leaving a ``with`` statement."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether all motor outputs are open."""
        ...

    def stop(self) -> None:
        """Stop the motor and allow it to coast."""
        ...

    def brake(self) -> None:
        """Stop the motor using electrical braking."""
        ...

    def forward(self, speed: float) -> None:
        """Run forward at ``speed`` from 0.0 through 1.0."""
        ...

    def backward(self, speed: float) -> None:
        """Run backward at ``speed`` from 0.0 through 1.0."""
        ...

    def set_speed(self, speed: float) -> None:
        """Run at signed ``speed`` from -1.0 backward through 1.0 forward."""
        ...
