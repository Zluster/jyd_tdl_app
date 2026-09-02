"""CV184X real-time audio algorithms.

``KeywordSpotter`` accepts Chinese text at runtime. The CV184X KWS model
internally uses tone-marked pinyin pieces, which are converted here before a
keyword is registered with the native streaming recognizer.
"""

from __future__ import annotations

import configparser
import os
import tempfile
from functools import lru_cache

import tdl_audio
import tdl_py


class Audio:
    """Basic AI/AO recording and playback without loading an NPU model.

    All recording methods use 16 kHz, mono, signed 16-bit PCM by default.
    The 320-sample hardware period is the stable setting validated on CV184X.
    Methods return ``False`` or ``None`` on a hardware error; inspect
    :attr:`last_error` before retrying.
    """

    def __init__(self) -> None:
        self._native = tdl_py.Audio()

    @property
    def last_error(self) -> str:
        return self._native.last_error

    def status(self):
        """Return AI/AO runtime and stream status fields."""
        return self._native.status()

    def record_wav(self, path: str, seconds: float = 3.0,
                   sample_rate: int = 16000, channels: int = 1,
                   input_volume: int = 40, points_per_frame: int = 320,
                   timeout_ms: int = 1000) -> bool:
        """Record microphone PCM into a standard WAV file."""
        return self._native.record_wav(
            path, seconds, sample_rate, channels, input_volume,
            points_per_frame, 8, 8, timeout_ms)

    def capture_pcm(self, seconds: float = 3.0, sample_rate: int = 16000,
                    channels: int = 1, input_volume: int = 40,
                    points_per_frame: int = 320,
                    timeout_ms: int = 1000):
        """Return captured signed PCM bytes, or ``None`` on an error."""
        return self._native.capture_pcm(
            seconds, sample_rate, channels, input_volume,
            points_per_frame, 8, 8, timeout_ms)

    def play_wav(self, path: str, output_volume: int = 16,
                 timeout_ms: int = 1000) -> bool:
        """Play a PCM WAV file through the selected board audio output."""
        return self._native.play_wav(path, output_volume, timeout_ms)

    def loopback(self, seconds: float = 3.0, sample_rate: int = 16000,
                 channels: int = 1, input_volume: int = 40,
                 output_volume: int = 16, points_per_frame: int = 320,
                 timeout_ms: int = 1000) -> bool:
        """Route microphone input directly to the output for a fixed duration."""
        return self._native.loopback(
            seconds, sample_rate, channels, input_volume, output_volume,
            points_per_frame, 8, 8, timeout_ms)

    def set_input_volume(self, volume: int) -> bool:
        return self._native.set_input_volume(volume)

    def set_output_volume(self, volume: int) -> bool:
        return self._native.set_output_volume(volume)

    def input_volume(self):
        return self._native.input_volume()

    def output_volume(self):
        return self._native.output_volume()


SpeakerRecognizer = tdl_audio.SpeakerRecognizer
StreamingAsr = tdl_audio.StreamingAsr
SpeechRecognizer = StreamingAsr


def _tokens_from_model_spec(model_spec: str) -> set[str]:
    parser = configparser.ConfigParser(interpolation=None)
    with open(model_spec, encoding="utf-8") as source:
        parser.read_file(source)
    try:
        token_path = parser["extra"]["tokens"].strip()
    except KeyError as error:
        raise ValueError("KWS model descriptor has no [extra] tokens entry") from error
    if not os.path.isabs(token_path):
        token_path = os.path.normpath(os.path.join(os.path.dirname(model_spec), token_path))
    try:
        with open(token_path, encoding="utf-8") as source:
            return {line.rsplit(None, 1)[0] for line in source if line.strip()}
    except OSError as error:
        raise ValueError("cannot read KWS token file: %s" % token_path) from error


def _split_syllable(syllable: str, token_set: set[str]) -> list[str]:
    """Split one tone-marked pinyin syllable using the model's BPE pieces."""
    usable = tuple(sorted((item for item in token_set if not item.startswith("<")),
                          key=len, reverse=True))

    @lru_cache(maxsize=None)
    def split_at(offset: int):
        if offset == len(syllable):
            return ()
        for token in usable:
            if syllable.startswith(token, offset):
                rest = split_at(offset + len(token))
                if rest is not None:
                    return (token,) + rest
        return None

    parts = split_at(0)
    if not parts:
        raise ValueError("KWS model has no token sequence for pinyin syllable: %s" % syllable)
    return list(parts)


