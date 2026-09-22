"""jyd 屏幕终端：把 Python 的 stdout / stderr 复制一份显示到屏幕上的 LVGL 控件。

JYD_LV_USE=0 时由 __init__ 在拉起显示通路之前安装（见 Console.install）。
用途是"没有终端可看"的场景（web 启动器、脱机跑脚本）：print、异常回溯、
logging 都能在屏幕正中的深色面板里看到，stdout 与 stderr 用不同颜色区分。

设计上只有一条铁律：**写流的一方只入队，绝不碰 LVGL**。原因是 LVGL 只能
在 jyd-ui 线程上操作，而 print 可能发生在任何线程——包括 jyd-ui 线程自己
的 bind 回调栈里。若在写流时直接跨桥更新控件，前者是跨线程调用、后者是
在渲染栈里再触发渲染，两条都会死锁或崩溃。于是分成两半：

    _Tee.write()   任意线程：原样转发给旧流（终端 / web 日志照常记录），
                   按行切开放进有上限的队列
    Console.pump() 只在 jyd-ui 线程、每拍 tick 之前调用：限量取出队列里的
                   行，相邻同源行合并成一块，一次跨桥调用建一个 label

边界：只捕获 Python 的 sys.stdout / sys.stderr。C/C++ 原生库直接写文件
描述符 1/2 的输出（tdl_py 的 printf 等）不经过 sys.stdout，仍只出现在原
终端。退出（runtime.close）或显示初始化失败时恢复原流。
"""

import collections
import sys
import threading

#: 队列里最多保留的行数；写得比屏幕消化得快时丢最旧的，丢弃数会在
#: 面板上提示一次
_MAX_LINES = 2000
#: 每拍 tick 前最多消化的行数：既保证输出洪水下 UI 仍能按节拍刷新，
#: 又不至于让一拍里跨桥太多次
_LINES_PER_PUMP = 64
#: 面板尺寸：居中于 720x480 屏，刻意避开左上角 (8,8)-(104,68) 的 web
#: 退出按钮（那是 web 场景唯一的退出手段，不能被盖住）
_PANEL_W, _PANEL_H = 640, 340
#: 面板最多保留的输出块数，超过删最旧的
_MAX_BLOCKS = 120

#: MicroPython 侧：建面板 + 追加一块文本。颜色：stdout 淡青、stderr 红、
#: 终端自身状态蓝。面板挂 layer_top（盖在所有 screen 之上，换屏不消失）
_CONSOLE_SRC = r"""
import lvgl as _lv

_jyd_console_log = None
_JYD_CONSOLE_MAX = %(max_blocks)d
_JYD_CONSOLE_COLORS = {"out": 0xB2EBF2, "err": 0xFF5252, "sys": 0x64B5F6}

def _jyd_console_create(w, h):
    global _jyd_console_log
    panel = _lv.obj(_lv.layer_top())
    panel.set_size(w, h)
    panel.center()
    panel.set_style_bg_color(_lv.color_hex(0x101418), 0)
    panel.set_style_bg_opa(_lv.OPA._90, 0)
    panel.set_style_border_color(_lv.color_hex(0x37474F), 0)
    panel.set_style_border_width(1, 0)
    panel.set_style_radius(6, 0)
    panel.set_style_pad_all(8, 0)
    panel.set_style_pad_row(4, 0)
    panel.set_flex_flow(_lv.FLEX_FLOW.COLUMN)
    title = _lv.label(panel)
    title.set_width(_lv.pct(100))
    title.set_text("console   stdout / stderr")
    title.set_style_text_color(_lv.color_hex(0x64B5F6), 0)
    log = _lv.obj(panel)
    log.set_width(_lv.pct(100))
    log.set_flex_grow(1)
    log.set_style_bg_opa(_lv.OPA.TRANSP, 0)
    log.set_style_border_width(0, 0)
    log.set_style_pad_all(0, 0)
    log.set_style_pad_row(2, 0)
    log.set_flex_flow(_lv.FLEX_FLOW.COLUMN)
    log.set_scrollbar_mode(_lv.SCROLLBAR_MODE.AUTO)
    _jyd_console_log = log

def _jyd_console_append(kind, text):
    log = _jyd_console_log
    if log is None:
        return
    while log.get_child_count() >= _JYD_CONSOLE_MAX:
        log.get_child(0).delete()
    lb = _lv.label(log)
    lb.set_width(_lv.pct(100))
    lb.set_long_mode(_lv.label.LONG_MODE.WRAP)
    lb.set_style_text_color(
        _lv.color_hex(_JYD_CONSOLE_COLORS.get(kind, 0xB2EBF2)), 0)
    lb.set_text(text)
    log.update_layout()          # 新 label 先有位置，滚动才能算对
    lb.scroll_to_view(False)     # 始终跟到最新输出
"""


