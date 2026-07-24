"""CPython 端封装：基于 nanobind 扩展模块 `mpy` 嵌入 MicroPython。

替代旧的 ctypes + libmicropython.so 方案（cpy_lvgl.py）。主要差异：

- 不再手动声明函数签名，import mpy 即用
- exec()/exec_file() 直接返回捕获的 stdout；MicroPython 未捕获异常
  变为 RuntimeError（消息为完整 traceback），不再需要
  capture()/capture_read()
- 函数注册用 @m.export 装饰器，参数/返回值注解决定类型转换
- LVGL 事件循环由 CPython 侧驱动（mpy.call 快速路径），不再依赖
  MicroPython 内部的 lv_timer.py（signal + ffi 方案已废弃）

用法示例见文件末尾 __main__。
"""

import asyncio
import ctypes
import signal

import mpy


class MicroPython:
    """MicroPython 解释器的生命周期与调用封装。"""

    def __init__(self, heap_size=16 * 1024 * 1024):
        mpy.init(heap_size)

    # ---- 执行 ----

    def exec(self, code: str, capture: bool = True) -> str:
        """执行 MicroPython 源码，返回捕获的 stdout。

        未捕获的 MicroPython 异常 -> RuntimeError(traceback 文本)。
        capture=False 时不重定向 stdout：print 直接打到终端（进程崩溃
        也不会丢），返回值为空字符串。调试崩溃问题时建议 False。
        """
        return mpy.exec(code, capture=capture)

    def exec_file(self, path: str, capture: bool = True) -> str:
        """执行 MicroPython 脚本文件，语义同 exec()。"""
        return mpy.exec_file(path, capture=capture)

    def call(self, name: str, *args):
        """快速路径：直接调用 MicroPython __main__ 里的函数。

        跳过词法分析/编译和 stdout 捕获，适合高频调用（tick、事件上报）。
        """
        return mpy.call(name, *args)

    # ---- 注册 CPython 函数给 MicroPython 调用 ----

    def export(self, fn=None, *, name=None):
        """装饰器：把 CPython 函数注册给 MicroPython。

        @m.export
        def read_sensor() -> float: ...

        @m.export(name="cfg")
        def load_config(path: str) -> bytes: ...

        MicroPython 侧: import mpy_embed; mpy_embed.read_sensor()
        参数/返回值注解（int/float/bool/str/bytes/memoryview/None）决定
        转换方式；无注解按运行时类型自动转换。
        """
        def wrap(f):
            mpy.register(f, name=name)
            return f
        if fn is None:
            return wrap          # @m.export(name=...)
        return wrap(fn)          # @m.export

    def unexport(self, name: str):
        mpy.unregister(name)

    def exports(self):
        return mpy.registered()

    # ---- 生命周期 ----

    def close(self):
        if mpy.active():
            mpy.deinit()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

# ==================== 使用示例 ====================
import time
async def lv_tick(m, freq=25):
        delay = 1.0 / freq
        next_tick = time.perf_counter()  # 使用perf_counter更精确
        tick_str = f'''
lv.tick_inc({int(delay*1000)})
if lv._nesting.value == 0:
    try:
        lv.task_handler()
    except Exception as e:
        print("Exception in task_handler:", e)
'''
        while True:
            m.exec(tick_str, capture=False)
            next_tick += delay
            sleep_time = next_tick - time.perf_counter()
            await asyncio.sleep(sleep_time if sleep_time > 0 else 0)

import tdl_py
async def main():
    vo = None
    if not tdl_py.vo_is_enabled(0):
        vo = tdl_py.VoOutput()
        vo.open()
    preview = None
    if tdl_py.get_bind_source_vpss(1) is None:          # grp1 输入端没人绑
        preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
        preview.bind()
    display = None
    if tdl_py.get_bind_source_vo(0, 0) is None:         # VO layer0/ch0 没人绑
        display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
        display.bind()

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

        with MicroPython(heap_size=4 * 1024 * 1024) as m:
            out = m.exec_file("/root/launcher/mpy_env.py", capture=False)
            # await lv_tick(m, 25)
            tick_task = asyncio.create_task(lv_tick(m, 25))
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
