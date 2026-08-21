from api_call_example import (
    initialize,
    scanning,
    getScanData,
)

api = initialize("/dev/ttyS2", 115200)

try:
    # print("scanning() result:")
    # nodes = scanning(api)
    # print(nodes)

    print("getScanData() result:")
    scan_data = getScanData(api)
    print(scan_data)

finally:
    api.close()