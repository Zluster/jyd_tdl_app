# 测试说明

无硬件回归测试：

```bash
python3 -m unittest discover -s tests -p "test_*.py" -v
```

测试覆盖协议组帧、CRC、15 种设备类型、传感器专用方法、WS2812B 分块、
ZW101 解码和 OTA 镜像槽位校验。

MFRC522 硬件调试：

```bash
python3 tests/mfrc522_debug.py --scan
python3 tests/mfrc522_debug.py --number 1
python3 tests/mfrc522_debug.py --number 1 --listen
```

硬件调试默认使用 `/dev/ttyS2`。运行前关闭其他串口程序。

全部在线传感器接口测试：

```bash
python3 tests/hardware_interface_test.py --device /dev/ttyS2
```

默认只查询数据。自动上传、灯板、指纹和 OTA 状态测试示例：

```bash
python3 tests/hardware_interface_test.py --auto-upload
python3 tests/hardware_interface_test.py --sensor 0x03:1 --ws2812b-node 1
python3 tests/hardware_interface_test.py --fingerprint-match-node 1
python3 tests/hardware_interface_test.py --ota-status 0x15:1
```
