"""jyd 内部运行时（进程内单例）：显示通路 + 嵌入 MicroPython + show 心跳 + 退出清理。

通路与初始化顺序复制改造自 ai_cycle/app.py（已验证的独立宿主全套链路）：

    MediaLink(live -> grp1 -> VO)                   幂等建链（已被绑就跳过）；
                                                    live 段可换源/遮挡，见
                                                    camera.preview("front"/
                                                    "rear"/"off")
    -> OSD 双缓冲（persistent_pair，常驻映射）
    -> mpy.set_flush_buf / set_flush_callback        必须先于 MicroPython 环境：
                                                     drive 的 Display 在 import 时
                                                     就取 get_flush_buf 建 LVGL display
    -> Mpyc + MicroPython 最小环境（lv.init + fs_driver + drive 显示驱动）
    -> live 相机保活（只 open 不读，预览通路需要，与 launcher/ai_cycle 一致）

VO 在首帧 flush 后才 enable（避免上电垃圾帧）。UI 心跳由用户显式调
lv.show() 驱动：按真实流逝毫秒 tick_inc + task_handler，完全单线程。

环境变量开关：

    JYD_RUN_SOURCE=web   叠加左上角退出按钮（点按干净退出进程）
    JYD_LV_USE=0         托管 UI 模式：import jyd 即拉起 UI 线程（显示
                         初始化 + show 循环都在该线程，见 __init__.py），
                         用户脚本不驱动 lv（也别再碰 lv 对象）；退出
                         按钮会先停 UI 线程，再给主线程注入
                         KeyboardInterrupt，走 atexit 完整拆除

与 launcher/ai_cycle 互斥：VO/OSD/相机通道是独占资源，跑 jyd 脚本前先停它们。
"""

import atexit
import os
import signal
import threading
import _thread
import time

import mpy      # 仅用于显示通路（set_flush_buf / set_flush_callback）
import tdl_py

from ._mpyc import Mpyc

_HEAP_SIZE = 4 * 1024 * 1024
_OSD_HANDLE = 202
#: show 的单次 tick 上限：长阻塞（慢推理等）后不让动画/超时一次性跳大步
_TICK_CLAMP_MS = 100

#: MicroPython 侧驱动捆绑目录（fs_driver.py + drive/ 显示与触摸 indev），
#: 随包走，不依赖板上 /root/mpy
_MP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_mp")

#: 中文字体（LVGL binfont，随包捆绑），注入 MP 环境做默认 theme 字体
_FONT_CN = os.path.join(_MP_DIR, "res", "font", "siyun_blod.bin")

#: MicroPython 最小环境（= launcher/mpy_env.py 去掉 mpy_home 菜单）。
#: mp_dir 注入捆绑驱动目录，插到 sys.path 最前：cwd 或旧部署目录下的
#: 同名 drive/fs_driver 不会被误加载。drive 导入后 display 已建，
#: 用中文 binfont 初始化默认 theme（font 注入绝对路径，经 fs_driver
#: 的 'L' 盘读取）
_MP_ENV = r"""
import lvgl as lv
import sys
sys.path.insert(0, %(mp_dir)r)
import fs_driver

lv.init()
fs_drv = lv.fs_drv_t()
fs_driver.fs_register(fs_drv, 'L')

import drive

font_cn = lv.binfont_create('L:' + %(font)r)
if font_cn is None:
    raise RuntimeError('中文字体加载失败: ' + %(font)r)
disp = lv.display_get_default()
disp.set_theme(lv.theme_default_init(
    disp, lv.palette_main(lv.PALETTE.BLUE),
    lv.palette_main(lv.PALETTE.RED), True, font_cn))

# lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
# lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
"""

