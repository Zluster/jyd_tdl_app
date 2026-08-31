"""DARA standalone hand gesture recognition demo."""

from dara import camera, lv, nn

MODEL = "hand_gesture.mud"
MAX_HANDS = 2
POINTS_PER_HAND = 21


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.align(lv.ALIGN.TOP_MID, 0, 8)
status.set_text("gesture: show your hand")

rects, texts = [], []
for _ in range(MAX_HANDS):
    rect = lv.obj(scr)
    rect.set_style_bg_opa(lv.OPA.TRANSP, 0)
    rect.set_style_border_color(lv.palette_main(lv.PALETTE.BLUE), 0)
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

# Show every raw hand landmark. The dots are preallocated so this debug view
# has no per-frame LVGL object allocation.
keypoint_dots = []
for _ in range(MAX_HANDS * POINTS_PER_HAND):
    dot = lv.obj(scr)
    dot.set_size(7, 7)
    dot.set_style_bg_color(lv.palette_main(lv.PALETTE.ORANGE), 0)
    dot.set_style_bg_opa(lv.OPA.COVER, 0)
    dot.set_style_border_width(0, 0)
    dot.set_style_radius(4, 0)
    dot.remove_flag(lv.obj.FLAG.CLICKABLE)
    dot.add_flag(lv.obj.FLAG.HIDDEN)
    keypoint_dots.append(dot)

recognizer = nn.load(MODEL, threshold=0.35, max_hands=MAX_HANDS)

while True:
    with camera.read() as frame:
        hands = recognizer.run(frame).hands

    visible = min(MAX_HANDS, len(hands))
    visible_points = 0
    for index, hand in enumerate(hands[:MAX_HANDS]):
        x1, y1 = hand.box.x1, hand.box.y1
        x2, y2 = hand.box.x2, hand.box.y2
        x1, x2 = sorted((x1, x2))
        y1, y2 = sorted((y1, y2))
        rects[index].set_pos(x1, y1)
        rects[index].set_size(max(2, x2 - x1), max(2, y2 - y1))
        rects[index].remove_flag(lv.obj.FLAG.HIDDEN)
        texts[index].set_text("%s %.2f" % (hand.label, hand.score))
        texts[index].set_pos(x1, max(0, y1 - 20))
        texts[index].remove_flag(lv.obj.FLAG.HIDDEN)

        for point in hand.keypoints[:POINTS_PER_HAND]:
            x, y = point.x, point.y
            keypoint_dots[visible_points].set_pos(x - 3, y - 3)
            keypoint_dots[visible_points].remove_flag(lv.obj.FLAG.HIDDEN)
            visible_points += 1

    for index in range(visible, MAX_HANDS):
        rects[index].add_flag(lv.obj.FLAG.HIDDEN)
        texts[index].add_flag(lv.obj.FLAG.HIDDEN)

    for index in range(visible_points, len(keypoint_dots)):
        keypoint_dots[index].add_flag(lv.obj.FLAG.HIDDEN)

    status.set_text("gesture: %s  points=%d" % (
        hands[0].label if hands else "looking...", visible_points))
    lv.show()
