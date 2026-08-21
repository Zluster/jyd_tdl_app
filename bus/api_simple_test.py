#!/usr/bin/env python3
"""最简单的传感器 API 调用测试。"""

from api_call_example import (getValue, initialize, listenning, read,
                              scanning, setUploadMode, setValue)

DEVICE = "/dev/ttyS2"
SENSOR_TYPE = 0x03
SENSOR_NUMBER = 1
UPLOAD_INTERVAL_MS = 1000


def printData(data):
    print("bus:", data.sensor_type, data.sensor_number, data.value)


def main():
    # 1、模块调用：见文件顶部 import
    # 2、初始化
    api = initialize(DEVICE, 115200)

    try:
        # 3、扫描
        print("scanning:", scanning(api))

        # 4、开启自动上传，间隔 1000 ms
        setUploadMode(api, SENSOR_TYPE, SENSOR_NUMBER,
                      True, UPLOAD_INTERVAL_MS)

        # 5、获取数据
        print("getValue:", getValue(api, SENSOR_TYPE, SENSOR_NUMBER))

        # 6、配置数据：将 1 号 WS2812B 设置为红色
        setValue(api, 0x07, 1, 0x00FF0000)

        # 7、读取接收缓存
        data = read(api, SENSOR_TYPE, SENSOR_NUMBER)
        if data is not None:
            print("read:", data.value)

        # 8、监听总线 10 秒
        listenning(api, printData, duration_s=10)

    finally:
        # 关闭自动上传，切换为手动查询模式
        try:
            setUploadMode(api, SENSOR_TYPE, SENSOR_NUMBER, False)
        finally:
            api.close()


if __name__ == "__main__":
    main()
