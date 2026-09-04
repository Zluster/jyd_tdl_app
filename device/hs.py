"""Humidity data types shared by concrete sensor drivers."""



class HSError(OSError):
    """Raised when a humidity sensor operation cannot be completed."""


class HSData:
    """Relative humidity data from a humidity sensor capability read."""

    """Relative humidity as a percentage."""

    def __init__(self, humidity):
        """Initialize a relative humidity sample."""
        self.humidity = humidity
