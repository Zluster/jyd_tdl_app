# ruff: noqa: E402
"""Board pin and peripheral configuration loading."""

import os

class PinInfo:
    """Pin-multiplexing register details loaded from a board configuration."""
    name: str
    reg_addr: int
    bit_offset: int
    bit_width: int
    func_regs: tuple[int, ...]
    funcs: tuple[str, ...]
    def __init__(self, name: str, reg_addr: int, bit_offset: int, bit_width: int, func_regs: tuple[int, ...], funcs: tuple[str, ...]) -> None:
        """Initialize pin-multiplexing register details."""
        ...



class UARTInfo:
    """Device and pin assignments for a configured UART."""
    dev: str
    tx: str | None
    rx: str | None
    tx_func: str | None
    rx_func: str | None
    init_cmd: str | None = ...
    """The shell command run before opening the UART, or ``None``."""
    def __init__(self, dev: str, tx: str | None, rx: str | None, tx_func: str | None, rx_func: str | None, init_cmd: str | None = ...) -> None:
        """Initialize a UART definition."""
        ...



class I2CInfo:
    """Device and pin assignments for a configured I2C bus."""
    dev: str
    scl: str | None
    sda: str | None
    scl_func: str | None
    sda_func: str | None
    init_cmd: str | None = ...
    """The shell command run before opening the I2C bus, or ``None``."""
    def __init__(self, dev: str, scl: str | None, sda: str | None, scl_func: str | None, sda_func: str | None, init_cmd: str | None = ...) -> None:
        """Initialize an I2C definition."""
        ...



class SPIInfo:
    """Device and pin assignments for a configured SPI bus."""
    dev: str
    sclk: str | None
    mosi: str | None
    miso: str | None
    cs: str | None
    sclk_func: str | None
    mosi_func: str | None
    miso_func: str | None
    cs_func: str | None
    init_cmd: str | None = ...
    """The shell command run before opening the SPI bus, or ``None``."""
    def __init__(self, dev: str, sclk: str | None, mosi: str | None, miso: str | None, cs: str | None, sclk_func: str | None, mosi_func: str | None, miso_func: str | None, cs_func: str | None, init_cmd: str | None = ...) -> None:
        """Initialize an SPI definition."""
        ...



class GPIOInfo:
    """Chip and line assignments for a configured GPIO."""
    pin: str | None
    pin_func: str | None
    chip: int
    num: int
    init_cmd: str | None = ...
    """The shell command run before opening the GPIO line, or ``None``."""
    def __init__(self, pin: str | None, pin_func: str | None, chip: int, num: int, init_cmd: str | None = ...) -> None:
        """Initialize a GPIO definition."""
        ...



class SoftKeyInfo:
    """GPIO or ADC assignment for a configured soft key."""
    gpio: str | None
    """The configured GPIO identifier, or ``None`` for an ADC-backed key."""
    init_cmd: str | None = ...
    """The shell command run before opening the soft key, or ``None``."""
    adc: str | None = ...
    """The configured ADC identifier, or ``None`` for a GPIO-backed key."""
    raw_values: tuple[int, ...] = ...
    """ADC raw values that represent the key's down state."""
    raw_tolerance: int = ...
    """The inclusive tolerance applied around each configured ADC raw value."""
    def __init__(self, gpio: str | None, init_cmd: str | None = ..., adc: str | None = ..., raw_values: tuple[int, ...] = ..., raw_tolerance: int = ...) -> None:
        """Initialize a soft-key definition."""
        ...



class PWMInfo:
    """Chip and channel assignments for a configured PWM output."""
    pin: str | None
    pin_func: str | None
    chip: int
    num: int
    freq: int | float
    duty_cycle: float
    enable: bool
    init_cmd: str | None = ...
    """The shell command run before opening the PWM output, or ``None``."""
    def __init__(self, pin: str | None, pin_func: str | None, chip: int, num: int, freq: int | float, duty_cycle: float, enable: bool, init_cmd: str | None = ...) -> None:
        """Initialize a PWM definition."""
        ...



