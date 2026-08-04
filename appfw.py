"""appfw: CPython 侧应用框架（应用经 mpyc 使用 LVGL）。

约定（与之前讨论一致）：
- launcher 管理应用生命周期；应用运行时菜单 screen 不再 active（自动隐藏）
- 应用 UI 由应用自己在专属 screen 上构建（mpyc 代理）
- 应用运行期间框架在 layer_top 显示退出按钮（左上角），点击回退菜单
- 切换是"挂起式"的：launch()/request_exit() 只挂起请求，真正的建屏/删屏
  发生在 process() ——由 tick 循环在 task_handler 之外的安全点每帧调用。
  LVGL 事件回调里调用它们是安全的。

应用只需继承 App：

    class MyApp(App):
        name = "my_app"
        def on_create(self, ctx): ...   # 在 ctx.screen 上建 UI
        def on_update(self, dt_ms): ... # 可选，每 tick 调用
        def on_destroy(self): ...       # 释放非 UI 资源（UI 由框架兜底删除）
"""

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    # IDE 补全：类型来自 mpyc/env_stub.pyi（gen_stub.py 生成），运行时不导入
    import tdl_py
    from mpyc import Mpyc
    from mpyc.env_stub import Env, _LvModule, obj_cls

# MicroPython 侧支撑：layer_top 退出按钮 + 事件绑定辅助
_MP_PRELUDE = r"""
import lvgl as lv
import mpy_embed as _appfw_embed

_appfw_exit_btn = lv.button(lv.layer_top())
_appfw_exit_btn.set_size(int(64*1.5), int(40*1.5))
_appfw_exit_btn.set_pos(8, 8)
_appfw_exit_btn.set_style_bg_color(lv.color_hex(0xB71C1C), 0)
_appfw_exit_btn.set_style_bg_opa(lv.OPA._80, 0)
_appfw_exit_btn.add_flag(lv.obj.FLAG.HIDDEN)
_appfw_exit_lbl = lv.label(_appfw_exit_btn)
_appfw_exit_lbl.set_text(lv.SYMBOL.CLOSE)
_appfw_exit_lbl.center()
_appfw_exit_btn.add_event_cb(lambda e: _appfw_embed._appfw_exit(), lv.EVENT.CLICKED, None)

def _appfw_show_exit(show):
    if show:
        _appfw_exit_btn.remove_flag(lv.obj.FLAG.HIDDEN)
    else:
        _appfw_exit_btn.add_flag(lv.obj.FLAG.HIDDEN)

def _appfw_bind(obj, code, host_name):
    # 宿主(CPython)回调不能直接收 lvgl 事件对象（无法跨桥），
    # 由 MicroPython 侧 lambda 吞掉事件参数后无参调用宿主函数。
    f = _appfw_embed.get(host_name)
    obj.add_event_cb(lambda e: f(), code, None)
"""


class App:
    """应用基类。子类实现 on_create / on_destroy（/ 可选 on_update）。"""

    name = "app"

    def on_create(self, ctx: "AppContext"):
        raise NotImplementedError

    def on_update(self, dt_ms: int):
        pass

    def on_destroy(self):
        pass


class AppContext:
    """交给应用的资源包。"""

    # IDE 补全用的类型声明（运行时实际是 mpyc 代理对象）
    m: "Mpyc"
    env: "Env"
    lv: "_LvModule"
    screen: "obj_cls"
    camera: "tdl_py.VpssCamera"

    def __init__(self, fw, app, screen):
        self._fw = fw
        self._app = app
        #: mpyc 实例与代理入口
        self.m = fw.m
        self.env = fw.m.env
        self.lv = fw.m.env.lv
        #: 应用专属 screen（句柄代理），on_create 时已创建、随后被 load
        self.screen = screen
        #: VPSS live 通道摄像头（grp0/ch2，720x480 NV12）。
        #: 框架级资源，AppFramework 创建时打开、程序退出时才 close，
        #: 应用只管用，别自己 close
        self.camera = fw.camera
        self._bound = []   # 本应用注册的宿主回调名，退出时反注册

    def exit(self):
        """应用主动退出（挂起请求，安全点执行）。"""
        self._fw.request_exit()

    def bind(self, obj, event_code, fn):
        """把 LVGL 事件绑定到 CPython 无参回调。

        obj/event_code 用代理（如 ctx.lv.EVENT.CLICKED），fn 为 CPython
        callable（不接收事件对象——事件对象无法跨桥）。
        """
        import mpy
        name = "_appcb_%s_%d" % (self._app.name, len(self._bound))
        mpy.register(fn, name=name)
        self._bound.append(name)
        self.m.proxy_call("_appfw_bind", obj, event_code, name)

    def _release(self):
        import mpy
        for name in self._bound:
            try:
                mpy.unregister(name)
            except Exception:
                pass
        self._bound = []