class KeywordSpotter:
    """Streaming KWS with runtime Chinese keyword registration.

    Typical use::

        kws = audio.KeywordSpotter()
        kws.load("/root/models/npu_zipformer_zh_kws.mud", beam_width=6)
        kws.register("竞业达", confidence=0.15)
        kws.register("文森特卡索", confidence=0.10)
        kws.start()

    ``register`` replaces an existing keyword with the same display name.
    Register before ``start``; modifying a registry while a capture thread is
    consuming it is deliberately rejected.
    """

    def __init__(self) -> None:
        self._native = tdl_audio.KeywordSpotter()
        self._tokens = None
        self._wrapper_error = ""
        self._model_spec = None
        self._keywords_path = None
        self._threshold = -1.0
        self._beam_width = 2
        self._registered = {}
        self._generated_keywords_path = None

    def load(self, model_spec: str, keywords_path=None,
             threshold: float = -1.0, beam_width: int = 2) -> bool:
        """Load the model; ``keywords_path`` remains for legacy applications.

        New code should omit ``keywords_path`` and call :meth:`register` with
        Chinese text instead.
        """
        try:
            self._tokens = _tokens_from_model_spec(model_spec)
        except ValueError as error:
            self._tokens = None
            self._wrapper_error = str(error)
            return False
        if self.listening:
            self._wrapper_error = "stop KWS before loading another model"
            return False
        self._clear_native()
        self._model_spec = model_spec
        self._keywords_path = keywords_path
        self._threshold = threshold
        self._beam_width = beam_width
        self._registered = {}
        self._wrapper_error = ""
        if keywords_path:
            ok = self._native.load(model_spec, keywords_path, threshold, beam_width)
            if not ok:
                self._wrapper_error = self._native.last_error
            return ok
        return True

    def register(self, text: str, confidence: float = 0.15, name=None) -> bool:
        """Register a Chinese keyword and its independent trigger threshold."""
        if self.listening:
            self._wrapper_error = "stop KWS before changing registered keywords"
            return False
        if not isinstance(text, str) or not text.strip():
            self._wrapper_error = "keyword text must be non-empty Chinese text"
            return False
        if not 0.0 <= confidence <= 1.0:
            self._wrapper_error = "keyword confidence must be in [0, 1]"
            return False
        if self._model_spec is None or self._tokens is None:
            self._wrapper_error = "load the KWS model before registering keywords"
            return False
        try:
            # The full offline pinyin dictionary is several megabytes and
            # takes about two seconds to parse on CV184X. Speaker/ASR users
            # should not pay that startup cost, so load it only for Chinese
            # KWS registration.
            try:
                from pypinyin import Style, lazy_pinyin
            except ImportError as exc:
                raise RuntimeError(
                    "Chinese keyword registration requires the Alpine "
                    "package py3-pypinyin") from exc
            syllables = lazy_pinyin(text.strip(), style=Style.TONE,
                                    errors=lambda value: [value])
            if any(any("\u4e00" <= char <= "\u9fff" for char in part)
                   for part in syllables):
                raise ValueError("keyword contains a character without pinyin")
            tokens = [piece for syllable in syllables
                      for piece in _split_syllable(syllable, self._tokens)]
        except (RuntimeError, TypeError, ValueError) as error:
            self._wrapper_error = str(error)
            return False
        display_name = name or text.strip()
        if not isinstance(display_name, str) or not display_name.strip():
            self._wrapper_error = "keyword name must be non-empty text"
            return False
        if "\n" in display_name or "\r" in display_name:
            self._wrapper_error = "keyword name must not contain line breaks"
            return False
        self._clear_native()
        self._registered[display_name.strip()] = (tokens, confidence)
        self._wrapper_error = ""
        return True

    def unregister(self, name: str) -> bool:
        if self.listening:
            self._wrapper_error = "stop KWS before changing registered keywords"
            return False
        if name not in self._registered:
            self._wrapper_error = "keyword is not registered: %s" % name
            return False
        self._clear_native()
        del self._registered[name]
        self._wrapper_error = ""
        return True

    def registered_keywords(self):
        if self._keywords_path:
            return [item["name"] for item in self._native.scores()]
        return list(self._registered)

    def accept(self, pcm):
        return self._native.accept(pcm)

    def finish(self):
        return self._native.finish()

    def start(self, input_volume: int = 40, points_per_frame: int = 320,
              timeout_ms: int = 1000) -> bool:
        if not self._keywords_path and not self._registered:
            self._wrapper_error = "register at least one keyword before start"
            return False
        if not self._native.initialized:
            if not self._load_registered_keywords():
                return False
        ok = self._native.start(input_volume, points_per_frame, timeout_ms)
        if ok:
            self._wrapper_error = ""
        return ok

    def read(self):
        return self._native.read()

    def stop(self) -> bool:
        return self._native.stop()

    def scores(self):
        return self._native.scores()

    def reset(self) -> None:
        self._native.reset()

    @property
    def initialized(self) -> bool:
        return self._model_spec is not None

    @property
    def listening(self) -> bool:
        return self._native.listening

    @property
    def last_error(self) -> str:
        return self._wrapper_error or self._native.last_error

    def _clear_native(self) -> None:
        if self._native.listening:
            self._native.stop()
        self._native = tdl_audio.KeywordSpotter()
        if self._generated_keywords_path:
            try:
                os.unlink(self._generated_keywords_path)
            except OSError:
                pass
        self._generated_keywords_path = None

    def _load_registered_keywords(self) -> bool:
        if self._model_spec is None:
            self._wrapper_error = "load the KWS model before start"
            return False
        try:
            with tempfile.NamedTemporaryFile(
                    mode="w", encoding="utf-8", prefix="dara-kws-",
                    suffix=".txt", dir="/tmp", delete=False) as source:
                for name, (tokens, confidence) in self._registered.items():
                    source.write("%s #%g @%s\n" %
                                 (" ".join(tokens), confidence, name))
                self._generated_keywords_path = source.name
            ok = self._native.load(self._model_spec,
                                   self._generated_keywords_path,
                                   self._threshold, self._beam_width)
        except OSError as error:
            self._wrapper_error = "cannot create KWS keyword file: %s" % error
            return False
        if not ok:
            self._wrapper_error = self._native.last_error
            return False
        try:
            os.unlink(self._generated_keywords_path)
        except OSError:
            pass
        self._generated_keywords_path = None
        self._wrapper_error = ""
        return True


__all__ = [
    "Audio",
    "SpeakerRecognizer",
    "StreamingAsr",
    "SpeechRecognizer",
    "KeywordSpotter",
]
