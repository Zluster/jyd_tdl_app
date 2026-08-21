"""Dara's unified logging interface."""


from enum import Enum
import logging
import sys


class LogLevel(str, Enum):
    """Severity levels supported by Dara logging."""

    DEBUG = "DEBUG"
    """Debug logging level."""
    INFO = "INFO"
    """Informational logging level."""
    WARN = "WARN"
    """Warning logging level."""
    ERROR = "ERROR"
    """Error logging level."""

_LOGGER = logging.getLogger("dara")
_LEVELS = {
    LogLevel.DEBUG: logging.DEBUG,
    LogLevel.INFO: logging.INFO,
    LogLevel.WARN: logging.WARNING,
    LogLevel.ERROR: logging.ERROR,
}
_LOGGER.setLevel(_LEVELS[LogLevel.WARN])

_CONSOLE_HANDLER = logging.StreamHandler(sys.stdout)
_CONSOLE_HANDLER.setLevel(_LEVELS[LogLevel.WARN])
_CONSOLE_HANDLER.setFormatter(logging.Formatter(
    "[%(name)s] [%(asctime)s.%(msecs)03d] [%(levelname).1s]: %(message)s",
    datefmt="%H:%M:%S",
))

_LOGGER.addHandler(_CONSOLE_HANDLER)


def set_level(level):
    """Set the minimum level emitted by Dara's logger."""
    try:
        _LOGGER.setLevel(_LEVELS[level])
        _CONSOLE_HANDLER.setLevel(_LEVELS[level])
    except KeyError as error:
        raise ValueError(f"unsupported log level: {level!r}") from error


def get_level():
    """Return Dara's current minimum logging level."""
    return next(level for level, value in _LEVELS.items() if value == _LOGGER.level)


def debug(msg, *args):
    """Log a debug message."""
    _LOGGER.debug(msg, *args)


def info(msg, *args):
    """Log an informational message."""
    _LOGGER.info(msg, *args)


def warn(msg, *args):
    """Log a warning message."""
    _LOGGER.warning(msg, *args)


def error(msg, *args):
    """Log an error message."""
    _LOGGER.error(msg, *args)


def exception(msg, *args):
    """Log an error message with the current exception traceback."""
    _LOGGER.exception(msg, *args)
