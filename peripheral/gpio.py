"""GPIO access to pins declared in the board pin map."""


from enum import Enum

from ._periphery.gpio import GPIO as _PeripheryGPIO
from ._periphery.gpio import GPIOError as _PeripheryGPIOError

from dara.core._error_helpers import wrap_error_as
from dara.core.common import DaraEvent

from .pinmap import PinMap


class GPIOError(OSError):
    """Raised when a GPIO operation cannot be completed."""


class GPIODirection(str, Enum):
    """Directions supported by a GPIO pin."""

    IN = "in"
    OUT = "out"
    HIGH = "high"
    LOW = "low"


class GPIOEdge(str, Enum):
    """Input edge events supported by a GPIO pin."""

    NONE = "none"
    RISING = "rising"
    FALLING = "falling"
    BOTH = "both"


class GPIOPull(str, Enum):
    """Input bias settings supported by a GPIO pin."""

    DEFAULT = "default"
    UP = "pull_up"
    DOWN = "pull_down"
    DISABLE = "disable"


class GPIODrive(str, Enum):
    """Output drive modes supported by a GPIO pin."""

    DEFAULT = "default"
    OPEN_DRAIN = "open_drain"
    OPEN_SOURCE = "open_source"


class GPIO:
    """A GPIO pin selected by its board configuration identifier."""


    def __init__(
        self,
        id,
        direction = GPIODirection.IN,
        pull = GPIOPull.DEFAULT,
        edge = GPIOEdge.NONE,
        drive = GPIODrive.DEFAULT,
        inverted = False,
        *,
        auto_open = True,
    ):
        """Create a GPIO pin and optionally open its mapped line."""
        self.id = id
        self.info = PinMap.get_gpio(id)
        self._periphery_instance = None
        self.reset(direction, pull, edge, drive, inverted)
        if auto_open:
            self.open()

    def reset(
        self,
        direction = GPIODirection.IN,
        pull = GPIOPull.DEFAULT,
        edge = GPIOEdge.NONE,
        drive = GPIODrive.DEFAULT,
        inverted = False,
    ):
        """Set the GPIO direction, pull, edge, drive, and active-low configuration."""
        if not isinstance(direction, GPIODirection):
            raise ValueError("direction must be a GPIODirection value")
        if not isinstance(pull, GPIOPull):
            raise ValueError("pull must be a GPIOPull value")
        if not isinstance(edge, GPIOEdge):
            raise ValueError("edge must be a GPIOEdge value")
        if not isinstance(drive, GPIODrive):
            raise ValueError("drive must be a GPIODrive value")
        if not isinstance(inverted, bool):
            raise ValueError("inverted must be a bool")
        self._direction = direction
        self._pull = pull
        self._edge = edge
        self._drive = drive
        self._inverted = inverted
        if self.is_opened:
            self.open()

    @property
    def direction(self):
        """The configured GPIO direction or initial output level."""
        return self._direction

    @property
    def pull(self):
        """The configured GPIO input bias."""
        return self._pull

    @property
    def edge(self):
        """The configured GPIO input edge detection mode."""
        return self._edge

    @property
    def drive(self):
        """The configured GPIO output drive mode."""
        return self._drive

    @property
    def inverted(self):
        """Whether GPIO values use active-low logic."""
        return self._inverted

    @wrap_error_as(GPIOError, "GPIO open failed", catch=_PeripheryGPIOError)
    def open(self):
        """Open or re-open the mapped GPIO line."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise GPIOError("GPIO init command failed") from error
            if return_code:
                raise GPIOError(
                    f"GPIO init command failed with exit status {return_code}"
                )
        if self.info.pin is not None and self.info.pin_func is not None:
            PinMap.set_pin_function(self.info.pin, self.info.pin_func)
        self._periphery_instance = _PeripheryGPIO(
            f"/dev/gpiochip{self.info.chip}",
            self.info.num,
            self._direction.value,
            edge=self._edge.value,
            bias=self._pull.value,
            drive=self._drive.value,
            inverted=self._inverted,
        )

    @wrap_error_as(GPIOError, "GPIO close failed", catch=_PeripheryGPIOError)
    def close(self):
        """Close the mapped GPIO line."""
        if self._periphery_instance is not None:
            self._periphery_instance.close()
            self._periphery_instance = None

    def __enter__(self):
        """Open the GPIO if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the GPIO when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the mapped GPIO line is open."""
        return self._periphery_instance is not None

    @wrap_error_as(GPIOError, "GPIO read failed", catch=_PeripheryGPIOError)
    def read(self):
        """Return the current GPIO state."""
        return self._backend.read()

    @wrap_error_as(GPIOError, "GPIO write failed", catch=_PeripheryGPIOError)
    def write(self, value):
        """Set the GPIO state to ``value``."""
        if not isinstance(value, bool):
            raise ValueError("value must be a bool")
        self._backend.write(value)

    @wrap_error_as(GPIOError, "GPIO poll failed", catch=_PeripheryGPIOError)
    def poll(self, timeout = None):
        """Wait for a configured edge and return whether one occurred.

        ``None`` and negative values wait indefinitely, ``0`` does not wait,
        and a positive value waits that many seconds.
        """
        return self._backend.poll(timeout)

    @wrap_error_as(GPIOError, "GPIO read_event failed", catch=_PeripheryGPIOError)
    def read_event(self):
        """Return the edge event found by :meth:`poll`.

        This is supported for GPIO character devices, including Dara's mapped
        ``/dev/gpiochip*`` lines.
        """
        event = self._backend.read_event()
        return PollEvent(GPIOEdge(event.edge), event.timestamp)  # pyright: ignore[reportAttributeAccessIssue]

    def high(self):
        """Set the GPIO output high."""
        self.write(True)

    def low(self):
        """Set the GPIO output low."""
        self.write(False)

    def toggle(self):
        """Invert the current GPIO output state."""
        self.write(not self.read())

    @property
    def _backend(self):
        """Return the open GPIO backend."""
        if self._periphery_instance is None:
            raise GPIOError("GPIO is not open")
        return self._periphery_instance


class PollEvent(DaraEvent):
    """An input edge event reported by :meth:`GPIO.poll`."""

    """The edge that occurred."""
    """The Linux event timestamp in nanoseconds."""

    def __init__(self, edge, timestamp):
        """Initialize an input edge event."""
        self.edge = edge
        self.timestamp = timestamp
