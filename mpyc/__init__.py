"""mpyc: CPython 侧的 MicroPython 交互层（基于 nanobind 扩展模块 mpy）。

替代旧的 ctypes(device.py) + stdout-capture 方案：
- 值传递走 mpy.call 类型化快速路径（int/float/bool/str/bytes/buffer）
- 对象结果用远端句柄表代理，可链式访问、可回传
- MicroPython 异常 -> RuntimeError（带完整 traceback）

用法:
    from mpyc import Mpyc
    m = Mpyc(heap_size=4*1024*1024)
    m.exec_file("/root/launcher/mpy_env.py")
    m.env.lv.tick_inc(40)              # 属性路径代理
    scr = m.env.lv.screen_active()     # 对象结果 -> 远端句柄代理
    scr.set_style_bg_opa(0, 0)         # 链式调用
"""

from .mpyc import Mpyc
from .proxy import Module, Attribute
