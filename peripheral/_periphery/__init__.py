import time

__version__ = "2.4.1"
"Module version string."

version = (2, 4, 1)
"Module version tuple."


def sleep(seconds):
    """Sleep for the specified number of seconds.

    Args:
        seconds (int, long, float): duration in seconds.

    """
    time.sleep(seconds)


def sleep_ms(milliseconds):
    """Sleep for the specified number of milliseconds.

    Args:
        milliseconds (int, long, float): duration in milliseconds.

    """
    time.sleep(milliseconds / 1000.0)


def sleep_us(microseconds):
    """Sleep for the specified number of microseconds.

    Args:
        microseconds (int, long, float): duration in microseconds.

    """
    time.sleep(microseconds / 1000000.0)
