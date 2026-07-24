"""Mpyc 主类：MicroPython 生命周期 + 类型化调用通道 + 代理入口。

数据通道约定（与 MicroPython 侧 prelude 配合）:
- 标量与二进制（None/int/float/bool/str/bytes/buffer）经 mpy.call 原生跨桥
- 特殊字符串标记（首字节 \\x00）用于表达无法直传的引用:
    \\x00ref\\x00<path>    MicroPython 对象句柄（存于 __mpyc_objs__ 注册表）
    \\x00host\\x00<name>   CPython 已注册函数（mpy_embed.get(name) 可取）
    \\x00path\\x00<path>   CPython 侧代理路径回传（eval 还原成对象）
"""

import textwrap
import inspect
import functools

import mpy

from .proxy import Module, Attribute, marshal_arg, unmarshal_result

# MicroPython 侧运行时支撑，Mpyc 初始化时注入 __main__ globals。
_PRELUDE = r"""
__mpyc_objs__ = {}
__mpyc_seq__ = [0]

def __mpyc_box__(v):
    if v is None or isinstance(v, (int, float, bool, str, bytes, bytearray)):
        return v
    __mpyc_seq__[0] += 1
    __mpyc_objs__[__mpyc_seq__[0]] = v
    return '\x00ref\x00__mpyc_objs__[%d]' % __mpyc_seq__[0]

def __mpyc_unbox__(a):
    if isinstance(a, str) and a.startswith('\x00'):
        if a.startswith('\x00host\x00'):
            import mpy_embed
            return mpy_embed.get(a[6:])
        if a.startswith('\x00path\x00'):
            return eval(a[6:], globals())
    return a

def __mpyc_eval__(expr):
    return __mpyc_box__(eval(expr, globals()))

def __mpyc_exec__(stmt):
    exec(stmt, globals())

def __mpyc_call__(path, *args):
    fn = eval(path, globals())
    return __mpyc_box__(fn(*[__mpyc_unbox__(a) for a in args]))

def __mpyc_set__(path, v):
    globals()['__mpyc_tmp__'] = __mpyc_unbox__(v)
    exec(path + ' = __mpyc_tmp__', globals())
    del globals()['__mpyc_tmp__']

def __mpyc_release__(i):
    __mpyc_objs__.pop(i, None)

def __mpyc_tick__(ms):
    import lvgl as lv
    lv.tick_inc(ms)
    lv.task_handler()
"""


class Mpyc:
    """MicroPython 解释器的生命周期与交互封装（进程内单例语义）。"""

    def __init__(self, heap_size=16 * 1024 * 1024):
        if not mpy.active():
            mpy.init(heap_size)
        mpy.exec(_PRELUDE)
        #: 属性路径代理入口：m.env.lv.tick_inc(40)
        self.env = Module(self)

    # ---- 基础通道（直通 nanobind 模块） ----

    def exec(self, code: str, capture: bool = True) -> str:
        """执行源码；返回捕获的 stdout。异常 -> RuntimeError(traceback)。"""
        return mpy.exec(code, capture=capture)

    def exec_file(self, path: str, capture: bool = True) -> str:
        return mpy.exec_file(path, capture=capture)

    def call(self, name: str, *args):
        """快速路径：调用 __main__ 里的函数（类型化参数/返回值）。"""
        return mpy.call(name, *args)

    # ---- 代理支撑：proxy.py 通过这三个入口访问 MicroPython ----

    def proxy_eval(self, expr: str):
        """求值任意表达式；对象结果返回句柄代理。"""
        return unmarshal_result(self, mpy.call("__mpyc_eval__", expr))

    def proxy_call(self, path: str, *args, **kwargs):
        """调用路径指向的可调用对象，参数走类型化桥。"""
        if kwargs:
            # 桥不支持关键字参数：回退到表达式构造（值须可 repr 还原）
            sig = ", ".join(
                [f"__mpyc_unbox__({marshal_arg(self, a)!r})" for a in args]
                + [f"{k}=__mpyc_unbox__({marshal_arg(self, v)!r})" for k, v in kwargs.items()])
            return self.proxy_eval(f"{path}({sig})")
        margs = [marshal_arg(self, a) for a in args]
        return unmarshal_result(self, mpy.call("__mpyc_call__", path, *margs))

    def proxy_set(self, path: str, value):
        mpy.call("__mpyc_set__", path, marshal_arg(self, value))

    def proxy_release(self, index: int):
        try:
            mpy.call("__mpyc_release__", index)
        except Exception:
            pass  # 解释器可能已 deinit，忽略

    # ---- 反向注册 ----

    def export(self, fn=None, *, name=None):
        """装饰器：注册 CPython 函数供 MicroPython 调用（mpy_embed.<name>）。

        注解（int/float/bool/str/bytes/memoryview/None）决定参数转换。
        """
        def wrap(f):
            mpy.register(f, name=name)
            return f
        return wrap(fn) if fn is not None else wrap

    # ---- 源码搬运：把 CPython 写的函数下放到 MicroPython 执行 ----

    def code(self, func):
        """装饰器：函数源码注入 MicroPython，调用即远端执行。

        @m.code
        def build_ui(title: str):
            import lvgl as lv
            ...
        build_ui("hello")        # 实际在 MicroPython 里运行
        """
        src = inspect.getsource(func)
        body = src.split("\n", 1)[1] if src.lstrip().startswith("@") else src
        self.exec(textwrap.dedent(body))

        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            return self.proxy_call(func.__name__, *args, **kwargs)
        return wrapper

    # ---- LVGL tick（配合 CPython 侧事件循环调用） ----

    def tick(self, ms: int):
        """推进 LVGL 时钟并跑一次 task_handler（prelude 内置快速路径）。"""
        mpy.call("__mpyc_tick__", ms)

    # ---- 生命周期 ----

    def close(self):
        if mpy.active():
            mpy.deinit()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
