"""Direct BMRT KWS test: capture chunks and print all keyword scores.

Keyword registry format and thresholds are in configs/kws_keywords.default.txt.
No Sherpa or ONNX Runtime is used.
"""

import sys
import tdl_py

ROOT = "/root/tdl_app_sdk_cv184x"
MODEL = ROOT + "/configs/model_specs/npu_zipformer_zh_kws.mud"
KEYWORDS = ROOT + "/configs/kws_keywords.default.txt"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
CHUNK_SECONDS = 0.8

audio = tdl_py.Audio()
kws = tdl_py.KeywordSpotter()
if not kws.load(MODEL, KEYWORDS, beam_width=6):
    raise RuntimeError("KWS model load failed: " + kws.last_error)

print("KWS listening; Ctrl+C to finish")
try:
    count = max(1, int(SECONDS / CHUNK_SECONDS))
    for _ in range(count):
        pcm = audio.capture_pcm(CHUNK_SECONDS)
        if pcm is None:
            raise RuntimeError("capture failed: " + audio.last_error)
        hits = kws.accept(pcm)
        if hits is None:
            raise RuntimeError("KWS failed: " + kws.last_error)
        for hit in hits:
            print("trigger:", hit)
        print("scores:", kws.scores())
except KeyboardInterrupt:
    pass
hits = kws.finish()
if hits is None:
    raise RuntimeError("KWS finish failed: " + kws.last_error)
for hit in hits:
    print("trigger:", hit)
