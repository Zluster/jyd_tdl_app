"""vendored mpyc：CPython 侧的 MicroPython 交互层（基于 nanobind 扩展模块 mpy）。

复制自 launcher/mpyc（jyd 自包含、不依赖部署目录）。与原版差异：
- 去掉 env_stub.pyi 的 TYPE_CHECKING 引用（jyd 的 IDE 补全走 jyd/lv.pyi）；
- Mpyc 增加跨线程转交（_submit/service）：跨桥方法在非绑定线程调用时
  排队转交绑定线程执行，任意线程都能安全使用代理（见 mpyc.py 头注释）。

用法:
    from jyd._mpyc import Mpyc
    m = Mpyc(heap_size=4*1024*1024)
    m.env.lv.tick_inc(40)              # 属性路径代理
    scr = m.env.lv.screen_active()     # 对象结果 -> 远端句柄代理
    scr.set_style_bg_opa(0, 0)         # 链式调用
"""

from .mpyc import Mpyc
from .proxy import Module, Attribute
