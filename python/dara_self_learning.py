"""DARA standalone online self-learning classification demo.

Tap a class button while its target is centered in the camera. The app collects
multiple live frames for that class, then saves the feature bank. Tapping the
same class again appends more samples and improves its prototype.
"""

import os

from dara import camera, lv, nn

# MobileNetV2 0.50 is a compact generic embedding model for CV184X. Do not use
# feature_cviface here: it is trained for face identity rather than objects.
MODEL = "/root/tdl_app_sdk_cv184x/configs/model_specs/feature_mobilenetv2_050_embedding_160.mud"
# Feature banks are model-specific. This model produces 1280-D embeddings.
BANK = "/root/tdl_self_learning_mobilenetv2_050.bank"
TOP_K = 3
MATCH_THRESHOLD = 0.78
CLASS_LABELS = ("class_a", "class_b", "class_c")
SAMPLES_PER_CAPTURE = 12
CAPTURE_FRAME_INTERVAL = 3


lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
scr = lv.screen_active()

status = lv.label(scr)
status.set_style_text_color(lv.color_white(), 0)
status.set_pos(8, 8)

labels = []
for index in range(TOP_K):
    label = lv.label(scr)
    label.set_style_text_color(lv.color_white(), 0)
    label.set_pos(16, 48 + index * 28)
    label.add_flag(lv.obj.FLAG.HIDDEN)
    labels.append(label)

classifier = nn.load(MODEL, top_k=TOP_K)
if os.path.exists(BANK):
    classifier.load_bank(BANK)

sample_label = ""
samples_remaining = 0
capture_tick = 0
message = "tap a class to collect %d samples" % SAMPLES_PER_CAPTURE


def request_capture(label):
    def callback():
        global sample_label, samples_remaining, capture_tick, message
        sample_label = label
        samples_remaining = SAMPLES_PER_CAPTURE
        capture_tick = 0
        message = "%s: hold target still" % label
    return callback


def request_cancel():
    global sample_label, samples_remaining, message
    sample_label = ""
    samples_remaining = 0
    message = "capture cancelled"


def request_clear():
    global sample_label, samples_remaining, message
    sample_label = "__clear__"
    samples_remaining = 0
    message = "clearing samples..."


def add_button(text, x, callback, color):
    button = lv.button(scr)
    button.set_size(104, 42)
    button.set_pos(x, 426)
    button.set_style_bg_color(lv.palette_main(color), 0)
    button.set_style_bg_opa(lv.OPA.COVER, 0)
    label = lv.label(button)
    label.set_text(text)
    label.center()
    lv.bind(button, lv.EVENT.CLICKED, callback)


add_button("class A", 8, request_capture(CLASS_LABELS[0]), lv.PALETTE.BLUE)
add_button("class B", 116, request_capture(CLASS_LABELS[1]), lv.PALETTE.GREEN)
add_button("class C", 224, request_capture(CLASS_LABELS[2]), lv.PALETTE.ORANGE)
add_button("cancel", 504, request_cancel, lv.PALETTE.GREY)
add_button("clear all", 612, request_clear, lv.PALETTE.RED)

while True:
    if sample_label == "__clear__":
        classifier.clear()
        if os.path.exists(BANK):
            os.remove(BANK)
        sample_label = ""
        message = "samples cleared"

    result = None
    with camera.read() as frame:
        if samples_remaining:
            capture_tick += 1
            if capture_tick % CAPTURE_FRAME_INTERVAL == 0:
                try:
                    classifier.add_frame(sample_label, frame)
                except Exception as error:
                    message = "sample failed: %s" % error
                    sample_label = ""
                    samples_remaining = 0
                else:
                    samples_remaining -= 1
                    if samples_remaining:
                        message = "%s: %d samples left" % (
                            sample_label, samples_remaining)
                    else:
                        try:
                            classifier.save_bank(BANK)
                        except RuntimeError as error:
                            message = "save failed: %s" % error
                        else:
                            message = "%s saved (%d total)" % (
                                sample_label, classifier.sample_count)
                        sample_label = ""
        elif classifier.sample_count:
            result = classifier.run(frame)

    if result is not None:
        best = result.classes[0] if result.classes else None
        if best is None or best.score < MATCH_THRESHOLD:
            labels[0].set_text("unknown  %.3f" % (best.score if best else 0.0))
            labels[0].remove_flag(lv.obj.FLAG.HIDDEN)
            for index in range(1, TOP_K):
                labels[index].add_flag(lv.obj.FLAG.HIDDEN)
        else:
            for index, item in enumerate(result.classes[:TOP_K]):
                labels[index].set_text("%d. %s  %.3f" % (
                    index + 1, item.label, item.score))
                labels[index].remove_flag(lv.obj.FLAG.HIDDEN)
            for index in range(len(result.classes), TOP_K):
                labels[index].add_flag(lv.obj.FLAG.HIDDEN)
    else:
        for label in labels:
            label.add_flag(lv.obj.FLAG.HIDDEN)

    status.set_text("%s  classes=%d samples=%d" % (
        message, classifier.class_count, classifier.sample_count))
    lv.show()
