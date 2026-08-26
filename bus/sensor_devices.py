"""Compatibility imports; new code should use jydbus_devices."""

from jydbus_devices import *  # noqa: F401,F403
from jydbus_devices import (JYDBUS_CLASS_BY_TYPE, JydbusAHT10,
                            JydbusBMP390, JydbusButtonPB1, JydbusDevice,
                            JydbusJoystick, JydbusKnobSwitchADC,
                            JydbusMAX30102, JydbusMFRC522, JydbusPAJ7620U2,
                            JydbusPhotoresistorADC, JydbusSoilMoistureADC,
                            JydbusVL53L0X, JydbusWS2812B,
                            JydbusWaterLevelADC, JydbusZSPD4003,
                            JydbusZW101, create_jydbus)

SensorDevice = JydbusDevice
AHT10Sensor = JydbusAHT10
BMP390Sensor = JydbusBMP390
MAX30102Sensor = JydbusMAX30102
VL53L0XSensor = JydbusVL53L0X
MFRC522Sensor = JydbusMFRC522
WS2812BSensor = JydbusWS2812B
ZW101Sensor = JydbusZW101
ButtonPB1Sensor = JydbusButtonPB1
JoystickSensor = JydbusJoystick
PhotoresistorADCSensor = JydbusPhotoresistorADC
WaterLevelADCSensor = JydbusWaterLevelADC
SoilMoistureADCSensor = JydbusSoilMoistureADC
ZSPD4003Sensor = JydbusZSPD4003
KnobSwitchADCSensor = JydbusKnobSwitchADC
PAJ7620U2Sensor = JydbusPAJ7620U2
SENSOR_CLASS_BY_TYPE = JYDBUS_CLASS_BY_TYPE
create_sensor = create_jydbus
