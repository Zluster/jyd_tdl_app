"""Capture microphone PCM in short chunks and decode direct BMRT ASR.

Press Ctrl+C to finish early. The recognizer has no Sherpa or ONNX dependency.
"""

import sys
import tdl_py

ROOT = "/root/tdl_app_sdk_cv184x"
MODEL = ROOT + "/configs/model_specs/npu_zipformer_zh_14m_asr.mud"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
CHUNK_SECONDS = 0.8

audio = tdl_py.Audio()
asr = tdl_py.StreamingAsr()
if not asr.load(MODEL):
    raise RuntimeError("ASR model load failed: " + asr.last_error)

print("ASR listening; Ctrl+C to finish")
try:
    count = max(1, int(SECONDS / CHUNK_SECONDS))
    for _ in range(count):
        pcm = audio.capture_pcm(CHUNK_SECONDS)
        if pcm is None:
            raise RuntimeError("capture failed: " + audio.last_error)
        text = asr.accept(pcm)
        if text is None:
            raise RuntimeError("ASR failed: " + asr.last_error)
        if text:
            print("partial:", text)
except KeyboardInterrupt:
    pass
final = asr.finish()
if final is None:
    raise RuntimeError("ASR finish failed: " + asr.last_error)
print("final:", asr.text)
