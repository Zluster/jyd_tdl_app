"""jyd 内部运行时（进程内单例）：显示通路 + 嵌入 MicroPython + UI 线程 + 退出清理。

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

线程模型：显示通路与 MicroPython 全部建立并运行在 jyd 专职 UI 线程
（jyd-ui）上——MP 的 C 栈/TLS 绑定该线程，且解释器不可重入。UI 线程
主循环按 _UI_FPS 上限自转 tick（真实流逝毫秒 tick_inc + task_handler，
带钳制），两次 tick 的空档消化 Mpyc 的转交队列：用户线程的所有 lv
代理调用都排队转到该线程执行、阻塞取结果（见 _mpyc.Mpyc._submit），
因此 lv 可在任意线程使用；lv.bind 等回调在 UI 线程的 tick 栈里执行。
lv.show() 不再驱动渲染，只保留帧率限速的作用。

VO 在首帧内容就绪后才 enable（避免上电垃圾帧，见 _ensure_vo）。

环境变量开关：

    JYD_RUN_SOURCE=web   叠加左上角退出按钮（点按干净退出进程）

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
#: UI 线程自转帧率上限（tick + 渲染 + 触摸分发的节拍）
_UI_FPS = 100
#: 单次 tick 的毫秒上限：长阻塞后不让动画/超时一次性跳大步
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
#: _MP_PRELUDE 样式）。点按 -> 宿主 _request_exit 置位 -> jyd-ui 循环
#: 退出（先停 tick、在属主线程收尾 MicroPython）-> interrupt_main 给
#: 主线程注入 KeyboardInterrupt -> atexit 走 close() 完整拆除（OSD
#: destroy / 相机 close），不残留 RGN handle。不能在 MP 回调栈里直接
#: 拆：那会在 tick 调用栈内部 deinit MicroPython
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
        self._exit_requested = False  # web 退出按钮置位，UI 循环检测
        self._flush_error = None      # _on_flush 内捕获的异常，UI 循环检测
        self._vo_lock = threading.Lock()  # _ensure_vo 可能被多线程调用
        self._mp_thread = None        # 初始化 MicroPython 的线程 id
        # 专职 UI 线程（jyd-ui）：显示初始化、tick 自转、转交队列消化
        # 都在该线程；用户线程的 lv 调用经 Mpyc 队列转交
        self._ui_thread = None
        self._ui_stop = threading.Event()
        self._ui_error = None         # UI 线程初始化失败的原因

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
        # 先停 jyd-ui 线程：tick 停止、MicroPython 在属主线程内收尾
        # （deinit），teardown 拆画布时不能有并发渲染
        self._stop_ui_thread()
        self._teardown()

    def _stop_ui_thread(self):
        t = self._ui_thread
        if (t is not None and t.is_alive()
                and t is not threading.current_thread()):
            self._ui_stop.set()
            t.join(timeout=2.0)   # 循环最多睡一拍（10ms），正常远快于超时
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
        """首次触碰 lv 的懒初始化（幂等）：拉起 jyd-ui 线程并等通路就绪。

        显示通路与 MicroPython 都在 jyd-ui 线程上建立/运行，本调用只
        负责拉起并等待；初始化失败时（UI 线程已回滚退出）把记录的
        原因在调用方线程重抛。"""
        if self._display_ready:
            return self
        if self._ui_thread is None or not self._ui_thread.is_alive():
            self._register_cleanup()
            self._ui_stop = threading.Event()
            self._ui_error = None
            t = threading.Thread(target=self._ui_main, name="jyd-ui",
                                 daemon=True)
            self._ui_thread = t
            t.start()
        deadline = time.monotonic() + 15.0
        while not self._display_ready:
            t = self._ui_thread
            if t is None or not t.is_alive():
                raise RuntimeError(
                    "jyd 显示通路初始化失败（VO/OSD/相机通道是独占资源，"
                    "确认 launcher / ai_cycle 等宿主已停止）: %s"
                    % (self._ui_error,))
            if time.monotonic() > deadline:
                raise RuntimeError("等待显示通路就绪超时")
            time.sleep(0.02)
        return self

    def wait_display_ready(self, timeout=10.0):
        """兼容旧名：ensure_display 现在本身就等待就绪。"""
        return self.ensure_display()

    def _ui_main(self):
        """jyd-ui 线程主体：建显示通路 -> tick 自转 + 消化转交队列 ->
        收尾（断 flush、在属主线程 deinit MicroPython）。

        退出路径两条：close() 置 _ui_stop（正常退出，主线程 join 后拆
        媒体资源）；退出按钮 / flush 致命错误则本线程收尾后
        interrupt_main() 给主线程注入 KeyboardInterrupt，走 atexit
        完整拆除。"""
        try:
            self._setup_display()
        except Exception as e:
            self._ui_error = e     # ensure_display 在调用方线程重抛
            try:
                self._teardown()   # 回滚已建部分（本线程可安全 deinit MP）
            except Exception:
                pass
            return
        period = 1.0 / _UI_FPS
        next_tick = time.monotonic()
        last = None
        fatal = None
        while not self._ui_stop.is_set():
            now = time.monotonic()
            if now < next_tick:
                # 空档消化用户线程转交的 lv 调用（最多睡到下一拍）
                self.m.service(next_tick - now)
                continue
            ms = 16 if last is None else max(
                0, min(int((now - last) * 1000), _TICK_CLAMP_MS))
            last = now
            next_tick = now + period       # 不追帧：长阻塞后不补 tick
            try:
                self.m.tick(ms)
            except Exception as e:
                print("jyd: ui tick error: %s" % e)
                time.sleep(0.05)           # 防异常空转
            if self._flush_error is not None:
                fatal, self._flush_error = self._flush_error, None
                print("jyd: 显示 flush 失败，进程退出: %s" % fatal)
                break
            if self._exit_requested:
                print("jyd: exit button pressed, shutting down")
                break
        # 收尾必须在本线程（MP 栈/TLS 属主）：先断 flush 通道再 deinit；
        # close() 会唤醒并回绝仍在排队的跨线程调用
        self._display_ready = False
        try:
            mpy.set_flush_callback(None)
        except Exception:
            pass
        try:
            mpy.set_flush_buf(None)
        except Exception:
            pass
        m, self.m = self.m, None
        if m is not None:
            try:
                m.close()
            except Exception as e:
                print("jyd: micropython deinit error: %s" % e)
        if ((self._exit_requested or fatal is not None)
                and not self._ui_stop.is_set()):
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

        bind 表只认 (源, 目标) 通道对，不认建立者：换源时外来残留
        bind（上个进程 kill -9 未清理等）用 MediaLink.force_unbind()
        直接拆掉再绑自己的。同源的外来 bind 沿用、不接管——固件对
        已有源的目标重复 CVI_SYS_Bind 一律失败，"重复 bind 收编"不
        可行，沿用也不妨碍之后换源。"""
        src_grp = (tdl_py.REAR_GROUP if self._preview_rear
                   else tdl_py.CAPTURE_GROUP)
        want = ("vpss", src_grp, tdl_py.LIVE_CHANNEL)
        cur = tdl_py.get_bind_source_vpss(1, 0)

        if cur == want:
            return          # 已是期望源（自己建的或外来沿用皆可）

        old, self._preview_link = self._preview_link, None
        if old is not None:
            old.unbind()                        # 自己建的链路正常解绑
        elif cur is not None:                   # 外来且源不同：强制拆掉
            if cur[0] != "vpss":
                raise RuntimeError("grp1 预览源异常，无法切换: %r" % (cur,))
            stale = tdl_py.MediaLink.vpss_to_vpss(cur[1], cur[2], 1, 0)
            if not hasattr(stale, "force_unbind"):
                raise RuntimeError(
                    "grp1 已被外部绑到 %r，拆除需要 MediaLink.force_unbind，"
                    "当前 tdl_py.so 过旧没有该接口：更新部署 tdl_py.so，"
                    "或重启板子清掉残留 bind" % (cur,))
            stale.force_unbind()
        link = tdl_py.MediaLink.vpss_to_vpss(src_grp, tdl_py.LIVE_CHANNEL, 1, 0)
        link.bind()
        self._preview_link = link

    def _request_exit(self):
        """web 退出按钮的宿主回调：只置位，不在 MP 回调栈里做任何拆除。"""
        self._exit_requested = True

    def _ensure_vo(self):
        """有内容可显示后使能 VO（幂等，可多线程调用）。

        VO 不在初始化时开，是为了避免显示未初始化的画布。原先只挂在
        LVGL 首帧 flush 上；image.show 直绘路径不 tick LVGL，提交完一帧
        后也要能点亮屏幕，所以抽成公共方法。两处调用时画布都已有内容
        （LVGL 刚 flush / OSD 建立时已清成透明 + 用户首帧已提交），
        不会闪垃圾帧。"""
        if self._vo_opened:
            return
        with self._vo_lock:
            if self._vo_opened or tdl_py.vo_is_enabled(0):
                return
            self._vo_opened = True
            vo = tdl_py.VoOutput()
            vo.open()
            self._media_links.append(vo)

    def _on_flush(self, x1, y1, x2, y2, data):
        # 这是 MP/原生回调（jyd-ui 线程 tick 栈内）：任何异常都不能跨出
        # 回调边界，否则被 C 侧吞成 "Exception ignored"。暂存起来，UI
        # 循环在 tick 返回后检测并按致命错误处理（见 _ui_main）
        try:
            self._osd.update()
            # 首帧渲染完成后才 enable VO，避免显示未初始化的画布
            self._ensure_vo()
        except BaseException as exc:
            self._flush_error = exc

    def show(self, fps=None):
        """兼容入口：确保显示通路就绪 + 可选帧率限速（sleep 补足）。

        渲染/触摸由 jyd-ui 线程自转（_UI_FPS 上限），show 不再驱动
        tick。保留它给老脚本控节奏：纯 UI 循环防 CPU 空转；相机循环
        由 read 阻塞起搏，缺省按 100 fps 封顶、不会额外拖慢。"""
        self.ensure_display()
        fps = fps or 100
        now = time.monotonic()
        if self._last_show is not None:
            wait = self._last_show + 1.0 / fps - now
            if wait > 0:
                time.sleep(wait)
                now = time.monotonic()
        self._last_show = now


_rt = _Runtime()


def runtime() -> _Runtime:
    return _rt
