"""Private asyncio loop used by the synchronous Bluetooth API."""

from __future__ import annotations

import asyncio
from collections.abc import Coroutine
from concurrent.futures import TimeoutError
import threading
from typing import Any, TypeVar

from dbus_fast.aio import MessageBus
from dbus_fast.constants import BusType
from dbus_fast.errors import DBusError

from .errors import BluetoothError, BluetoothTimeoutError


_Result = TypeVar("_Result")
_loop: asyncio.AbstractEventLoop | None = None
_thread: threading.Thread | None = None
_bus: MessageBus | None = None


def _start(ready: threading.Event) -> None:
    """Run the Bluetooth event loop in its daemon thread."""
    global _loop
    _loop = asyncio.new_event_loop()
    ready.set()
    _loop.run_forever()
    _loop.close()


def _get_loop() -> asyncio.AbstractEventLoop:
    """Return the running loop, starting it when necessary."""
    global _thread
    if _loop is None or not _loop.is_running():
        ready = threading.Event()
        _thread = threading.Thread(
            target=_start,
            args=(ready,),
            name="dara-bluetooth",
            daemon=True,
        )
        _thread.start()
        ready.wait()
    assert _loop is not None
    return _loop


async def get_bus() -> MessageBus:
    """Return the shared system bus, connecting it lazily."""
    global _bus
    if _bus is None or not getattr(_bus, "connected", True):
        _bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    return _bus


def run(
    coroutine: Coroutine[Any, Any, _Result],
    timeout: float | None = 30.0,
) -> _Result:
    """Run a coroutine and expose only Dara-owned backend errors."""
    future = asyncio.run_coroutine_threadsafe(coroutine, _get_loop())
    try:
        return future.result(timeout)
    except TimeoutError as error:
        future.cancel()
        raise BluetoothTimeoutError("Bluetooth operation timed out") from error
    except BluetoothError:
        raise
    except (DBusError, OSError) as error:
        raise BluetoothError(str(error)) from error


def shutdown() -> None:
    """Disconnect the bus and stop the event loop."""
    global _bus, _loop, _thread
    if _loop is None:
        return
    if _bus is not None:
        _bus.disconnect()
        _bus = None
    _loop.call_soon_threadsafe(_loop.stop)
    if _thread is not threading.current_thread():
        assert _thread is not None
        _thread.join(1)
    _loop = None
    _thread = None