#: web 启动（JYD_RUN_SOURCE=web）时叠加的退出按钮（仿 launcher appfw 的
#: _MP_PRELUDE 样式）。点按 -> 宿主 _request_exit 置位 -> show() 抛
#: SystemExit -> atexit 走 close() 完整拆除（OSD destroy / 相机 close /
#: flush 断开），不残留 RGN handle。不能在 MP 回调栈里直接拆：那会在
#: tick 调用栈内部 deinit MicroPython
_EXIT_BTN_SRC = r"""
import lvgl as lv
import mpy_embed as _jyd_embed

_jyd_exit_btn = lv.button(lv.layer_top())
_jyd_exit_btn.set_size(96, 60)
_jyd_exit_btn.set_pos(8, 8)
_jyd_exit_btn.set_style_bg_color(lv.color_hex(0xB71C1C), 0)
_jyd_exit_btn.set_style_bg_opa(lv.OPA._80, 0)
_jyd_exit_lbl = lv.label(_jyd_exit_btn)
_jyd_exit_lbl.set_text(lv.SYMBOL.CLOSE)
_jyd_exit_lbl.center()
_jyd_exit_btn.add_event_cb(lambda e: _jyd_embed.get("_jyd_exit")(),
                           lv.EVENT.CLICKED, None)
"""


