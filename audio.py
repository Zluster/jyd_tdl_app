"""dara.audio: CV184X board audio and direct-BMRT speech interfaces.

All algorithm inputs are signed 16-bit little-endian, mono, 16 kHz PCM
``bytes``. Use ``Audio.capture_pcm()`` to obtain compatible input directly
from the microphone.
"""

import tdl_py


def _require(name):
    value = getattr(tdl_py, name, None)
    if value is None:
        raise RuntimeError(
            "current tdl_py.so does not contain %s; install the matching "
            "Dara package or update tdl_py.so" % name)
    return value


Audio = _require("Audio")
SpeakerRecognizer = _require("SpeakerRecognizer")
StreamingAsr = _require("StreamingAsr")
KeywordSpotter = _require("KeywordSpotter")


__all__ = [
    "Audio",
    "SpeakerRecognizer",
    "StreamingAsr",
    "KeywordSpotter",
]
