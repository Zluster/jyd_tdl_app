"""Temperature data types shared by concrete sensor drivers."""



class TSError(OSError):
    """Raised when a temperature sensor operation cannot be completed."""


class TSData:
    """Temperature data from a temperature sensor capability read."""

    def __init__(self, temperature):
        """Initialize a temperature sample."""
        self.temperature = temperature