class _Tee:
    """替身文本流：写入原样转发给旧流，同时按行进 Console 的队列。

    除 write/flush 外的属性（encoding/fileno/isatty/buffer...）全部转发给
    旧流，subprocess、logging 这类会探测流属性的调用方不会察觉替换。"""

    def __init__(self, console, kind, orig):
        self._console = console
        self._kind = kind
        self._orig = orig
        self._buf = ""       # 未遇到换行的半行

    def write(self, s):
        if not isinstance(s, str):
            s = str(s)
        try:
            self._orig.write(s)              # 终端 / web 日志照常
        except Exception:
            pass
        parts = (self._buf + s).split("\n")
        self._buf = parts.pop()              # 最后一段没换行，留到下次
        for line in parts:
            self._console._push(self._kind, line)
        return len(s)

    def flush(self):
        try:
            self._orig.flush()
        except Exception:
            pass
        if self._buf:                        # print(end="", flush=True) 的半行
            self._console._push(self._kind, self._buf)
            self._buf = ""

    def writable(self):
        return True

    def __getattr__(self, name):
        return getattr(self._orig, name)


class Console:
    """屏幕终端的宿主侧状态：队列、原流、MP 侧控件是否已建。"""

    def __init__(self):
        self._lock = threading.Lock()
        self._lines = collections.deque(maxlen=_MAX_LINES)
        self._dropped = 0
        self._orig = None        # 安装前的 (stdout, stderr)，restore 用
        self._ready = False      # MP 侧面板已建、可以 pump

    @classmethod
    def install(cls):
        """替换 sys.stdout / sys.stderr 为 tee，返回 Console（幂等在调用方保证）。"""
        console = cls()
        console._orig = (sys.stdout, sys.stderr)
        sys.stdout = _Tee(console, "out", sys.stdout)
        sys.stderr = _Tee(console, "err", sys.stderr)
        return console

    def restore(self):
        """把 sys.stdout / sys.stderr 换回原流（幂等）。之后的输出不再进屏幕。"""
        if self._orig is None:
            return
        for stream in (sys.stdout, sys.stderr):
            if isinstance(stream, _Tee):
                stream.flush()
        sys.stdout, sys.stderr = self._orig
        self._orig = None
        self._ready = False

    # ---- 任意线程：入队 ----

    def _push(self, kind, line):
        with self._lock:
            if len(self._lines) == self._lines.maxlen:
                self._dropped += 1           # deque 满时 append 会挤掉最旧的
            self._lines.append((kind, line))

    # ---- 只在 jyd-ui 线程 ----

    def attach(self, m):
        """MP 环境就绪后建面板（jyd-ui 线程，_setup_display 里调用）。"""
        m.exec(_CONSOLE_SRC % {"max_blocks": _MAX_BLOCKS}, capture=False)
        m.call("_jyd_console_create", _PANEL_W, _PANEL_H)
        self._ready = True
        self._push("sys", "console ready: stdout / stderr mirrored here")

    def pump(self, m):
        """每拍 tick 前调用：限量取行、相邻同源合并、逐块跨桥建 label。

        出错时只关掉屏幕镜像（不再 pump），原终端照常——绝不能在这里
        print 报错，否则报错本身又进队列，下一拍再失败，变成每拍刷屏。"""
        if not self._ready:
            return
        with self._lock:
            n = min(_LINES_PER_PUMP, len(self._lines))
            batch = [self._lines.popleft() for _ in range(n)]
            dropped, self._dropped = self._dropped, 0
        if not batch and not dropped:
            return
        blocks = []                          # [(kind, [lines])]，相邻同源合并
        for kind, line in batch:
            if blocks and blocks[-1][0] == kind:
                blocks[-1][1].append(line)
            else:
                blocks.append((kind, [line]))
        if dropped:
            blocks.append(("sys", ["... %d 行输出因积压过多被丢弃" % dropped]))
        try:
            for kind, lines in blocks:
                m.call("_jyd_console_append", kind, "\n".join(lines))
        except Exception as e:
            self._ready = False
            orig_err = self._orig[1] if self._orig else None
            if orig_err is not None:
                try:
                    orig_err.write("jyd: 屏幕终端已停用: %s\n" % e)
                except Exception:
                    pass
