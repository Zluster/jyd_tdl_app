# LVGL indev driver for evdev mouse device
# (for the unix micropython port)
path = __file__[:__file__.rfind('/')+1]
import ustruct
import select
import sys
import lvgl as lv

# struct input_event 尺寸随用户态位数变化：64 位为 24 字节（QQHHi），
# 32 位为 16 字节（IIHHi）。读长度不是事件尺寸整数倍时内核返回 EINVAL。
if sys.maxsize > 2**32:
    EV_FMT, EV_SIZE = 'QQHHi', 24
else:
    EV_FMT, EV_SIZE = 'IIHHi', 16

#import lv_pm
#pm = lv_pm.pm()
# evdev driver for mouse
class mouse_indev:
    def __init__(self, scr=None, dev="/dev/input/event0"):
        self.evdev = open(dev, 'rb')
        self.poll = select.poll()
        self.poll.register(self.evdev.fileno())
        self.scr = scr if scr else lv.screen_active()
        self.hor_res = self.scr.get_width()
        self.ver_res = self.scr.get_height()

        # Register LVGL indev driver
        self.indev = lv.indev_create()
        self.indev.set_type(lv.INDEV_TYPE.POINTER)
        self.indev.set_read_cb(self.mouse_read)

        # with open(path + 'mouse.png', 'rb') as f:
        #     png_data = f.read()

        # img = lv.image_dsc_t({
        #     "data_size": len(png_data),
        #     "data": png_data,
        # })

        # mouse_img = lv.image(self.scr)
        # mouse_img.set_src(img)
        # self.indev.set_cursor(mouse_img)
        
        self.timer = self.indev.get_read_timer()
        self.timer.set_period(16)
        
        self.x = 50
        self.y = 50
        self.b = 0

    def mouse_read(self, indev, data) -> int:
        # Check if there is input to be read from evdev
        if not self.poll.poll()[0][1] & select.POLLIN:
            return 0
        while True:
            # Data is relative, update coordinates
            time_sec, time_usec, _type, code, value = ustruct.unpack(EV_FMT, self.evdev.read(EV_SIZE))
            if _type == 0x03:
                if code == 53:
                    self.x = value
                elif code == 54:
                    self.y = value
            # elif code == 50:
            #     self.b = value
            elif _type == 0x01 and code == 330:
                self.b = value
            if not self.poll.poll()[0][1] & select.POLLIN:
                break
        data.point.x = self.hor_res - self.x
        data.point.y = self.ver_res - self.y

        # data.point.x = int(data.point.x / 2) + 256
        # data.point.y = int(data.point.y / 2) #150
        
        # Handle coordinate overflow cases
        # data.point.x = min(data.point.x, self.hor_res - 1)
        # data.point.y = min(data.point.y, self.ver_res - 1)
        # data.point.x = max(data.point.x, 0)
        # data.point.y = max(data.point.y, 0)

        # Update "pressed" status
        data.state = lv.INDEV_STATE.PRESSED if ((self.b & 1) == 1) else lv.INDEV_STATE.RELEASED

        return 0

    def delete(self):
        self.evdev.close()
        self.indev.enable(False)

