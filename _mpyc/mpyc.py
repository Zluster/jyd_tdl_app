"""Mpyc 主类：MicroPython 生命周期 + 类型化调用通道 + 代理入口。
（vendored，复制自 launcher/mpyc/mpyc.py。dara 侧差异：去掉 env_stub 的
TYPE_CHECKING 引用；新增跨线程转交——MicroPython 不可重入且 C 栈/TLS
绑定创建线程，所有跨桥方法在其他线程调用时排队转交绑定线程执行、
阻塞取结果，绑定线程的主循环周期性调 service() 消化队列）

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
import threading
import queue

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


class _Job:
    """一次跨线程转交调用：绑定线程执行，调用方阻塞等结果。"""

    __slots__ = ("fn", "done", "result", "error")

    def __init__(self, fn):
        self.fn = fn
        self.done = threading.Event()
        self.result = None
        self.error = None

    def run(self):
        try:
            self.result = self.fn()
        except BaseException as e:      # 原样带回调用线程重抛
            self.error = e
        self.done.set()


class Mpyc:
    """MicroPython 解释器的生命周期与交互封装（进程内单例语义）。

    实例绑定创建线程（mpy.init 时 MP 的 C 栈/TLS 记录在该线程），全部
    跨桥方法经 _submit：绑定线程直接执行，其他线程排队转交并等结果，
    因此任意线程都能安全使用代理。绑定线程需周期调 service() 消化队列。"""

    def __init__(self, heap_size=16 * 1024 * 1024):
        if not mpy.active():
            mpy.init(heap_size)
        mpy.exec(_PRELUDE)
        #: 属性路径代理入口（m.env.lv 即 MicroPython 里的 lvgl 模块）
        self.env = Module(self)
        self._thread = threading.get_ident()   # MP 栈/TLS 绑定的线程
        self._jobs = queue.SimpleQueue()       # 其他线程转交的待执行调用
        self._closed = False

    # ---- 跨线程转交 ----

    def _submit(self, fn, wait=True):
        """绑定线程直接执行 fn；其他线程入队转交，默认阻塞等结果
        （异常原样重抛）。wait=False 供 __del__ 等不可阻塞场合：入队
        即返回，结果/异常丢弃。"""
        if threading.get_ident() == self._thread:
            return fn()
        if self._closed:
            if not wait:
                return None
            raise RuntimeError("MicroPython 已关闭，跨线程调用被丢弃")
        job = _Job(fn)
        self._jobs.put(job)
        if not wait:
            return None
        while not job.done.wait(1.0):   # 周期醒来：绑定线程死亡不悬死
            if self._closed:
                raise RuntimeError("MicroPython 已关闭，跨线程调用被丢弃")
        if job.error is not None:
            raise job.error
        return job.result

    def service(self, timeout=0.0):
        """消化转交队列（只能在绑定线程调用）：最多阻塞 timeout 秒等
        首个任务，随后把已到队的一并执行完。宿主的 UI 线程主循环在
        两次 tick 之间调用本方法。"""
        try:
            job = (self._jobs.get(timeout=timeout) if timeout > 0
                   else self._jobs.get_nowait())
        except queue.Empty:
            return
        while True:
            job.run()
            try:
                job = self._jobs.get_nowait()
            except queue.Empty:
                return

    # ---- 基础通道（直通 nanobind 模块） ----

    def exec(self, code: str, capture: bool = True) -> str:
        """执行源码；返回捕获的 stdout。异常 -> RuntimeError(traceback)。"""
        return self._submit(lambda: mpy.exec(code, capture=capture))

    def exec_file(self, path: str, capture: bool = True) -> str:
        return self._submit(lambda: mpy.exec_file(path, capture=capture))

    def call(self, name: str, *args):
        """快速路径：调用 __main__ 里的函数（类型化参数/返回值）。"""
        return self._submit(lambda: mpy.call(name, *args))

    # ---- 代理支撑：proxy.py 通过这三个入口访问 MicroPython ----

    def proxy_eval(self, expr: str):
        """求值任意表达式；对象结果返回句柄代理。"""
        return self._submit(
            lambda: unmarshal_result(self, mpy.call("__mpyc_eval__", expr)))

    def proxy_call(self, path: str, *args, **kwargs):
        """调用路径指向的可调用对象，参数走类型化桥。"""
        return self._submit(lambda: self._proxy_call(path, args, kwargs))

    def _proxy_call(self, path, args, kwargs):
        # 已在绑定线程。marshal_arg 里的 mpy.register 也必须在此执行：
        # 宿主函数注册表无锁，跨线程写会与 MicroPython 侧读取竞争
        if kwargs:
            # 桥不支持关键字参数：回退到表达式构造（值须可 repr 还原）
            sig = ", ".join(
                [f"__mpyc_unbox__({marshal_arg(self, a)!r})" for a in args]
                + [f"{k}=__mpyc_unbox__({marshal_arg(self, v)!r})" for k, v in kwargs.items()])
            return self.proxy_eval(f"{path}({sig})")
        margs = [marshal_arg(self, a) for a in args]
        return unmarshal_result(self, mpy.call("__mpyc_call__", path, *margs))

    def proxy_set(self, path: str, value):
        self._submit(
            lambda: mpy.call("__mpyc_set__", path, marshal_arg(self, value)))

    def proxy_release(self, index: int):
        def drop():
            try:
                mpy.call("__mpyc_release__", index)
            except Exception:
                pass  # 解释器可能已 deinit，忽略
        self._submit(drop, wait=False)   # 代理 __del__ 里调用，不可阻塞

    # ---- 反向注册 ----

    def export(self, fn=None, *, name=None):
        """装饰器：注册 CPython 函数供 MicroPython 调用（mpy_embed.<name>）。

        注解（int/float/bool/str/bytes/memoryview/None）决定参数转换。
        """
        def wrap(f):
            self._submit(lambda: mpy.register(f, name=name))
            return f
        return wrap(fn) if fn is not None else wrap

    def register(self, fn, name=None):
        self._submit(lambda: mpy.register(fn, name=name or fn.__name__))

    def unregister(self, name: str):
        """反注册 register/export 过的宿主函数（同样经转交队列串行）。"""
        self._submit(lambda: mpy.unregister(name))

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

    # ---- LVGL tick（由绑定线程的主循环调用） ----

    def tick(self, ms: int):
        """推进 LVGL 时钟并跑一次 task_handler（prelude 内置快速路径）。"""
        return self._submit(lambda: mpy.call("__mpyc_tick__", ms))

    # ---- 生命周期 ----

    def close(self):
        """只能在绑定线程调用（跨线程 deinit 必段错误）。置关闭标志、
        唤醒并回绝所有排队中的转交调用，再 deinit。"""
        self._closed = True
        while True:
            try:
                job = self._jobs.get_nowait()
            except queue.Empty:
                break
            job.error = RuntimeError("MicroPython 已关闭，跨线程调用被丢弃")
            job.done.set()
        if mpy.active():
            mpy.deinit()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
