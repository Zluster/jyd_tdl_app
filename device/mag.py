"""Magnetometer data types shared by concrete sensor drivers."""



class MagError(OSError):
    """Raised when a magnetometer operation cannot be completed."""


class MagData:
    """Magnetic-field data from a magnetometer capability read."""

    """Magnetic field along each axis in microteslas."""

    def __init__(self, mag):
        """Initialize a magnetic-field sample."""
        self.mag = mag
