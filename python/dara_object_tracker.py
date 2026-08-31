"""DARA 触控框选 FearTrack 单目标跟踪示例。

按住屏幕拖出一个框，松手后用该区域初始化模板；点击一次屏幕取消。
后续帧不运行检测模型，直接由 FearTrack 做视觉跟踪。
"""

from dara import camera, lv, nn

MODEL = "feartrack.mud"

lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

# A dedicated transparent object receives every touch in the camera area.
# LVGL events stay inside MicroPython; lv.bind_touch() exports scalar data.
touch_surface = lv.obj(scr)
touch_surface.set_pos(0, 0)
touch_surface.set_size(720, 480)
touch_surface.set_style_bg_opa(lv.OPA.TRANSP, 0)
touch_surface.set_style_border_width(0, 0)
touch_surface.set_style_pad_all(0, 0)
touch_surface.add_flag(lv.obj.FLAG.CLICKABLE)
touch_surface.remove_flag(lv.obj.FLAG.SCROLLABLE)
lv.bind_touch(touch_surface)

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.set_pos(8, 8)

select_rect = lv.obj(scr)
select_rect.set_style_bg_opa(lv.OPA.TRANSP, 0)
select_rect.set_style_border_color(lv.palette_main(lv.PALETTE.YELLOW), 0)
select_rect.set_style_border_width(2, 0)
select_rect.set_style_radius(0, 0)
select_rect.set_style_pad_all(0, 0)
select_rect.remove_flag(lv.obj.FLAG.CLICKABLE)
select_rect.add_flag(lv.obj.FLAG.HIDDEN)

track_rect = lv.obj(scr)
track_rect.set_style_bg_opa(lv.OPA.TRANSP, 0)
track_rect.set_style_border_color(lv.palette_main(lv.PALETTE.GREEN), 0)
track_rect.set_style_border_width(3, 0)
track_rect.set_style_radius(0, 0)
track_rect.set_style_pad_all(0, 0)
track_rect.remove_flag(lv.obj.FLAG.CLICKABLE)
track_rect.add_flag(lv.obj.FLAG.HIDDEN)
track_text = lv.label(scr)
track_text.set_style_text_color(lv.color_white(), 0)
track_text.add_flag(lv.obj.FLAG.HIDDEN)

# Keep the transparent input target above all non-interactive overlays.
touch_surface.move_foreground()

tracker = nn.load(MODEL)
drag_start = None
drag_current = None
selection_requested = None
message = "drag a box to select"


def _set_rect(obj, x1, y1, x2, y2):
    x1, x2 = sorted((int(x1), int(x2)))
    y1, y2 = sorted((int(y1), int(y2)))
    obj.set_pos(x1, y1)
    obj.set_size(max(2, x2 - x1), max(2, y2 - y1))
    obj.remove_flag(lv.obj.FLAG.HIDDEN)


def on_pressed(x, y):
    global drag_start, drag_current, message
    drag_start = (x, y)
    drag_current = (x, y)
    tracker.reset()
    message = "drag from %d,%d" % (x, y)


def on_pressing(x, y):
    global drag_current
    if drag_start is not None:
        drag_current = (x, y)


def on_released(x, y):
    global drag_start, drag_current, selection_requested, message
    if drag_start is None:
        return
    start = drag_start
    drag_start = None
    drag_current = None
    if abs(x - start[0]) < 8 and abs(y - start[1]) < 8:
        tracker.reset()
        message = "selection cleared"
    else:
        selection_requested = (start[0], start[1], x, y)
        message = "target %d,%d -> %d,%d" % (start[0], start[1], x, y)


def process_touch_events():
    while True:
        event = lv.read_touch()
        if event is None:
            return
        phase, x, y = event
        if phase == "pressed":
            on_pressed(x, y)
        elif phase == "moving":
            on_pressing(x, y)
        elif phase == "released":
            on_released(x, y)

while True:
    with camera.read() as frame:
        if selection_requested is not None:
            target = selection_requested
            selection_requested = None
            try:
                tracker.initialize(frame, *target)
                message = "tracking"
            except RuntimeError as error:
                print("tracker initialize failed:", error)
                message = "target initialization failed"
        selected = tracker.run(frame) if tracker.ready and drag_start is None else None

    if drag_start is not None and drag_current is not None:
        _set_rect(select_rect, drag_start[0], drag_start[1],
                  drag_current[0], drag_current[1])
    else:
        select_rect.add_flag(lv.obj.FLAG.HIDDEN)

    if selected is not None:
        box = selected.box
        _set_rect(track_rect, box.x1, box.y1, box.x2, box.y2)
        track_text.set_text("%s %.2f" % (
            "tracked" if selected.tracked else "lost", selected.confidence))
        track_text.set_pos(int(round(box.x1)),
                           max(0, int(round(box.y1 - 20))))
        track_text.remove_flag(lv.obj.FLAG.HIDDEN)
        status.set_text("%s  %.1f ms" % (message, selected.total_ms))
    else:
        track_rect.add_flag(lv.obj.FLAG.HIDDEN)
        track_text.add_flag(lv.obj.FLAG.HIDDEN)
        status.set_text("%s" % message)
    lv.show()
    process_touch_events()