class ADCInfo:
    """Pin assignment, sysfs path, and defaults for a configured ADC input."""
    pin: str | None
    pin_func: str | None
    sysfs: str
    resolution: int
    vref: int | float
    init_cmd: str | None = ...
    """The shell command run before opening the ADC input, or ``None``."""
    def __init__(self, pin: str | None, pin_func: str | None, sysfs: str, resolution: int, vref: int | float, init_cmd: str | None = ...) -> None:
        """Initialize an ADC definition."""
        ...



class WDTInfo:
    """Configuration for the watchdog timer."""
    init_cmd: str | None = ...
    """The shell command run before opening the watchdog, or ``None``."""
    def __init__(self, init_cmd: str | None = ...) -> None:
        """Initialize a watchdog definition."""
        ...



class BoardInfo:
    """Descriptive metadata for a board configuration."""
    id: str
    """The stable board identifier."""
    name: str
    """The human-readable board name."""
    description: str
    """A concise description of the board."""
    platform: str
    """The platform family implemented by the board."""
    board_version: str
    """The board hardware or configuration version."""
    def __init__(self, id: str, name: str, description: str, platform: str, board_version: str) -> None:
        """Initialize descriptive board metadata."""
        ...



class _Default:
    """Private marker for omitted pin-map constructor arguments."""
    ...


_DEFAULT = ...
class PinMapConfig:
    """Strongly typed pin and peripheral definitions for a board."""
    pins: dict[str, PinInfo]
    """Pin-multiplexing definitions keyed by pin identifier."""
    adc: dict[str, ADCInfo]
    """ADC definitions keyed by peripheral identifier."""
    softkey: dict[str, SoftKeyInfo]
    """Soft-key definitions keyed by key identifier."""
    gpio: dict[str, GPIOInfo]
    """GPIO definitions keyed by peripheral identifier."""
    i2c: dict[str, I2CInfo]
    """I2C definitions keyed by peripheral identifier."""
    pwm: dict[str, PWMInfo]
    """PWM definitions keyed by peripheral identifier."""
    spi: dict[str, SPIInfo]
    """SPI definitions keyed by peripheral identifier."""
    uart: dict[str, UARTInfo]
    """UART definitions keyed by peripheral identifier."""
    wdt: WDTInfo
    """The watchdog timer definition."""
    def __init__(self, pins: dict[str, PinInfo] | _Default = ..., adc: dict[str, ADCInfo] | _Default = ..., softkey: dict[str, SoftKeyInfo] | _Default = ..., gpio: dict[str, GPIOInfo] | _Default = ..., i2c: dict[str, I2CInfo] | _Default = ..., pwm: dict[str, PWMInfo] | _Default = ..., spi: dict[str, SPIInfo] | _Default = ..., uart: dict[str, UARTInfo] | _Default = ..., wdt: WDTInfo | _Default = ...) -> None:
        """Initialize pin-map sections with independent omitted defaults."""
        ...



class BoardConfig:
    """Versioned board metadata and pin-map configuration."""
    config_version: int
    """The board configuration schema version."""
    board: BoardInfo
    """The board's descriptive metadata."""
    pinmap: PinMapConfig
    """The board's pin and peripheral definitions."""
    def __init__(self, config_version: int, board: BoardInfo, pinmap: PinMapConfig) -> None:
        """Initialize a versioned board configuration."""
        ...



