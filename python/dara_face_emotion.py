"""人脸情绪识别：nn.load() 统一接口 + LVGL 实时叠加显示。"""

from dara import camera, lv, nn


MODEL = "face_emotion.mud"
MAX_FACES = 2


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.set_pos(8, 8)

rects, labels = [], []
for _ in range(MAX_FACES):
    rect = lv.obj(scr)
    rect.set_style_bg_opa(lv.OPA.TRANSP, 0)
    rect.set_style_border_color(lv.palette_main(lv.PALETTE.GREEN), 0)
    rect.set_style_border_width(2, 0)
    rect.set_style_radius(0, 0)
    rect.set_style_pad_all(0, 0)
    rect.remove_flag(lv.obj.FLAG.CLICKABLE)
    rect.add_flag(lv.obj.FLAG.HIDDEN)
    rects.append(rect)

    label = lv.label(scr)
    label.set_style_text_color(lv.color_white(), 0)
    label.add_flag(lv.obj.FLAG.HIDDEN)
    labels.append(label)

model = nn.load(MODEL, threshold=0.35, max_faces=MAX_FACES)

while True:
    with camera.read() as frame:
        result = model.run(frame)

    faces = result.faces[:MAX_FACES]
    for index, face in enumerate(faces):
        box = face.box
        x, y = max(0, box.x1), max(0, box.y1)
        rects[index].set_pos(x, y)
        rects[index].set_size(max(2, box.x2 - x), max(2, box.y2 - y))
        rects[index].remove_flag(lv.obj.FLAG.HIDDEN)
        age = "?" if face.age_years < 0 else str(face.age_years)
        labels[index].set_text("%s %.2f %s %s glasses:%s" % (
            face.emotion, face.emotion_score, face.gender_label, age,
            "yes" if face.has_glasses else "no"))
        labels[index].set_pos(x, max(0, y - 20))
        labels[index].remove_flag(lv.obj.FLAG.HIDDEN)

    for index in range(len(faces), MAX_FACES):
        rects[index].add_flag(lv.obj.FLAG.HIDDEN)
        labels[index].add_flag(lv.obj.FLAG.HIDDEN)
    status.set_text("emotion: %d face(s)" % len(faces))
    lv.show()
