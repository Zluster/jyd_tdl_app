"""launcher: CPython 宿主入口。

职责划分：
- 硬件/显示通路（tdl_py 的 VO/VPSS/OSD、framebuffer 注册）在本文件
- 与 MicroPython 的一切交互走 mpyc（typed bridge + 属性代理），
  不再有内联的 MicroPython 封装类和 tick 源码模板
"""

import asyncio
import signal
import time

import mpy      # 仅用于显示通路（set_flush_buf / set_flush_callback）
import tdl_py
from mpyc import Mpyc


async def lv_tick_loop(m: Mpyc, freq: int = 25):
    """LVGL 事件循环：走 mpyc 的 __mpyc_tick__ 快速路径。"""
    delay = 1.0 / freq
    ms = int(delay * 1000)
    next_tick = time.perf_counter()
    while True:
        try:
            m.tick(ms)
        except RuntimeError as e:
            # MicroPython 侧异常不终止 UI 循环，打印后继续
            print("Exception in task_handler:", e)
        next_tick += delay
        sleep_time = next_tick - time.perf_counter()
        await asyncio.sleep(sleep_time if sleep_time > 0 else 0)


def setup_display_path():
    """摄像头预览 -> VPSS -> VO 的媒体链路（幂等）。

    返回创建的链路对象列表，调用者必须持有到程序结束：VoOutput 的
    析构会关闭 VO、MediaLink 的析构会 unbind，对象被 GC 即通路被拆。
    """
    keep = []
    if not tdl_py.vo_is_enabled(0):
        vo = tdl_py.VoOutput()
        vo.open()
        keep.append(vo)
    if tdl_py.get_bind_source_vpss(1) is None:          # grp1 输入端没人绑
        preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
        preview.bind()
        keep.append(preview)
    if tdl_py.get_bind_source_vo(0, 0) is None:         # VO layer0/ch0 没人绑
        display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
        display.bind()
        keep.append(display)
    return keep


async def main():
    media_links = setup_display_path()  # 持有到 main 结束，勿删

    osd_screen = tdl_py.Osd(handle=202, canvas_count=2)
    try:
        osd_screen.create()
        osd_screen.attach(group=1, channel=0, layer=1)
        # 常驻双缓冲：persistent_pair() 返回 (back, front) 两块跨 update() 常驻
        # 有效的画布映射（普通 canvas() 的地址在 update 后失效）。按 (back,
        # front) 顺序交给 LVGL 当双缓冲，update() 翻页与 LVGL 缓冲交替相位锁定，
        # 消除单缓冲下"清区-重绘被硬件扫到"造成的闪烁。
        back, front = osd_screen.persistent_pair()
        mpy.set_flush_buf(back.data, front.data)
        osd_screen.set_visible(True)

        def on_flush(x1, y1, x2, y2, data):
            osd_screen.update()
        mpy.set_flush_callback(on_flush, bytes_per_pixel=4)

        with Mpyc(heap_size=4 * 1024 * 1024) as m:
            m.exec_file("/root/launcher/mpy_env.py", capture=False)
            tick_task = asyncio.create_task(lv_tick_loop(m, 25))
            while True:
                await asyncio.sleep(0.01)
    finally:
        # 确定性清理：不依赖 GC 触发析构。清理期间屏蔽 SIGINT，
        # 避免第二次 Ctrl+C 把 CVI_RGN_Destroy 打断导致 region 泄漏
        # （泄漏后 handle 被占用，下次 create 失败，只能重启恢复）。
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        mpy.set_flush_callback(None)
        mpy.set_flush_buf(None)
        osd_screen.destroy()


if __name__ == "__main__":
    asyncio.run(main())