class PinMap:
    """The pin and peripheral definitions for the active board configuration."""
    pins: dict[str, PinInfo] = ...
    adc: dict[str, ADCInfo] = ...
    softkey: dict[str, SoftKeyInfo] = ...
    gpio: dict[str, GPIOInfo] = ...
    i2c: dict[str, I2CInfo] = ...
    pwm: dict[str, PWMInfo] = ...
    spi: dict[str, SPIInfo] = ...
    uart: dict[str, UARTInfo] = ...
    wdt: WDTInfo = ...
    _initialized = ...
    @classmethod
    def init(cls, board_cfg: str | os.PathLike[str] | None = ...) -> None:
        """Load a board configuration and replace the active pin map.

        The selected ``.py`` file is trusted executable code and must export a
        :class:`BoardConfig` as ``BOARD_CONFIG``. An explicitly supplied path
        takes precedence. Otherwise, look beside the entry-point script, then
        use ``/etc/board.py``. Active maps change only after full validation.
        Section dictionaries are copied shallowly, so entry and watchdog
        mutations remain visible and are not automatically revalidated.
        """
        ...

    @classmethod
    def get_uart(cls, identifier: int | str) -> UARTInfo:
        """Return the definition for an integer or named UART identifier."""
        ...

    @classmethod
    def get_adc(cls, identifier: int | str) -> ADCInfo:
        """Return the definition for an integer or named ADC identifier."""
        ...

    @classmethod
    def get_wdt(cls) -> WDTInfo:
        """Return the watchdog configuration."""
        ...

    @classmethod
    def get_pins(cls) -> tuple[str, ...]:
        """Return the configured pin identifiers."""
        ...

    @classmethod
    def get_pin_functions(cls, pin: str) -> tuple[str, ...]:
        """Return the functions supported by a configured pin."""
        ...

    @classmethod
    def set_pin_function(cls, pin: str, function: str) -> None:
        """Select a configured function for a pin."""
        ...

    @classmethod
    def get_gpio(cls, identifier: int | str) -> GPIOInfo:
        """Return the definition for an integer or named GPIO identifier."""
        ...

    @classmethod
    def get_softkey(cls, identifier: int | str) -> SoftKeyInfo:
        """Return the definition for a named softkey identifier."""
        ...

    @classmethod
    def get_pwm(cls, identifier: int | str) -> PWMInfo:
        """Return the definition for an integer or named PWM identifier."""
        ...

    @classmethod
    def get_i2c(cls, identifier: int | str) -> I2CInfo:
        """Return the definition for an integer or named I2C identifier."""
        ...

    @classmethod
    def get_spi(cls, identifier: int | str, cs: int | str | None = ...) -> SPIInfo:
        """Return the definition for an SPI bus and optional chip select."""
        ...
    @staticmethod
    def _board_config_path(board_cfg: str | os.PathLike[str] | None) -> str: ...
    @staticmethod
    def _load_board_config(board_cfg_path: str) -> BoardConfig: ...
    @staticmethod
    def _config_error(board_cfg_path: str, message: str) -> ValueError: ...
    @classmethod
    def _validate_board_config(cls, config: BoardConfig, board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_mapping(cls, name: str, mapping: object, value_type: type[object], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_init_cmd(cls, label: str, value: object, board_cfg_path: str) -> None: ...
    @classmethod
    def _get_pin(cls, pin: str) -> PinInfo: ...
    @staticmethod
    def _valid_pin_function(pin: object, function: object, pins: dict[str, PinInfo]) -> bool: ...
    @staticmethod
    def _validate_pins(value: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_adc(cls, value: dict[str, ADCInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_gpio(cls, value: dict[str, GPIOInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_softkeys(cls, value: dict[str, SoftKeyInfo], gpio: dict[str, GPIOInfo], adc: dict[str, ADCInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_pwm(cls, value: dict[str, PWMInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_i2c(cls, value: dict[str, I2CInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_spi(cls, value: dict[str, SPIInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @classmethod
    def _validate_uarts(cls, value: dict[str, UARTInfo], pins: dict[str, PinInfo], board_cfg_path: str) -> None: ...
    @staticmethod
    def _uart_name(identifier: int | str) -> str: ...
    @staticmethod
    def _adc_name(identifier: int | str) -> str: ...
    @staticmethod
    def _i2c_name(identifier: int | str) -> str: ...
    @staticmethod
    def _spi_name(identifier: int | str, cs: int | str | None) -> str: ...
    @staticmethod
    def _gpio_name(identifier: int | str) -> str: ...
    @staticmethod
    def _pwm_name(identifier: int | str) -> str: ...
