"""vendored mpyc：CPython 侧的 MicroPython 交互层（基于 nanobind 扩展模块 mpy）。

复制自 launcher/mpyc（按约定不动老代码，jyd 自包含、不依赖部署目录）。
与原版唯一差异：去掉 env_stub.pyi 的 TYPE_CHECKING 引用（jyd 的 IDE 补全
走 jyd/lv.pyi）。

用法:
    from jyd._mpyc import Mpyc
    m = Mpyc(heap_size=4*1024*1024)
    m.env.lv.tick_inc(40)              # 属性路径代理
    scr = m.env.lv.screen_active()     # 对象结果 -> 远端句柄代理
    scr.set_style_bg_opa(0, 0)         # 链式调用
"""

from .mpyc import Mpyc
from .proxy import Module, Attribute
