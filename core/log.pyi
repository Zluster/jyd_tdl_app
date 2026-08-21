# ruff: noqa: E402
"""Dara's unified logging interface."""

from enum import Enum

class LogLevel(str, Enum):
    """Severity levels supported by Dara logging."""
    DEBUG = ...
    """Debug logging level."""
    INFO = ...
    """Informational logging level."""
    WARN = ...
    """Warning logging level."""
    ERROR = ...
    """Error logging level."""


_LOGGER = ...
_LEVELS = ...
_CONSOLE_HANDLER = ...
def set_level(level: LogLevel) -> None:
    """Set the minimum level emitted by Dara's logger."""
    ...

def get_level() -> LogLevel:
    """Return Dara's current minimum logging level."""
    ...

def debug(msg: str, *args: object) -> None:
    """Log a debug message."""
    ...

def info(msg: str, *args: object) -> None:
    """Log an informational message."""
    ...

def warn(msg: str, *args: object) -> None:
    """Log a warning message."""
    ...

def error(msg: str, *args: object) -> None:
    """Log an error message."""
    ...

def exception(msg: str, *args: object) -> None:
    """Log an error message with the current exception traceback."""
    ...
