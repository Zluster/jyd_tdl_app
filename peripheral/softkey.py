"""Long-press button access through configured GPIO or ADC inputs."""


from enum import Enum
from threading import Event, Lock, Thread, current_thread
from time import monotonic, monotonic_ns

import dara.core.log as log
from dara.core.common import DaraEvent

from .adc import ADC
from .gpio import GPIO, GPIODirection, GPIOEdge
from .pinmap import PinMap


class SoftKeyError(OSError):
    """Raised when a soft-key operation cannot be completed."""


class SoftKeyEvent(DaraEvent):
    """A button release classified by its press duration."""

    class Type(str, Enum):
        """Button press classifications reported on release."""

        NORMAL_PRESS = "normal_press"
        LONG_PRESS = "long_press"

    def __init__(self, button, type, timestamp):
        """Initialize a classified button release event."""
        self.button = button
        self.type = type
        self.timestamp = timestamp


class SoftKey:
    """A button whose GPIO or ADC state is maintained by a background thread."""

    _ADC_POLL_INTERVAL = 0.01

    def __init__(self, id, long_press_ms = 2000):
        """Create and open a button input with an optional long-press duration."""
        if (
            long_press_ms is not None
            and (isinstance(long_press_ms, bool) or not isinstance(long_press_ms, int))
        ):
            raise ValueError("long_press_ms must be an integer or None")
        if long_press_ms is not None and long_press_ms < 0:
            raise ValueError("long_press_ms must be non-negative")

        self.id = id
        self._long_press_ms = long_press_ms
        self._callback = None
        self._down_at = None
        self._pressed = False
        self._long_pressed = False
        self._lock = Lock()
        self._stop = Event()
        self._thread = None
        self._gpio = None
        self._adc = None
        try:
            self.info = PinMap.get_softkey(id)
        except ValueError:
            self.info = None
            self._gpio = GPIO(
                id, GPIODirection.IN, edge=GPIOEdge.BOTH, auto_open=False
            )
        else:
            if self.info.gpio is not None:
                self._gpio = GPIO(
                    self.info.gpio,
                    GPIODirection.IN,
                    edge=GPIOEdge.BOTH,
                    auto_open=False,
                )
            else:
                assert self.info.adc is not None
                self._adc = ADC(self.info.adc, auto_open=False)
        self.open()

    def open(self):
        """Open the input, reset button state, and start background monitoring."""
        self.close()
        if self.info is not None and self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise SoftKeyError("soft-key init command failed") from error
            if return_code:
                raise SoftKeyError(
                    f"soft-key init command failed with exit status {return_code}"
                )
        if self._gpio is not None:
            self._gpio.open()
        else:
            assert self._adc is not None
            self._adc.open()
        with self._lock:
            self._reset_state()
        self._start_watching()

    def close(self):
        """Stop monitoring, close the input, and discard pending button state."""
        self._stop_watching()
        try:
            if self._gpio is not None:
                self._gpio.close()
            else:
                assert self._adc is not None
                self._adc.close()
        finally:
            with self._lock:
                self._reset_state()

    def __enter__(self):
        """Open the button if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the button when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the configured button input is open."""
        if self._gpio is not None:
            return self._gpio.is_opened
        assert self._adc is not None
        return self._adc.is_opened

    @property
    def long_press_ms(self):
        """The long-press duration in milliseconds, or ``None`` when disabled."""
        with self._lock:
            return self._long_press_ms

    @long_press_ms.setter
    def long_press_ms(self, ms):
        """Set the long-press duration in milliseconds, or disable it with ``None``."""
        if ms is None:
            with self._lock:
                self._long_press_ms = None
            return
        if isinstance(ms, bool) or not isinstance(ms, int) or ms < 0:
            raise ValueError("ms must be a non-negative integer")
        with self._lock:
            self._long_press_ms = ms

    def is_down(self):
        """Return the current down state maintained by the monitor thread."""
        with self._lock:
            return self._down_at is not None

    def is_long_down(self):
        """Return whether the monitored down state has reached the long threshold."""
        with self._lock:
            return (
                self._long_press_ms is not None
                and self._down_at is not None
                and (monotonic() - self._down_at) * 1000 >= self._long_press_ms
            )

    def is_pressed(self):
        """Consume the normal-press state set by the monitor thread."""
        with self._lock:
            pressed, self._pressed = self._pressed, False
            return pressed

    def is_long_pressed(self):
        """Consume the long-press state set by the monitor thread."""
        with self._lock:
            pressed, self._long_pressed = self._long_pressed, False
            return pressed

    def set_callback(self, cb):
        """Set or remove the callback without changing background monitoring."""
        if cb is not None and not callable(cb):
            raise ValueError("cb must be callable or None")
        with self._lock:
            self._callback = cb

    def _start_watching(self):
        """Start background monitoring when it is not already running."""
        if self._thread is None or not self._thread.is_alive():
            self._stop.clear()
            self._thread = Thread(target=self._watch, daemon=True)
            self._thread.start()

    def _stop_watching(self):
        """Stop background monitoring without closing the input."""
        self._stop.set()
        if self._thread is not None and self._thread is not current_thread():
            self._thread.join()
        self._thread = None

    def _watch(self):
        """Maintain button state from the configured input source."""
        if self._adc is not None:
            self._watch_adc()
        else:
            self._watch_gpio()

    def _watch_gpio(self):
        """Translate GPIO edge events into key state changes."""
        assert self._gpio is not None
        while not self._stop.is_set() and self.is_opened:
            if not self._gpio.poll(0.1):
                continue
            event = self._gpio.read_event()
            if event.edge is GPIOEdge.FALLING:
                self._begin_press()
                continue
            if event.edge is not GPIOEdge.RISING:
                continue
            self._finish_press(event.timestamp)

    def _watch_adc(self):
        """Poll ADC raw values and translate matching transitions into key events."""
        assert self._adc is not None
        assert self.info is not None
        was_down = False
        while not self._stop.is_set() and self.is_opened:
            raw = self._adc.read_raw()
            is_down = any(
                abs(raw - target) <= self.info.raw_tolerance
                for target in self.info.raw_values
            )
            if is_down and not was_down:
                self._begin_press()
            elif was_down and not is_down:
                self._finish_press(monotonic_ns())
            was_down = is_down
            self._stop.wait(self._ADC_POLL_INTERVAL)

    def _begin_press(self):
        """Start timing a press and clear pending release states."""
        with self._lock:
            self._down_at = monotonic()
            self._pressed = False
            self._long_pressed = False

    def _finish_press(self, timestamp):
        """Classify a release, update pending state, and dispatch its callback."""
        with self._lock:
            if self._down_at is None:
                return
            duration_ms = (monotonic() - self._down_at) * 1000
            is_long = (
                self._long_press_ms is not None
                and duration_ms >= self._long_press_ms
            )
            press_type = (
                SoftKeyEvent.Type.LONG_PRESS
                if is_long
                else SoftKeyEvent.Type.NORMAL_PRESS
            )
            self._down_at = None
            self._long_pressed = is_long
            self._pressed = not is_long
            callback = self._callback
        if callback is not None:
            try:
                callback(SoftKeyEvent(self, press_type, timestamp))
            except Exception:
                log.exception("soft-key callback failed")

    def _reset_state(self):
        """Clear monitored and pending button state while holding ``_lock``."""
        self._down_at = None
        self._pressed = False
        self._long_pressed = False
