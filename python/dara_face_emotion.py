"""Live face emotion display using SCRFD + attribute/emotion NPU model."""

from dara import camera, lv
import tdl_py

DETECT_MODEL = "/root/tdl_app_sdk_cv184x/configs/model_specs/scrfd_real.mud"
ATTRIBUTE_MODEL = (
    "/root/tdl_app_sdk_cv184x/configs/model_specs/"
    "face_attribute_gender_age_glass_emotion.mud")
MAX_FACES = 2


def to_screen(frame, x, y):
    # Same sensor-content letterbox mapping used by dara.nn.
    scale = min(frame.width / 1600.0, frame.height / 1200.0)
    content_w, content_h = 1600.0 * scale, 1200.0 * scale
    offset_x = (frame.width - content_w) / 2.0
    offset_y = (frame.height - content_h) / 2.0
    return (int((x - offset_x) / content_w * 640.0 + 40.0),
            int((y - offset_y) / content_h * 480.0))


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

emotion = tdl_py.FaceEmotionRecognizer(
    DETECT_MODEL, ATTRIBUTE_MODEL, threshold=0.35, max_faces=MAX_FACES)

while True:
    with camera.read() as frame:
        faces = emotion.recognize(frame)
        for index, face in enumerate(faces[:MAX_FACES]):
            x1, y1 = to_screen(frame, face.box.x1, face.box.y1)
            x2, y2 = to_screen(frame, face.box.x2, face.box.y2)
            rects[index].set_pos(max(0, x1), max(0, y1))
            rects[index].set_size(max(2, x2 - x1), max(2, y2 - y1))
            rects[index].remove_flag(lv.obj.FLAG.HIDDEN)
            age = "?" if face.age_years < 0 else str(face.age_years)
            labels[index].set_text("%s %.2f %s %s glasses:%s" % (
                face.emotion, face.emotion_score, face.gender_label, age,
                "yes" if face.has_glasses else "no"))
            labels[index].set_pos(max(0, x1), max(0, y1 - 20))
            labels[index].remove_flag(lv.obj.FLAG.HIDDEN)
        for index in range(len(faces), MAX_FACES):
            rects[index].add_flag(lv.obj.FLAG.HIDDEN)
            labels[index].add_flag(lv.obj.FLAG.HIDDEN)
    status.set_text("emotion: %d face(s)" % len(faces))
    lv.show()
