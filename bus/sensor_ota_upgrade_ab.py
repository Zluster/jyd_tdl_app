"""SensorOta.upgrade_ab() example; writes matching A/B firmware images."""

if __package__:
    from .sensor_ota import SensorOta
else:
    from sensor_ota import SensorOta


with SensorOta("/dev/ttyS2", 115200, debug=True) as updater:
    updater.upgrade_ab(0x17, 1,
                       "Project_OTA_Slot_A.bin",
                       "Project_OTA_Slot_B.bin",
                       version=2,
                       progress=lambda done, total: print(f"{done}/{total}"))
