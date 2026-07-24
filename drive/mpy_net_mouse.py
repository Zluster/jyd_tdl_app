# LVGL indev driver: network mouse (UDP server)
# (for the unix micropython port, embedded via mpy.so)
#
# 与 mpy_mouse.py（evdev 触摸屏）平行的实现：坐标来自网络而不是 /dev/input。
# 配套 PC 端发送工具: test_lvgl/net_mouse_sender.py
#
# 协议: UDP, 每包 5 字节, struct 格式 "!HHB"
#   x: uint16  绝对屏幕坐标（发送端已按目标分辨率映射好）
#   y: uint16
#   b: uint8   按键位掩码, bit0 = 左键按下
#
# 用法（MicroPython 侧）:
#   from drive.mpy_net_mouse import net_mouse_indev
#   mouse = net_mouse_indev(lv.layer_sys(), port=5555)

path = __file__[:__file__.rfind('/') + 1]
import socket
import ustruct
import lvgl as lv


class net_mouse_indev:
    def __init__(self, scr=None, port=5555):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        addr = socket.getaddrinfo('0.0.0.0', port)
        self.sock.bind(addr[0][4])
        self.sock.setblocking(False)

        self.scr = scr if scr else lv.screen_active()
        self.hor_res = self.scr.get_width()
        self.ver_res = self.scr.get_height()

        # Register LVGL indev driver
        self.indev = lv.indev_create()
        self.indev.set_type(lv.INDEV_TYPE.POINTER)
        self.indev.set_read_cb(self.mouse_read)

        with open(path + 'mouse.png', 'rb') as f:
            png_data = f.read()

        self.img_dsc = lv.image_dsc_t({
            "data_size": len(png_data),
            "data": png_data,
        })

        mouse_img = lv.image(self.scr)
        mouse_img.set_src(self.img_dsc)
        self.indev.set_cursor(mouse_img)

        self.timer = self.indev.get_read_timer()
        self.timer.set_period(16)

        self.x = self.hor_res // 2
        self.y = self.ver_res // 2
        self.b = 0

    def mouse_read(self, indev, data) -> int:
        # 排空积压的数据报，只取最新状态
        while True:
            try:
                pkt = self.sock.recv(16)
            except OSError:  # EAGAIN: 没有更多数据
                break
            if len(pkt) >= 5:
                self.x, self.y, self.b = ustruct.unpack('!HHB', pkt[:5])

        # 边界钳制，防止发送端分辨率配置不一致时光标飞出屏幕
        data.point.x = min(max(self.x, 0), self.hor_res - 1)
        data.point.y = min(max(self.y, 0), self.ver_res - 1)
        data.state = lv.INDEV_STATE.PRESSED if (self.b & 1) else lv.INDEV_STATE.RELEASED
        return 0

    def delete(self):
        self.sock.close()
        self.indev.enable(False)
