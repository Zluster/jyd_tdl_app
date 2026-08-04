"""区域颜色获取（get_blob_color）。

灰度图中心开一块 ROI，get_blob_color 统计主色，
右上角色块实时显示该颜色，label 显示 RGB 值。
灰度源下 R=G=B，实际看到的是区域亮度。"""

from appfw import AppContext
from apps.vision import VisionApp

_ROI = (350, 230, 20, 20)   # x, y, w, h（720x480 中心）


class BlobColorApp(VisionApp):
    name = "blobcolor"

    def on_create(self, ctx: AppContext):
        self._vision_init(ctx)
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)

        self._label = lv.label(scr)
        self._label.set_text("blob: ---")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        # ROI 指示框：白色描边，固定不动
        x, y, w, h = _ROI
        self._roi_obj = lv.obj(scr)
        self._roi_obj.set_style_bg_opa(lv.OPA.TRANSP, 0)
        self._roi_obj.set_style_border_color(lv.color_white(), 0)
        self._roi_obj.set_style_border_width(1, 0)
        self._roi_obj.set_style_border_opa(lv.OPA.COVER, 0)
        self._roi_obj.set_pos(x, y)
        self._roi_obj.set_size(w, h)

        # 颜色块：右上角，实时显示采样色（左上角是退出按钮，避开）
        self._swatch = lv.obj(scr)
        self._swatch.set_size(44, 44)
        self._swatch.set_pos(660, 16)
        self._swatch.set_style_border_color(lv.color_white(), 0)
        self._swatch.set_style_border_width(1, 0)

        self._rgb = None   # 上一帧颜色，用于变化检测

    def process(self, img):
        colors = img.get_blob_color(_ROI, 0, 0)
        rgb = (int(colors[0]), int(colors[1]), int(colors[2]))
        if rgb == self._rgb:
            return
        self._rgb = rgb
        r, g, b = rgb
        self._swatch.set_style_bg_color(
            self._lv.color_hex((r << 16) | (g << 8) | b), 0)
        self._label.set_text("R=%d G=%d B=%d" % (r, g, b))
