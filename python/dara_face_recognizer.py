"""DARA standalone face enrollment and recognition demo.

Set ENROLL_NAME, then tap the on-screen enroll button to register the largest
visible face. The face bank is saved to FACE_BANK.
"""

import os

from dara import camera, lv, nn

MODEL = "face_recognizer.mud"
FACE_BANK = "/root/faces.bin"
ENROLL_NAME = "alice"  # Change this name before tapping the enroll button.
MAX_FACES = 3


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.align(lv.ALIGN.TOP_MID, 0, 8)

rects, texts = [], []
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

    text = lv.label(scr)
    text.set_style_text_color(lv.color_white(), 0)
    text.add_flag(lv.obj.FLAG.HIDDEN)
    texts.append(text)

recognizer = nn.load(MODEL, threshold=0.35, match_threshold=0.60,
                     max_faces=MAX_FACES)
if os.path.exists(FACE_BANK):
    recognizer.load_faces(FACE_BANK)

status.set_text("face: %d enrolled" % len(recognizer.names()))

enroll_requested = False
clear_requested = False
enroll_message = ""


def request_enroll():
    global enroll_requested, enroll_message
    enroll_requested = True
    enroll_message = "enroll: look at camera"


def request_clear():
    global clear_requested, enroll_requested, enroll_message
    clear_requested = True
    enroll_requested = False
    enroll_message = "clearing faces..."


clear_button = lv.button(scr)
clear_button.set_size(96, 44)
clear_button.align(lv.ALIGN.TOP_RIGHT, -126, 8)
clear_button.set_style_bg_color(lv.palette_main(lv.PALETTE.RED), 0)
clear_button.set_style_bg_opa(lv.OPA.COVER, 0)
clear_label = lv.label(clear_button)
clear_label.set_text("clear")
clear_label.center()
lv.bind(clear_button, lv.EVENT.CLICKED, request_clear)


enroll_button = lv.button(scr)
enroll_button.set_size(108, 44)
enroll_button.align(lv.ALIGN.TOP_RIGHT, -10, 8)
enroll_button.set_style_bg_color(lv.palette_main(lv.PALETTE.GREEN), 0)
enroll_button.set_style_bg_opa(lv.OPA.COVER, 0)
enroll_label = lv.label(enroll_button)
enroll_label.set_text("enroll")
enroll_label.center()
lv.bind(enroll_button, lv.EVENT.CLICKED, request_enroll)

while True:
    if clear_requested:
        recognizer.clear()
        if os.path.exists(FACE_BANK):
            os.remove(FACE_BANK)
        clear_requested = False
        enroll_message = "faces cleared"

    with camera.read() as frame:
        if enroll_requested:
            try:
                recognizer.enroll(frame, ENROLL_NAME)
                recognizer.save_faces(FACE_BANK)
            except RuntimeError:
                # No usable face in this frame. Keep waiting for the next one.
                enroll_message = "enroll: waiting for face"
            else:
                enroll_requested = False
                enroll_message = "enrolled: %s" % ENROLL_NAME
        faces = recognizer.run(frame).faces

    visible = min(MAX_FACES, len(faces))
    for index, face in enumerate(faces[:MAX_FACES]):
        x1, y1 = face.box.x1, face.box.y1
        x2, y2 = face.box.x2, face.box.y2
        x1, x2 = sorted((x1, x2))
        y1, y2 = sorted((y1, y2))
        rects[index].set_pos(x1, y1)
        rects[index].set_size(max(2, x2 - x1), max(2, y2 - y1))
        rects[index].remove_flag(lv.obj.FLAG.HIDDEN)
        texts[index].set_text("%s %.2f" % (face.name, face.score))
        texts[index].set_pos(x1, max(0, y1 - 20))
        texts[index].remove_flag(lv.obj.FLAG.HIDDEN)

    for index in range(visible, MAX_FACES):
        rects[index].add_flag(lv.obj.FLAG.HIDDEN)
        texts[index].add_flag(lv.obj.FLAG.HIDDEN)

    if enroll_requested or enroll_message:
        status.set_text(enroll_message)
    elif faces:
        status.set_text("face: %s" % faces[0].name)
    else:
        status.set_text("face: looking...")
    lv.show()
