"""属性路径代理：让 CPython 用自然语法操作 MicroPython 对象。

m.env.lv.tick_inc(40)          -> __mpyc_call__("lv.tick_inc", 40)
scr = m.env.lv.screen_active() -> 对象结果进远端句柄表, 返回 Attribute 代理
scr.set_style_bg_opa(0, 0)     -> __mpyc_call__("__mpyc_objs__[3].set_style_bg_opa", 0, 0)
m.env.brightness = 50          -> __mpyc_set__("brightness", 50)
v = scr.get_width().value      -> 标量直接跨桥返回

约定（与 mpyc.py 的 _PRELUDE 对应）:
- 标量/bytes 原生跨桥；对象以 '\x00ref\x00<path>' 句柄字符串返回
- CPython callable 参数自动注册为宿主函数并以 '\x00host\x00<name>' 传递
- Attribute 参数以 '\x00path\x00<path>' 传递（MicroPython 侧 eval 还原）
"""

_REF = "\x00ref\x00"
_HOST = "\x00host\x00"
_PATH = "\x00path\x00"

_SCALARS = (int, float, bool, str, bytes, bytearray, memoryview, type(None))


def marshal_arg(client, a):
    """CPython 参数 -> 可跨桥表示。"""
    if isinstance(a, Attribute):
        return _PATH + object.__getattribute__(a, "_path")
    if isinstance(a, _SCALARS):
        return a
    if callable(a):
        name = getattr(a, "__name__", None)
        if not name or name == "<lambda>":
            name = "_mpyc_cb_%x" % id(a)
        import mpy
        mpy.register(a, name=name)
        return _HOST + name
    raise TypeError(
        "cannot pass %r to MicroPython (use scalars, bytes, buffers, "
        "callables or mpyc proxies)" % type(a))


def unmarshal_result(client, v):
    """跨桥返回值 -> CPython 值或代理。"""
    if isinstance(v, str) and v.startswith(_REF):
        path = v[len(_REF):]
        # 句柄路径形如 __mpyc_objs__[42]，代理销毁时释放远端条目
        idx = int(path[path.index("[") + 1:path.index("]")])
        return Attribute(client, path, ref_index=idx)
    return v


class Module:
    """根命名空间代理：m.env.<name> 即 MicroPython __main__ 的全局名。"""

    def __init__(self, client):
        object.__setattr__(self, "_client", client)

    def __getattr__(self, name):
        return Attribute(object.__getattribute__(self, "_client"), name)

    def __setattr__(self, name, value):
        object.__getattribute__(self, "_client").proxy_set(name, value)


class Attribute:
    """路径代理。所有真实交互延迟到 调用/取值/赋值 发生时。"""

    def __init__(self, client, path, ref_index=None, owner=None):
        object.__setattr__(self, "_client", client)
        object.__setattr__(self, "_path", path)
        object.__setattr__(self, "_ref_index", ref_index)
        # 派生代理持有其句柄根，防止根代理先被回收导致远端对象释放
        object.__setattr__(self, "_owner", owner)

    # ---- 路径构造 ----

    def __getattr__(self, name):
        return Attribute(self._client, "%s.%s" % (self._path, name),
                         owner=self._owner or self)

    def __getitem__(self, index):
        if isinstance(index, slice):
            part = "%s:%s%s" % (
                "" if index.start is None else index.start,
                "" if index.stop is None else index.stop,
                "" if index.step is None else ":%s" % index.step)
        else:
            part = repr(index)
        return Attribute(self._client, "%s[%s]" % (self._path, part),
                         owner=self._owner or self)

    # ---- 真实交互 ----

    def __call__(self, *args, **kwargs):
        return self._client.proxy_call(self._path, *args, **kwargs)

    def __setattr__(self, name, value):
        self._client.proxy_set("%s.%s" % (self._path, name), value)

    def __setitem__(self, index, value):
        self._client.proxy_set("%s[%s]" % (self._path, repr(index)), value)

    @property
    def value(self):
        """取值：标量直接返回，对象返回新的句柄代理。"""
        return self._client.proxy_eval(self._path)

    # ---- 表现与回收 ----

    def __repr__(self):
        return "<mpyc %s>" % self._path

    def __str__(self):
        v = self.value
        return v if isinstance(v, str) else repr(v)

    def __del__(self):
        idx = object.__getattribute__(self, "_ref_index")
        if idx is not None:
            try:
                object.__getattribute__(self, "_client").proxy_release(idx)
            except Exception:
                pass
