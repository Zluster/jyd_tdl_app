# 测试说明

在 `linux_uart_Python` 目录执行。

## 无硬件测试

验证协议组帧、CRC、数据解析、15 种 Jydbus 类、WS2812B 分块和 OTA 镜像：

```bash
python3 -m unittest discover -s tests -p "test_*.py" -v
```

也可以直接执行：

```bash
python3 tests/test_protocol.py
```

## 硬件 API 测试

测试前确认 `/dev/ttyS2` 未被其他程序占用，板端应用正常运行，并按实际硬件修改
`api_simple_test.py` 顶部的 `SENSOR_TYPE` 和 `SENSOR_NUMBER`：

```bash
python3 tests/api_simple_test.py
```

该测试会扫描节点、开启自动上传、查询和读取数据、将 1 号 WS2812B 设置为红色，
监听 10 秒后关闭自动上传。

## 专项硬件调试

扫描总线并打印完整扫描结果：

```bash
python3 tests/scan_debug.py
```

MFRC522 调试：

```bash
python3 tests/mfrc522_debug.py --scan
python3 tests/mfrc522_debug.py --number 1 --once
python3 tests/mfrc522_debug.py --number 1 --listen
```

这些脚本会访问 `/dev/ttyS2`，运行前必须关闭其他串口程序。
