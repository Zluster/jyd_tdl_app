"""Enroll or recognize a speaker from one short microphone capture.

Usage:
  python3 dara_speaker_recognition_test.py enroll alice
  python3 dara_speaker_recognition_test.py recognize
"""

import os
import sys
import tdl_py

ROOT = "/root/tdl_app_sdk_cv184x"
MODEL = ROOT + "/configs/model_specs/speaker_campplus_sv.mud"
DATABASE = "/tmp/tdl_speakers.db"
SECONDS = 4.0

mode = sys.argv[1] if len(sys.argv) > 1 else "recognize"
label = sys.argv[2] if len(sys.argv) > 2 else "speaker"

audio = tdl_py.Audio()
pcm = audio.capture_pcm(SECONDS)
if pcm is None:
    raise RuntimeError("capture failed: " + audio.last_error)

recognizer = tdl_py.SpeakerRecognizer()
if not recognizer.load(MODEL):
    raise RuntimeError("model load failed: " + recognizer.last_error)
if os.path.exists(DATABASE) and not recognizer.load_database(DATABASE):
    raise RuntimeError("database load failed: " + recognizer.last_error)

if mode == "enroll":
    if not recognizer.enroll(label, pcm):
        raise RuntimeError("enroll failed: " + recognizer.last_error)
    if not recognizer.save_database(DATABASE):
        raise RuntimeError("database save failed: " + recognizer.last_error)
    print("enrolled", label, "labels=", recognizer.labels())
elif mode == "recognize":
    result = recognizer.recognize(pcm, 0.60)
    if result is None:
        raise RuntimeError("recognize failed: " + recognizer.last_error)
    print(result)
else:
    raise ValueError("mode must be enroll or recognize")
