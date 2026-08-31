"""DARA standalone multi-object tracking and vertical-line counting demo."""

from dara import camera, lv, nn

MODEL = "yolov8n_det_coco80.mud"
THRESHOLD = 0.25
TRACK_CLASS_ID = 0       # COCO person; set to None to track every class.
LINE_X = 360
MAX_TRACKS = 8


def screen_rect(box):
    """Convert nn's screen-space float box to LVGL integer geometry."""
    x = int(round(box.x1))
    y = int(round(box.y1))
    width = max(2, int(round(box.width)))
    height = max(2, int(round(box.height)))
    return x, y, width, height


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.align(lv.ALIGN.TOP_MID, 0, 8)

line = lv.obj(scr)
line.set_size(3, 480)
line.set_pos(LINE_X - 1, 0)
line.set_style_bg_color(lv.palette_main(lv.PALETTE.YELLOW), 0)
line.set_style_bg_opa(lv.OPA.COVER, 0)
line.set_style_border_width(0, 0)
line.remove_flag(lv.obj.FLAG.CLICKABLE)

rects, texts = [], []
for _ in range(MAX_TRACKS):
    rect = lv.obj(scr)
    rect.set_style_bg_opa(lv.OPA.TRANSP, 0)
    rect.set_style_border_color(lv.palette_main(lv.PALETTE.GREEN), 0)
    rect.set_style_border_width(2, 0)
    rect.set_style_radius(0, 0)
    rect.set_style_pad_all(0, 0)
    rect.remove_flag(lv.obj.FLAG.CLICKABLE)
    rect.add_flag(lv.obj.FLAG.HIDDEN)
    rects.append(rect)

    text = lv.label(scr)
    text.set_style_text_color(lv.color_white(), 0)
    text.add_flag(lv.obj.FLAG.HIDDEN)
    texts.append(text)

detector = nn.load(MODEL, threshold=THRESHOLD)
tracker = nn.ObjectTracker(high_score=0.45, low_score=0.15,
                           iou_threshold=0.30, max_missed=30)
counter = nn.LineCounter(LINE_X)

while True:
    with camera.read() as frame:
        detections = detector.run(frame)

    boxes = detections.boxes
    if TRACK_CLASS_ID is not None:
        boxes = [box for box in boxes if box.class_id == TRACK_CLASS_ID]
    tracks = tracker.update(boxes)
    counter.update(tracks)

    for index, track in enumerate(tracks[:MAX_TRACKS]):
        box = track.box
        x, y, width, height = screen_rect(box)
        rects[index].set_pos(x, y)
        rects[index].set_size(width, height)
        rects[index].remove_flag(lv.obj.FLAG.HIDDEN)
        texts[index].set_text("ID %d %s %.2f" % (
            track.track_id, detections.label_of(box.class_id), box.score))
        texts[index].set_pos(x, max(0, y - 20))
        texts[index].remove_flag(lv.obj.FLAG.HIDDEN)
    for index in range(min(len(tracks), MAX_TRACKS), MAX_TRACKS):
        rects[index].add_flag(lv.obj.FLAG.HIDDEN)
        texts[index].add_flag(lv.obj.FLAG.HIDDEN)

    status.set_text("L->R %d  R->L %d  active %d" % (
        counter.left_to_right, counter.right_to_left, len(tracks)))
    lv.show()
