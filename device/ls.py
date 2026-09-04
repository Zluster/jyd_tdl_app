"""Light-sensor data types shared by concrete sensor drivers."""



class LSError(OSError):
    """Raised when a light sensor operation cannot be completed."""


class LSData:
    """Brightness data from a light sensor capability read."""

    def __init__(self, brightness):
        """Initialize a brightness sample."""
        self.brightness = brightness