class AppFramework:
    """应用生命周期调度器。launch() 是进入应用的入口。"""

    ON_UPDATE_ERR_LIMIT = 5   # on_update 连续异常阈值，超过强制退出

    def __init__(self, m):
        self.m = m
        self._app = None
        self._ctx = None
        self._screen = None
        self._pending = []
        self._err_count = 0
        # 菜单 screen：框架初始化时的 active screen，常驻不删
        self._menu_screen = m.env.lv.screen_active()
        # 退出按钮点击 -> 宿主函数 -> 挂起退出请求（回调里只置标志，安全）
        import mpy
        mpy.register(lambda: self.request_exit(), name="_appfw_exit")
        m.exec(_MP_PRELUDE)

        # dark 主题（mpy_home set_theme）把 layer_bottom 重设成不透明黑底，
        # 盖掉 mpy_env 的透明设置；LVGL 在透明屏下找不到不透明顶层对象时
        # 会回退绘制 layer_bottom（lv_refr.c），应用屏黑底即来源于此。
        # 框架级修成透明：菜单屏自身不透明不受影响，应用屏透明才能透出视频
        m.env.lv.layer_bottom().set_style_bg_opa(0, m.env.lv.STATE.DEFAULT)

        # 框架级摄像头：创建框架即打开，失败直接抛（启动阶段暴露）；
        # 只在 close() 里释放，应用进出不重复开关
        import tdl_py
        self.camera: "tdl_py.VpssCamera" = tdl_py.VpssCamera.live()
        self.camera.open()

    def close(self):
        """程序退出时由宿主调用，释放框架级资源（目前只有摄像头）。"""
        if self.camera is not None:
            try:
                self.camera.close()
            except Exception as e:
                print("appfw: camera close error: %s" % e)
            self.camera = None

    # ---- 公开入口 ----

    def launch(self, app: App):
        """进入应用。可在任意时刻调用（含 LVGL 回调内）；
        实际切换发生在下一个 tick 的安全点。"""
        self._pending.append(("launch", app))

    def request_exit(self):
        """退出当前应用（退出按钮/ctx.exit()/宿主代码均走这里）。"""
        self._pending.append(("exit", None))

    @property
    def running(self) -> App:
        """当前运行中的应用，菜单态为 None。"""
        return self._app

    # ---- tick 安全点：由 CPython tick 循环在 task_handler 之后调用 ----

    def process(self, dt_ms: int):
        while self._pending:
            action, arg = self._pending.pop(0)
            if action == "launch":
                if self._app is not None:
                    self._do_exit()
                self._do_launch(arg)
            elif action == "exit" and self._app is not None:
                self._do_exit()

        if self._app is not None:
            try:
                self._app.on_update(dt_ms)
                self._err_count = 0
            except Exception as e:
                self._err_count += 1
                print("appfw: on_update error (%d/%d): %s"
                      % (self._err_count, self.ON_UPDATE_ERR_LIMIT, e))
                if self._err_count >= self.ON_UPDATE_ERR_LIMIT:
                    print("appfw: too many errors, force exit '%s'" % self._app.name)
                    self._do_exit()

    # ---- 内部实现 ----

    def _do_launch(self, app: App):
        lv = self.m.env.lv
        screen = lv.obj(None)          # 应用专属 screen
        ctx = AppContext(self, app, screen)
        try:
            app.on_create(ctx)
        except Exception as e:
            print("appfw: on_create failed for '%s': %s" % (app.name, e))
            ctx._release()
            screen.delete()
            return
        self._app, self._ctx, self._screen = app, ctx, screen
        self._err_count = 0
        lv.screen_load(screen)
        self.m.proxy_call("_appfw_show_exit", True)

    def _do_exit(self):
        app, ctx, screen = self._app, self._ctx, self._screen
        self._app = self._ctx = self._screen = None
        try:
            app.on_destroy()
        except Exception as e:
            print("appfw: on_destroy error for '%s': %s" % (app.name, e))
        finally:
            self.m.proxy_call("_appfw_show_exit", False)
            self.m.env.lv.screen_load(self._menu_screen)   # 恢复菜单
            try:
                screen.delete()                            # 兜底删除应用 UI
            except Exception as e:
                print("appfw: screen delete error: %s" % e)
            ctx._release()
            self.m.exec("import gc; gc.collect()")
