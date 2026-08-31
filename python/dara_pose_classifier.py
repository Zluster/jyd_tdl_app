"""DARA standalone body-pose classification demo with COCO-17 keypoints."""

import math

from dara import camera, lv, nn

MODEL = "pose_classifier.mud"
POINT_COUNT = 17
# COCO pose can hallucinate occluded joints. Keep display and classification
# thresholds independent: visual overlays should show only reliable joints,
# while pose labels require a sufficiently reliable torso/leg geometry.
DISPLAY_SCORE = 0.50
CLASSIFY_SCORE = 0.35

# Standard COCO-17 skeleton edges. An edge is shown only when both endpoints
# have sufficient keypoint confidence in the current frame.
SKELETON_EDGES = (
    (0, 1), (0, 2), (1, 3), (2, 4),
    (5, 6), (5, 7), (7, 9), (6, 8), (8, 10),
    (5, 11), (6, 12), (11, 12),
    (11, 13), (13, 15), (12, 14), (14, 16),
)


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.align(lv.ALIGN.TOP_MID, 0, 8)
status.set_text("pose: loading...")

# Reuse 17 tiny circular controls. They show the returned body keypoints;
# no objects are created in the per-frame loop.
points = []
for _ in range(POINT_COUNT):
    point = lv.obj(scr)
    point.set_size(7, 7)
    point.set_style_bg_color(lv.palette_main(lv.PALETTE.ORANGE), 0)
    point.set_style_bg_opa(lv.OPA.COVER, 0)
    point.set_style_border_width(0, 0)
    point.set_style_radius(4, 0)
    point.remove_flag(lv.obj.FLAG.CLICKABLE)
    point.add_flag(lv.obj.FLAG.HIDDEN)
    points.append(point)

# The detector result currently exposes keypoints rather than its raw person
# box. Draw the visible-keypoint envelope as the person box, and preallocate
# every skeleton line so the frame loop only changes point coordinates.
person_box = lv.obj(scr)
person_box.set_style_bg_opa(lv.OPA.TRANSP, 0)
person_box.set_style_border_color(lv.palette_main(lv.PALETTE.GREEN), 0)
person_box.set_style_border_width(2, 0)
person_box.set_style_radius(0, 0)
person_box.set_style_pad_all(0, 0)
person_box.remove_flag(lv.obj.FLAG.CLICKABLE)
person_box.add_flag(lv.obj.FLAG.HIDDEN)

skeleton_lines = []
for _ in SKELETON_EDGES:
    # DARA's MicroPython bridge cannot marshal lv_line point arrays. A thin
    # rotated object gives the same persistent, allocation-free skeleton edge.
    line = lv.obj(scr)
    line.set_style_bg_color(lv.palette_main(lv.PALETTE.GREEN), 0)
    line.set_style_bg_opa(lv.OPA.COVER, 0)
    line.set_style_border_width(0, 0)
    line.set_style_radius(0, 0)
    line.set_style_pad_all(0, 0)
    line.remove_flag(lv.obj.FLAG.CLICKABLE)
    line.add_flag(lv.obj.FLAG.HIDDEN)
    skeleton_lines.append(line)

classifier = nn.load(MODEL, threshold=CLASSIFY_SCORE)

while True:
    with camera.read() as frame:
        result = classifier.run(frame)

    visible = 0
    mapped_points = [None] * POINT_COUNT
    for index, keypoint in enumerate(result.points[:POINT_COUNT]):
        if keypoint.score < DISPLAY_SCORE:
            continue
        x, y = keypoint.x, keypoint.y
        mapped_points[index] = (x, y)
        points[visible].set_pos(x - 3, y - 3)
        points[visible].remove_flag(lv.obj.FLAG.HIDDEN)
        visible += 1
    for index in range(visible, POINT_COUNT):
        points[index].add_flag(lv.obj.FLAG.HIDDEN)

    for index, (start, end) in enumerate(SKELETON_EDGES):
        first, second = mapped_points[start], mapped_points[end]
        if first is None or second is None:
            skeleton_lines[index].add_flag(lv.obj.FLAG.HIDDEN)
            continue
        dx, dy = second[0] - first[0], second[1] - first[1]
        length = max(2, int(round(math.hypot(dx, dy))))
        line = skeleton_lines[index]
        line.set_size(length, 3)
        # LVGL's default transform pivot is not guaranteed to be the object
        # center. Set it explicitly after each size update so rotation keeps
        # the two endpoints fixed around their geometric midpoint.
        line.set_style_transform_pivot_x(length // 2, 0)
        line.set_style_transform_pivot_y(1, 0)
        line.set_pos(int(round((first[0] + second[0] - length) * 0.5)),
                     int(round((first[1] + second[1] - 3) * 0.5)))
        line.set_style_transform_rotation(
            int(round(math.degrees(math.atan2(dy, dx)) * 10)), 0)
        line.remove_flag(lv.obj.FLAG.HIDDEN)

    if visible >= 2:
        xs = [point[0] for point in mapped_points if point is not None]
        ys = [point[1] for point in mapped_points if point is not None]
        x1, x2 = max(0, min(xs) - 8), min(719, max(xs) + 8)
        y1, y2 = max(0, min(ys) - 8), min(479, max(ys) + 8)
        person_box.set_pos(x1, y1)
        person_box.set_size(max(2, x2 - x1), max(2, y2 - y1))
        person_box.remove_flag(lv.obj.FLAG.HIDDEN)
    else:
        person_box.add_flag(lv.obj.FLAG.HIDDEN)

    status.set_text("pose: %s %.2f  points=%d" %
                    (result.label, result.confidence, visible))
    lv.show()
