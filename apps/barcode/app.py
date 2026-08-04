"""条形码定位识别（find_barcodes）。

worker 线程跑识别（VisionWorkerApp 模板：识别与 UI 解耦）。
只跟踪第一个条码：四角连线框出（条码 corners 非轴对齐，
不能用矩形描边），label 显示 payload 和类型。
type 值示例：CODE39=12，CODE128=15。"""

from appfw import AppContext
from apps.vision import VisionWorkerApp, LinePool


class BarcodeApp(VisionWorkerApp):
    name = "barcode"

    def on_create(self, ctx: AppContext):
        self._vision_init(ctx)
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)
        lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)

        self._label = lv.label(scr)
        self._label.set_text("barcode: none")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        self._pool = LinePool(ctx, 4)
        self._visible = False

    def process(self, img):
        """worker 内：纯识别，返回拷贝出的标量结果。禁碰 LVGL/桥。
        无码返回 None——也是结果，draw 据此清屏。"""
        mks = img.find_barcodes()
        if not mks:
            return None
        mk = mks[0]   # 多条码只跟第一个，demo 从简
        return mk["corners"], mk["payload"], mk["type"]

    def draw(self, result):
        """UI 线程：按结果更新四角连线和 payload。"""
        if result is not None:
            corners, payload, btype = result
            for i in range(4):
                x1, y1 = corners[i]
                x2, y2 = corners[(i + 1) % 4]
                self._pool.set(i, x1, y1, x2, y2)
            self._label.set_text("%s (type=%d)" % (payload, btype))
            self._visible = True
        elif self._visible:
            for i in range(4):
                self._pool.hide(i)
            self._label.set_text("barcode: none")
            self._visible = False
