# 示例程序

在 `linux_uart_Python` 目录运行：

```bash
python3 examples/example_dual_uart.py --device /dev/ttyS2 --baud 115200
python3 examples/api_call_example.py --device /dev/ttyS2 --type 0x03 --number 1
python3 examples/jydbus_classes_example.py --device /dev/ttyS2 --number 1
python3 examples/paj7620_example.py --device /dev/ttyS2 --number 1
python3 examples/zw101_api_example.py
```

这些示例需要 Linux、可用的 UART 和对应硬件；同一时刻只能有一个程序占用串口。
