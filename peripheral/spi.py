"""SPI access to buses declared in the board pin map."""



from ._periphery.spi import SPI as _PeripherySPI
from ._periphery.spi import SPIError as _PeripherySPIError

from dara.core._error_helpers import wrap_error_as

from .gpio import GPIO, GPIODirection
from .pinmap import PinMap

_SPI_NO_CS = 0x40


class SPIError(OSError):
    """Raised when an SPI operation cannot be completed."""


class SPI:
    """An SPI bus selected by its bus and chip-select identifiers.

    ``SPI(0, cs=0)`` selects ``SPI0.0``. With ``soft_cs=True``, ``cs`` is a
    GPIO identifier driven around each transfer.
    """


    def __init__(
        self,
        id,
        freq = 1_000_000,
        polarity = 0,
        phase = 0,
        bits = 8,
        cs = None,
        soft_cs = False,
        *,
        auto_open = True,
    ):
        """Create an SPI bus and optionally open its mapped device.

        When ``soft_cs`` is false, ``cs`` selects the configured hardware
        chip-select entry. When true, ``cs`` selects a GPIO output.
        """
        if isinstance(freq, bool) or not isinstance(freq, int) or freq <= 0:
            raise ValueError("freq must be a positive integer")
        if polarity not in (0, 1) or phase not in (0, 1):
            raise ValueError("polarity and phase must be 0 or 1")
        if not isinstance(bits, int) or not 1 <= bits <= 32:
            raise ValueError("bits must be an integer from 1 through 32")
        if not isinstance(soft_cs, bool):
            raise ValueError("soft_cs must be a bool")
        if soft_cs and cs is None:
            raise ValueError("cs is required when soft_cs is true")

        self.id = id
        self.info = PinMap.get_spi(id, None if soft_cs else cs)
        self._freq = freq
        self._mode = polarity << 1 | phase
        self._bits = bits
        self._periphery_instance = None
        self._cs_gpio = None
        if soft_cs:
            assert cs is not None
            self._cs_gpio = GPIO(cs, GPIODirection.OUT, auto_open=False)
        if auto_open:
            self.open()

    @wrap_error_as(SPIError, "SPI open failed", catch=_PeripherySPIError)
    def open(self):
        """Open or re-open the mapped SPI device."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise SPIError("SPI init command failed") from error
            if return_code:
                raise SPIError(
                    f"SPI init command failed with exit status {return_code}"
                )
        try:
            if self.info.sclk is not None and self.info.sclk_func is not None:
                PinMap.set_pin_function(self.info.sclk, self.info.sclk_func)
            if self.info.mosi is not None and self.info.mosi_func is not None:
                PinMap.set_pin_function(self.info.mosi, self.info.mosi_func)
            if self.info.miso is not None and self.info.miso_func is not None:
                PinMap.set_pin_function(self.info.miso, self.info.miso_func)
            if self.info.cs is not None and self.info.cs_func is not None:
                PinMap.set_pin_function(self.info.cs, self.info.cs_func)
            self._periphery_instance = _PeripherySPI(
                self.info.dev,
                self._mode,
                self._freq,
                bits_per_word=self._bits,
                extra_flags=_SPI_NO_CS if self._cs_gpio is not None else 0,
            )
            if self._cs_gpio is not None:
                self._cs_gpio.open()
                self._cs_gpio.high()
        except Exception:
            self.close()
            raise

    @wrap_error_as(SPIError, "SPI close failed", catch=_PeripherySPIError)
    def close(self):
        """Close the mapped SPI device."""
        try:
            if self._cs_gpio is not None and self._cs_gpio.is_opened:
                try:
                    self._cs_gpio.high()
                finally:
                    self._cs_gpio.close()
        finally:
            if self._periphery_instance is not None:
                self._backend.close()
                self._periphery_instance = None

    def __enter__(self):
        """Open the SPI bus if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the SPI bus when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the mapped SPI device is open."""
        return self._periphery_instance is not None

    @wrap_error_as(SPIError, "SPI freq failed", catch=_PeripherySPIError)
    def get_freq(self):
        """Return the SPI clock frequency in Hertz."""
        return self._backend.max_speed

    @wrap_error_as(SPIError, "SPI freq failed", catch=_PeripherySPIError)
    def set_freq(self, value):
        """Set the SPI clock frequency in Hertz."""
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError("freq must be a positive integer")
        self._backend.max_speed = value

    @wrap_error_as(SPIError, "SPI mode failed", catch=_PeripherySPIError)
    def get_mode(self):
        """Return the SPI clock polarity and phase."""
        mode = self._backend.mode
        return mode >> 1, mode & 1

    @wrap_error_as(SPIError, "SPI mode failed", catch=_PeripherySPIError)
    def set_mode(self, polarity, phase):
        """Set the SPI clock polarity and phase, each to 0 or 1."""
        if polarity not in (0, 1) or phase not in (0, 1):
            raise ValueError("polarity and phase must be 0 or 1")
        self._backend.mode = polarity << 1 | phase

    def read(self, nbytes):
        """Read ``nbytes`` from the SPI device."""
        if not isinstance(nbytes, int) or nbytes < 0:
            raise ValueError("nbytes must be a non-negative integer")
        return self._transfer(bytes(nbytes))

    def write(self, data):
        """Write a byte buffer to the SPI device."""
        self._transfer(data)

    def write_read(self, data):
        """Transmit bytes and return the simultaneously received bytes."""
        return self._transfer(data)

    @wrap_error_as(SPIError, "SPI _transfer failed", catch=_PeripherySPIError)
    def _transfer(self, data):
        """Transfer bytes through the open SPI backend."""
        if self._cs_gpio is None:
            return bytes(self._backend.transfer(data))
        self._cs_gpio.low()
        try:
            return bytes(self._backend.transfer(data))
        finally:
            self._cs_gpio.high()

    @property
    def _backend(self):
        """Return the open SPI backend."""
        if self._periphery_instance is None:
            raise SPIError("SPI bus is not open")
        return self._periphery_instance
