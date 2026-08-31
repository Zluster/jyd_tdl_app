"""Record and play a 16 kHz mono PCM WAV file through tdl_py.Audio."""

import sys
import tdl_py

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
WAV_PATH = "/tmp/tdl_audio_test.wav"

audio = tdl_py.Audio()
print("status:", audio.status())
if not audio.record_wav(WAV_PATH, SECONDS):
    raise RuntimeError("record failed: " + audio.last_error)
print("recorded:", WAV_PATH)
if not audio.play_wav(WAV_PATH):
    raise RuntimeError("playback failed: " + audio.last_error)
print("playback complete")
