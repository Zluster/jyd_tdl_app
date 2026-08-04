"""AprilTag 识别：角度信息 + 三维坐标。

find_apriltags 返回字典：corners 四角、x/y/z_rotation（弧度）、
x/y/z_translation。只跟踪第一个 tag：四角连线框出，
label 显示 id、三轴角度（度）和距离（平移向量模长）。

fx/fy/cx/cy 沿用 MaixII-Dock 6mm 镜头参数按 720x480 缩放，
三维数值只有相对意义，要准需按实际镜头重新标定。"""

from appfw import AppContext
from apps.vision import VisionApp, LinePool

_F_X = (6 / 5.76) * 720   # 焦距 6mm / 感光长 5.76mm × 图宽
_F_Y = (6 / 3.24) * 480   # 焦距 6mm / 感光宽 3.24mm × 图高
_C_X = 720 * 0.5
_C_Y = 480 * 0.5

_DEBUG = True   # 排查段错误用：逐步打点，定位后改 False 或删除


class AprilTagApp(VisionApp):
    name = "apriltag"

    def on_create(self, ctx: AppContext):
        self._vision_init(ctx)
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)

        self._label = lv.label(scr)
        self._label.set_text("apriltag: none")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        self._pool = LinePool(ctx, 4)
        self._visible = False

    def process(self, img):
        if _DEBUG:
            print("[apriltag] find_apriltags...", flush=True)
        mks = img.find_apriltags(families=16, fx=_F_X, fy=_F_Y,
                                 cx=_C_X, cy=_C_Y)
        if _DEBUG:
            print("[apriltag] -> %r" % (mks,), flush=True)
        if mks:
            mk = mks[0]   # 多 tag 只跟第一个，demo 从简
            corners = mk["corners"]
            if _DEBUG:
                print("[apriltag] corners=%r" % (corners,), flush=True)
            for i in range(4):
                x1, y1 = corners[i]
                x2, y2 = corners[(i + 1) % 4]
                self._pool.set(i, x1, y1, x2, y2)
            if _DEBUG:
                print("[apriltag] pooled", flush=True)
            xr = int(mk["x_rotation"] * 57.2958)
            yr = int(mk["y_rotation"] * 57.2958)
            zr = int(mk["z_rotation"] * 57.2958)
            xt = mk["x_translation"]
            yt = mk["y_translation"]
            zt = mk["z_translation"]
            dist = (xt * xt + yt * yt + zt * zt) ** 0.5
            self._label.set_text("id=%d xR=%d yR=%d zR=%d d=%.2f"
                                 % (mk["id"], xr, yr, zr, dist))
            if _DEBUG:
                print("[apriltag] labeled", flush=True)
            self._visible = True
        elif self._visible:
            for i in range(4):
                self._pool.hide(i)
            self._label.set_text("apriltag: none")
            self._visible = False
