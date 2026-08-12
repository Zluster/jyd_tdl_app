# import ffi
import lvgl as lv
import uctypes

path = __file__[:__file__.rfind('/')+1]

import time
import mpy_embed

fcnt = 0
ts1 = 0
def calc_fps(tips: str) -> None:
    global fcnt, ts1
    """
    计算并打印帧率(FPS),功能等价于C语言的CALC_FPS宏.
    
    参数:
        tips: 用于标识FPS信息的字符串,将作为前缀打印
    """
    fcnt += 1
    
    # 获取当前单调时间(毫秒)
    # 等价于C代码: clock_gettime(CLOCK_MONOTONIC, &ts2)
    # 然后计算: ts2.tv_sec * 1000 + ts2.tv_nsec / 1000000
    ts2_ms = time.ticks_ms()
    
    # 检查是否已经过去1000毫秒(1秒)
    if ts2_ms - ts1 >= 1235:
        # 打印FPS信息,格式与C代码一致: "tips => fcnt FPS\r\n"
        print(f"{tips} => {fcnt} FPS", end="\r\n")
        # 更新基准时间戳和重置帧计数器
        ts1 = ts2_ms
        fcnt = 0

class Display:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.buf_size = width * height * 4
        
        self.disp_drv = lv.display_create(self.width, self.height)
        # lv.screen_active().get_style_bg_opa(0, lv.STATE.MAIN)
        self.disp_drv.set_color_format(lv.COLOR_FORMAT.ARGB8888)

        self.buf_addr = mpy_embed.get_flush_buf()
        self.buf = uctypes.bytearray_at(self.buf_addr, self.buf_size)
        # 第二块 framebuffer（OSD 双缓冲），CPython 侧注册了才有
        buf_addr2 = mpy_embed.get_flush_buf(1)
        self.buf2 = uctypes.bytearray_at(buf_addr2, self.buf_size) if buf_addr2 else None
        self.disp_drv.set_buffers(self.buf, self.buf2, self.buf_size, lv.DISPLAY_RENDER_MODE.DIRECT)
        self.disp_drv.set_flush_cb(self.flush)
        
        # self.disp_drv.refr_timer.set_period(16)

    def flush(self, disp, area, color_p):
        data_view = color_p.__dereference__(self.buf_size)
        #data_bytes = bytes(data_view)
        # flush_is_last
        # display_show()
        if disp.flush_is_last():
            # calc_fps("flush")
            mpy_embed.call_flush_callback(area.x1, area.y1, area.x2, area.y2, data_view)
        disp.flush_ready()
