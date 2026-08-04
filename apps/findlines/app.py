"""寻线处理（find_lines 霍夫线段）。

灰度图上跑 find_lines，最多画 _MAX_LINES 条绿色线段，
顶部 label 显示当帧线段总数。"""

from appfw import AppContext
from apps.vision import VisionApp, LinePool

_MAX_LINES = 6
# find_lines(roi, x_stride, y_stride, threshold, theta_margin, rho_margin)
# 参数沿用 MaixPy3 官方示例，threshold 需按实际场景调
_ROI = (0, 0, 720, 480)


class FindLinesApp(VisionApp):
    name = "findlines"

    def on_create(self, ctx: AppContext):
        self._vision_init(ctx)
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)   # 透明底，露出摄像头画面
        lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)

        self._label = lv.label(scr)
        self._label.set_text("find_lines: 0")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        self._pool = LinePool(ctx, _MAX_LINES)
        self._shown = 0   # 上一帧实际显示的线段数，用于隐藏多余线段

    def process(self, img):
        lines = img.find_lines(_ROI, 2, 1, 1100, 50, 50) or []
        show = lines[:_MAX_LINES]
        for i, l in enumerate(show):
            self._pool.set(i, l[0], l[1], l[2], l[3])
        for i in range(len(show), self._shown):
            self._pool.hide(i)
        self._shown = len(show)
        self._label.set_text("find_lines: %d" % len(lines))
