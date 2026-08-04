"""二维码扫描应用。

worker 线程跑 find_qrcodes()（VisionWorkerApp 模板：识别与 UI 解耦），
payload 显示在顶部 label，外框用绿色描边 rect 框出。"""

from appfw import AppContext
from apps.vision import VisionWorkerApp


class QrApp(VisionWorkerApp):
    name = "qrcode"

    def on_create(self, ctx: AppContext):
        self._vision_init(ctx)
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)   # 透明底，扫码时要看到摄像头画面
        lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)

        self._label = lv.label(scr)
        self._label.set_text("scanning...")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        # 二维码外框：透明底 + 绿色描边，无码时隐藏
        self._box_obj = lv.obj(scr)
        self._box_obj.set_style_bg_opa(lv.OPA.TRANSP, 0)
        self._box_obj.set_style_border_color(lv.color_hex(0x00E676), 0)
        self._box_obj.set_style_border_width(3, 0)
        self._box_obj.set_style_border_opa(lv.OPA.COVER, 0)
        self._box_obj.add_flag(lv.obj.FLAG.HIDDEN)

        self._box = None          # 上一帧外框 (x, y, w, h)，用于变化检测
        self._payload = ""

    def process(self, img):
        """worker 内：纯识别，返回拷贝出的标量结果。禁碰 LVGL/桥。"""
        mks = img.find_qrcodes()
        if mks:
            mk = mks[0]   # 多码场景只框第一个，demo 从简
            return (mk["x"], mk["y"], mk["w"], mk["h"]), mk["payload"]
        return None, ""

    def draw(self, result):
        """UI 线程：按结果更新外框和 payload。"""
        box, payload = result
        # 状态不变就不碰 LVGL 代理，减少每帧跨桥流量
        if box == self._box and payload == self._payload:
            return
        self._box, self._payload = box, payload

        if box is None:
            self._box_obj.add_flag(self._lv.obj.FLAG.HIDDEN)
            self._label.set_text("scanning...")
        else:
            x, y, w, h = box
            self._box_obj.remove_flag(self._lv.obj.FLAG.HIDDEN)
            self._box_obj.set_pos(x, y)
            self._box_obj.set_size(w, h)
            self._label.set_text(payload)
