# 示例程序

在项目目录运行：

```bash
python3 examples/example_dual_uart.py --device /dev/ttyS2 --baud 115200
python3 examples/paj7620_example.py --device /dev/ttyS2 --number 1
python3 zw101_cli.py /dev/ttyS2 match
```

这些示例需要 Linux、可用 UART 和对应硬件。同一时刻只能有一个程序占用串口。