class _Runtime:
    def __init__(self):
        self.m = None                 # Mpyc 实例，ensure_display 后可用
        self._media_links = []        # 本进程建立的 grp1->VO 链路和 VoOutput，
                                      # 运行期必须持有（提前析构即断链黑屏），
                                      # 退出时由 _teardown 显式 unbind/关 VO
        self._preview_link = None     # live -> grp1 的 bind（可换源，单独管理）
        self._preview_rear = False    # 期望的预览源：False 前摄 / True 后摄
        self._preview_show = None     # None 未指定（不动 opa）/ True 显示 / False 遮挡
        self._osd = None
        self._flush_bufs = None       # persistent canvas 对，防常驻映射被 GC
        self._vo_opened = False
        self._display_ready = False
        self._last_show = None
        self._exit_callbacks = []     # 相机等外设的 close，先于显示通路拆除
        self._cleanup_registered = False
        self._exit_requested = False  # web 退出按钮置位，show() 检测
        self._flush_error = None      # _on_flush 内捕获的异常，tick 后重抛
        self._mp_thread = None        # 初始化 MicroPython 的线程 id
        # JYD_LV_USE=0：jyd 自带 UI 心跳线程（显示初始化 + show 循环都在
        # 该线程，用户脚本不驱动 lv）
        self._ui_thread = None
        self._ui_stop = threading.Event()

    # ---- 退出清理 ----

    def on_exit(self, fn):
        """登记进程退出回调（相机 close 等），先于显示通路拆除执行。"""
        self._register_cleanup()
        self._exit_callbacks.append(fn)

    def _register_cleanup(self):
        if not self._cleanup_registered:
            self._cleanup_registered = True
            atexit.register(self.close)

    def close(self):
        """atexit：屏蔽信号后完整拆除。

        先屏蔽信号：清理中再来 Ctrl+C 会打断 OSD destroy，RGN handle
        泄漏后只能重启恢复（进程本来就在退出，屏蔽无副作用）。"""
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            try:
                signal.signal(sig, signal.SIG_IGN)
            except Exception:
                pass
        self._stop_ui_thread()   # 先停 tick，teardown 拆画布时不能有并发渲染
        self._teardown()

    def _stop_ui_thread(self):
        t = self._ui_thread
        if (t is not None and t.is_alive()
                and t is not threading.current_thread()):
            self._ui_stop.set()
            t.join(timeout=2.0)   # show(fps=30) 一帧内醒来，正常远快于超时
        self._ui_thread = None

    def _teardown(self):
        """关 VO -> 停外设 -> 断 flush -> 灭 MicroPython -> 销毁 OSD ->
        拆媒体链路（幂等，不动信号）。"""
        # 先关本进程打开的 VO，停止扫描输出：屏幕一次性变黑，之后拆
        # OSD/相机/链路的中间状态（叠层消失、画面冻结）不会闪上屏
        vos = [x for x in self._media_links if isinstance(x, tdl_py.VoOutput)]
        self._media_links = [x for x in self._media_links
                             if not isinstance(x, tdl_py.VoOutput)]
        for vo in vos:
            try:
                vo.close()
            except Exception as e:
                print("jyd: vo close error: %s" % e)
        for fn in self._exit_callbacks:
            try:
                fn()
            except Exception as e:
                print("jyd: exit callback error: %s" % e)
        self._exit_callbacks = []
        # 无条件断开 flush 通道：部分初始化失败时也可能已注册
        # （悬挂指向已销毁画布的映射），未注册时清除无害
        try:
            mpy.set_flush_callback(None)
        except Exception:
            pass
        try:
            mpy.set_flush_buf(None)
        except Exception:
            pass
        if self.m is not None:
            # 只在初始化 MicroPython 的线程上 deinit：MP 的 C 栈锚点/线程
            # 状态绑在那个线程，跨线程 deinit 必段错误（mp_cstack_init_
            # with_sp_here 写无效 TLS）；跨线程时跳过，堆随进程退出由 OS 回收
            if threading.get_ident() == self._mp_thread:
                try:
                    self.m.close()
                except Exception as e:
                    print("jyd: micropython deinit error: %s" % e)
            self.m = None
        if self._osd is not None:
            try:
                self._osd.destroy()
            except Exception as e:
                print("jyd: osd destroy error: %s" % e)
            self._osd = None
        self._flush_bufs = None
        self._display_ready = False
        # 显式解绑本进程建立的媒体链路（启动时已存在而复用的不在列表里，
        # 不会误拆别人的；VO 已在开头关掉）。不能依赖解释器退出时的析构：
        # nanobind 对象在模块清理阶段析构顺序不保证、可能不执行，bind
        # 残留会让退出后链路仍在流动。反向拆：先解 grp1->VO，preview
        # 段（live->grp1）最先建、最后解
        for link in reversed(self._media_links):
            try:
                link.unbind()
            except Exception as e:
                print("jyd: media link teardown error: %s" % e)
        self._media_links = []
        if self._preview_link is not None:      # live->grp1 最先建，最后解
            try:
                self._preview_link.unbind()
            except Exception as e:
                print("jyd: media link teardown error: %s" % e)
            self._preview_link = None
        self._vo_opened = False

    # ---- 显示 + LVGL ----

    def ensure_display(self):
        """首次触碰 lv 时的懒初始化，幂等。失败时回滚已建部分再抛。

        JYD_LV_USE=0 时改为托管模式：显示初始化和 show 循环都跑在
        jyd 自带的 UI 线程里（本调用只负责拉起，不等就绪），
        用户脚本不驱动 lv。"""
        if self._display_ready:
            return self
        if os.environ.get("JYD_LV_USE") == "0":
            return self._ensure_display_threaded()
        self._register_cleanup()
        try:
            self._setup_display()
        except Exception as e:
            # 回滚已建部分。进程未必在退出（用户可能捕获异常继续跑），
            # 不走 close()——那会把 SIGINT 屏蔽掉
            self._teardown()
            raise RuntimeError(
                "jyd 显示通路初始化失败（VO/OSD/相机通道是独占资源，"
                "确认 launcher / ai_cycle 等宿主已停止）: %s" % e) from e
        return self

    def _ensure_display_threaded(self):
        """JYD_LV_USE=0：拉起 UI 心跳线程（幂等，不等待初始化完成）。"""
        if self._ui_thread is None:
            self._register_cleanup()
            self._ui_stop = threading.Event()
            t = threading.Thread(target=self._ui_main, name="jyd-ui",
                                 daemon=True)
            self._ui_thread = t
            t.start()
        return self

    def _ui_main(self):
        """UI 线程主体：初始化显示通路 -> show 循环 -> 退出时唤醒主线程。

        显示初始化在本线程执行，MicroPython 的栈/线程状态因此绑在本
        线程（teardown 在主线程跑时会自动跳过 mpy.deinit）。退出按钮
        置位后先退出循环（停 tick），再 interrupt_main() 给主线程注入
        KeyboardInterrupt——主线程按正常流程退出，atexit 完整拆除。"""
        try:
            self._setup_display()
        except Exception as e:
            try:
                self._teardown()
            except Exception:
                pass
            self._ui_thread = None
            return
        while not self._ui_stop.is_set():
            try:
                self.show(fps=30)
            except SystemExit:
                break                      # 退出按钮：停止 tick 后通知主线程
            except Exception as e:
                print("jyd: ui thread error: %s" % e)
                time.sleep(0.05)           # 防异常空转
        if self._exit_requested and not self._ui_stop.is_set():
            _thread.interrupt_main()       # 主线程收到 KeyboardInterrupt

    def _setup_display(self):
        # 1) 媒体链路：live（前摄 grp0/ch2 或后摄 grp3/ch2）-> grp1 -> VO
        #    （幂等；预览段按 _preview_rear 对齐，可随时换源）
        self._apply_preview_source()
        if tdl_py.get_bind_source_vo(0, 0) is None:
            link = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
            link.bind()
            self._media_links.append(link)

        # 2) OSD 双缓冲 + flush 通道（先于 MicroPython 环境）。
        destroy_rgn = getattr(tdl_py, "rgn_destroy", None)
        if destroy_rgn is not None:
            destroy_rgn(_OSD_HANDLE, 1, 0)
        osd = tdl_py.Osd(handle=_OSD_HANDLE, canvas_count=2)
        osd.create()
        self._osd = osd
        osd.attach(group=1, channel=0, layer=1)
        back, front = osd.persistent_pair()
        self._flush_bufs = (back, front)
        mpy.set_flush_buf(back.data, front.data)
        osd.set_visible(True)
        mpy.set_flush_callback(self._on_flush, bytes_per_pixel=4)

        # 3) MicroPython + LVGL 环境（capture=False：崩溃时输出不丢），
        #    fs_driver/drive 从包内 _mp 目录加载
        self._mp_thread = threading.get_ident()   # MP 栈/TLS 绑在本线程
        self.m = Mpyc(heap_size=_HEAP_SIZE)
        self.m.exec(_MP_ENV % {"mp_dir": _MP_DIR, "font": _FONT_CN},
                    capture=False)

        # web 启动：叠加退出按钮，点按即干净退出（无终端可 Ctrl+C 的场景）
        if os.environ.get("JYD_RUN_SOURCE") == "web":
            mpy.register(self._request_exit, name="_jyd_exit")
            self.m.exec(_EXIT_BTN_SRC, capture=False)

        # 建链前调过 camera.preview() 的话，此刻应用显示/遮挡状态
        self._apply_preview_visibility()

        self._display_ready = True

        # 4) 预览通路保活：live 相机只 open 不读（进程退出自动 close）
        from . import camera
        camera.live()

    # ---- 预览源（屏幕底层视频）切换 ----

    def set_preview(self, source="front"):
        """三态预览开关："front"/False 前摄、"rear"/True 后摄、"off"/None 遮挡。

        显示态自动把 screen_active/layer_bottom 背景透明（bg_opa=0）露出
        视频；遮挡态置回不透明（bg_opa=255）。注意"off"不解绑视频链路：
        OSD/UI 依附 grp1 的视频帧，没有帧流动连 UI 都不显示，所以视频
        仍在底层流动，仅被不透明底盖住。显示通路未建立时只记录期望
        （建链时生效）；已建立时立即生效。"""
        if source in (None, "off"):
            show = False
        elif source in (True, "rear"):
            show, self._preview_rear = True, True
        elif source in (False, "front"):
            show, self._preview_rear = True, False
        else:
            raise ValueError(
                'preview source 只接受 "front"/"rear"/"off"'
                "（或 False/True/None），拿到: %r" % (source,))
        self._preview_show = show
        if self._display_ready:
            if show:
                self._apply_preview_source()
            self._apply_preview_visibility()

    def _apply_preview_visibility(self):
        """按 _preview_show 设置 screen/layer_bottom 底色透明度（幂等）。

        opa=0 露出底层视频，opa=255 遮住。screen_active 是"当前"屏幕：
        用户 screen_load 换屏后需重调 preview()。未指定（None）时不动，
        保留 _MP_ENV/用户脚本自己的设置。"""
        if self._preview_show is None or self.m is None:
            return
        opa = 0 if self._preview_show else 255
        self.m.exec(
            "import lvgl as lv\n"
            "lv.screen_active().set_style_bg_opa(%d, lv.STATE.DEFAULT)\n"
            "lv.layer_bottom().set_style_bg_opa(%d, lv.STATE.DEFAULT)\n"
            % (opa, opa), capture=False)

    def _apply_preview_source(self):
        """把 grp1 的 bind 源对齐到期望的 live 通道（幂等）。

        MediaLink.unbind() 只对 bind 过的对象生效，所以外来残留 bind
        （上个进程未走清理，如 kill -9）先按同参数重复 bind() 收编成
        本进程对象（小核对重复 bind 返回成功即可），再解绑换源。"""
        src_grp = (tdl_py.REAR_GROUP if self._preview_rear
                   else tdl_py.CAPTURE_GROUP)
        want = ("vpss", src_grp, tdl_py.LIVE_CHANNEL)
        cur = tdl_py.get_bind_source_vpss(1, 0)

        if cur == want:
            if self._preview_link is None:      # 外来但同源：收编，退出可清
                try:
                    link = tdl_py.MediaLink.vpss_to_vpss(
                        src_grp, tdl_py.LIVE_CHANNEL, 1, 0)
                    link.bind()
                    self._preview_link = link
                except Exception as e:
                    # print("jyd: 预览链路收编失败（沿用外部 bind）: %s" % e)
                    pass
            return

        old = self._preview_link
        if old is None and cur is not None:     # 外来且源不同：先收编再解绑
            if cur[0] != "vpss":
                raise RuntimeError("grp1 预览源异常，无法切换: %r" % (cur,))
            old = tdl_py.MediaLink.vpss_to_vpss(cur[1], cur[2], 1, 0)
            try:
                old.bind()
            except Exception as e:
                raise RuntimeError(
                    "grp1 已被外部绑到 %r 且无法收编（%s）：确认 launcher/"
                    "ai_cycle 已停，或重启板子清掉残留后再试" % (cur, e))
        if old is not None:
            old.unbind()
        self._preview_link = None
        link = tdl_py.MediaLink.vpss_to_vpss(src_grp, tdl_py.LIVE_CHANNEL, 1, 0)
        link.bind()
        self._preview_link = link

    def _request_exit(self):
        """web 退出按钮的宿主回调：只置位，不在 MP 回调栈里做任何拆除。"""
        self._exit_requested = True

    def _on_flush(self, x1, y1, x2, y2, data):
        # 这是 MP/原生回调：任何异常（含 Ctrl+C 的 KeyboardInterrupt）都不
        # 能跨出回调边界，否则被 C 侧吞成 "Exception ignored"。暂存起来，
        # 等 tick() 返回后的普通 Python 调用栈里重抛（见 show()）
        try:
            self._osd.update()
            # 首帧渲染完成后才 enable VO，避免显示未初始化的画布
            if not self._vo_opened and not tdl_py.vo_is_enabled(0):
                self._vo_opened = True
                vo = tdl_py.VoOutput()
                vo.open()
                self._media_links.append(vo)
        except BaseException as exc:
            self._flush_error = exc

    def show(self, fps=30):
        """推进一次 UI：真实流逝毫秒 tick_inc + task_handler + 渲染/触摸。

        fps 给定时限速（距上次 show 不足一帧间隔则 sleep 补足），
        适合没有相机 read 之类阻塞源的纯 UI 循环，防 CPU 空转。

        JYD_LV_USE=0 托管模式下心跳由 jyd 的 UI 线程负责：其他线程调
        show() 只确保通路就绪，直接返回（防双驱动）。"""
        self.ensure_display()
        if (self._ui_thread is not None
                and threading.current_thread() is not self._ui_thread):
            return
        now = time.monotonic()
        if fps and self._last_show is not None:
            wait = self._last_show + 1.0 / fps - now
            if wait > 0:
                time.sleep(wait)
                now = time.monotonic()
        if self._last_show is None:
            ms = 16
        else:
            ms = int((now - self._last_show) * 1000)
            ms = max(0, min(ms, _TICK_CLAMP_MS))
        self._last_show = now
        self.m.tick(ms)
        # 已出 MP 调用栈：先把 flush 回调里捕获的异常（含 Ctrl+C）重抛
        # 回普通 Python 栈，让 KeyboardInterrupt 走正常退出/atexit 清理
        if self._flush_error is not None:
            exc, self._flush_error = self._flush_error, None
            raise exc
        # 退出按钮在 tick 内置位；出了 MP 调用栈再抛，走正常解释器退出
        # -> atexit close() 完整拆除（OSD/相机/VO 链路）
        if self._exit_requested:
            print("jyd: exit button pressed, shutting down")
            raise SystemExit(0)


_rt = _Runtime()


def runtime() -> _Runtime:
    return _rt
