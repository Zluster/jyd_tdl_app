"""示例应用：一个标签 + 一个按钮（点击计数），运行在 CPython，
UI 全部通过 mpyc 代理操作 LVGL。"""

from appfw import App, AppContext


class DemoApp(App):
    name = "demo"

    def on_create(self, ctx: AppContext):
        lv, scr = ctx.lv, ctx.screen
        self._ctx = ctx

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)   # 透明底，露出底下的摄像头画面
        lv.layer_bottom().set_style_bg_opa(255, lv.STATE.DEFAULT)

        self._count = 0
        self._label = lv.label(scr)
        self._label.set_text("Hello from CPython app")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.CENTER, 0, -50)

        self._btn = lv.button(scr)
        self._btn.set_size(180, 60)
        self._btn.align(lv.ALIGN.CENTER, 0, 40)
        btn_label = lv.label(self._btn)
        btn_label.set_text("Click me")
        btn_label.center()

        # LVGL 事件 -> CPython 无参回调
        ctx.bind(self._btn, lv.EVENT.CLICKED, self._on_click)


    def on_update(self, dt_ms: int):
        # 同步取帧会阻塞到下一帧到达：UI tick 节奏与摄像头帧率绑定，
        # demo 可接受；标量必须在 with 块内读出，frame 出块即失效
        pass

    def _on_click(self):
        self._count += 1
        self._label.set_text("clicked %d times" % self._count)

    def on_destroy(self):
        # UI 由框架删 screen 兜底；这里只清理非 UI 资源（本例没有）
        pass
