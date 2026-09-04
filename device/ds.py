"""Distance data types shared by concrete sensor drivers."""



class DSError(OSError):
    """Raised when a distance sensor operation cannot be completed."""


class DSData:
    """Distance data from a distance sensor capability read."""

    """Distance in centimeters."""

    def __init__(self, distance):
        """Initialize a distance sample."""
        self.distance = distance
