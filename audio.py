"""CV184X real-time audio algorithms and playback.

``KeywordSpotter`` accepts Chinese text at runtime. The CV184X KWS model
internally uses tone-marked pinyin pieces, which are converted here before a
keyword is registered with the native streaming recognizer.

``WavPlayer`` plays one PCM WAV file at a time with pause/resume/stop and
progress, on top of the low-level ``AudioOutputStream`` (one AO channel per
object).  ``Audio.play_wav`` remains the simple blocking one-shot playback.
"""

from __future__ import annotations

import configparser
import os
import tempfile
import threading
import time
import wave
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


#: Low-level AO output: one channel per object, interleaved PCM in, with
#: pause/resume/buffer_state.  See WavPlayer for the file-level API.
AudioOutputStream = tdl_py.AudioOutputStream


class WavPlayer:
    """Play one uncompressed PCM WAV file with pause/resume/stop and progress.

    The file is parsed with the standard ``wave`` module and streamed by a
    background thread into an :class:`AudioOutputStream`; the AO channel is
    opened per track with the file's own sample rate / channels / bit depth.
    The driver only accepts blocks of exactly its period, and it rounds the
    requested ``points_per_frame`` to whole milliseconds (320 samples are
    20 ms at 16 kHz but 288 at 48 kHz), so by default the block is chosen as
    20 ms of the track's own rate and the size the driver actually settled on
    (``stream.frame_samples``) is what gets written.

    The library's own queue is 64 periods (about 1.3 s) deep and cannot be
    dropped safely: ``CVI_AO_ClearChnBuf`` leaves a mark that makes the
    following close wait about 10 s (vendor bug, see AudioOutputStream).  So
    the worker keeps at most ``_QUEUE_PERIODS`` blocks queued and a stop just
    lets the driver drain them: stopping or switching tracks -- including on
    application exit -- goes quiet within roughly a tenth of a second.

        player = audio.WavPlayer(volume=16)
        player.play("/root/jyd_data/music/song.wav")   # False + last_error on failure
        player.pause(); player.resume()
        player.seek(90_000)    # jump to 1:30, keeps playing / stays paused
        st = player.status()   # state/path/elapsed_ms/total_ms/volume/error
        player.stop()          # also releases the AO channel

    ``state`` is one of ``idle`` (nothing loaded / stopped), ``playing``,
    ``paused``, ``finished`` (track played to the end) or ``error``.  The
    playlist logic (next track, repeat) belongs to the caller.

    Seeking cannot reuse the open channel: the ~120 ms already queued would
    play on and the driver's clear is off limits, so ``seek()`` halts the
    stream (the queue drains in a few tens of ms) and opens a fresh channel
    that continues from the target frame via ``wave.setpos``.  While paused
    only the position moves and the channel is reopened by ``resume()``.
    """

    IDLE, PLAYING, PAUSED, FINISHED, ERROR = "idle", "playing", "paused", "finished", "error"

    #: consecutive driver write failures tolerated before giving up (each one
    #: waited timeout_ms already); a full queue normally frees a slot within
    #: one frame period, so this only triggers on a real AO fault
    _MAX_WRITE_RETRIES = 25
    #: how long to wait for the driver to play out the tail after the last write
    _DRAIN_TIMEOUT_S = 2.0
    #: default block length; a whole number of ms so the driver keeps it as is
    _BLOCK_MS = 20
    #: most blocks kept queued in the library (about 120 ms at 20 ms blocks):
    #: enough headroom against scheduling hiccups, short enough that close()
    #: drains it in a few tens of ms
    _QUEUE_PERIODS = 6
    #: periods the hardware FIFO holds beyond the library queue (the dual-OS
    #: driver clamps its period count to 3); waited out at a natural end
    _TAIL_PERIODS = 3

    def __init__(self, volume: int = 16, points_per_frame=None,
                 frame_count: int = 8, timeout_ms: int = 100,
                 ao_device: int = 0, ao_channel: int = 0, ao_card_id: int = -1):
        """``points_per_frame`` None (default) means 20 ms of each track's own
        sample rate; an explicit value is passed to the driver as is."""
        self._volume = int(volume)
        self._points = None if points_per_frame is None else int(points_per_frame)
        self._frame_count = int(frame_count)
        self._timeout_ms = int(timeout_ms)
        self._ao = (int(ao_device), int(ao_channel), int(ao_card_id))
        self._cond = threading.Condition()   # guards every field below
        self._state = self.IDLE
        self._path = None
        self._error = ""
        self._stream = None
        self._thread = None
        self._stop_requested = False
        self._paused = False
        self._rate = 0
        self._format = None          # (rate, channels, sample_width) of the current track
        self._nframes = 0            # sample frames in the current track
        self._total_ms = 0
        self._frame_samples = 0      # samples per channel in one driver block (current track)
        self._sample_bytes = 0       # bytes per sample frame (channels x sample width)
        self._frames_written = 0     # track position (sample frames) handed to the driver

    # ---- control ----

    def play(self, path: str) -> bool:
        """Stop whatever is playing and start ``path`` from the beginning."""
        self.stop()
        try:
            rate, channels, sample_width, nframes = self._probe(path)
        except (OSError, wave.Error, ValueError) as error:
            self._set_error("%s: %s" % (path, error))
            return False
        with self._cond:
            self._path = path
            self._format = (rate, channels, sample_width)
            self._rate = rate
            self._nframes = nframes
            self._total_ms = nframes * 1000 // rate
            self._sample_bytes = channels * sample_width
        return self._start(path, 0)

    def seek(self, elapsed_ms) -> bool:
        """Jump to ``elapsed_ms`` (clamped to the track).  Playing: continues
        from there on a fresh channel; paused: stays paused and resume() picks
        up from there.  False when nothing is playing or paused."""
        with self._cond:
            state, path, rate, nframes = self._state, self._path, self._rate, self._nframes
        if state not in (self.PLAYING, self.PAUSED) or path is None or not rate:
            return False
        frame = max(0, min(nframes, int(elapsed_ms) * rate // 1000))
        self._halt_stream()
        if state == self.PLAYING:
            return self._start(path, frame)
        with self._cond:
            self._frames_written = frame     # what status() shows; resume() starts here
            self._state = self.PAUSED
        return True

    def pause(self) -> bool:
        with self._cond:
            if self._state != self.PLAYING:
                return False
            self._paused = True          # worker stops feeding after its current write
            self._state = self.PAUSED
            stream = self._stream
        return self._report(stream, stream.pause())

    def resume(self) -> bool:
        with self._cond:
            if self._state != self.PAUSED:
                return False
            stream, path, frame = self._stream, self._path, self._frames_written
        if stream is None:               # paused and then seeked: no channel open
            return self._start(path, frame)
        ok = stream.resume()             # let the driver run again first...
        with self._cond:
            self._paused = False         # ...then wake the worker
            self._state = self.PLAYING
            self._cond.notify_all()
        return self._report(stream, ok)

    def stop(self) -> bool:
        """Stop feeding, let the short queue drain and release the AO channel
        (safe to call anytime; returns within about a tenth of a second)."""
        self._halt_stream()
        with self._cond:
            if self._state in (self.PLAYING, self.PAUSED):
                self._state = self.IDLE
        return True

    close = stop

    def _start(self, path, start_frame) -> bool:
        """Open a channel for the probed track and feed it from ``start_frame``
        (a previous stream, if any, has been halted).  Failure -> ERROR state."""
        rate, channels, sample_width = self._format
        points = self._points or max(1, rate * self._BLOCK_MS // 1000)
        stream = AudioOutputStream()
        if not stream.open(rate, channels, sample_width * 8, self._volume,
                           points, self._frame_count, self._timeout_ms,
                           *self._ao):
            self._set_error(stream.last_error)
            return False
        frame_samples = stream.frame_samples     # what the driver really accepts per write
        with self._cond:
            self._stream = stream
            self._error = ""
            self._frame_samples = frame_samples
            self._frames_written = start_frame
            self._stop_requested = False
            self._paused = False
            self._state = self.PLAYING
            self._thread = threading.Thread(target=self._pump,
                                            args=(path, stream, frame_samples, start_frame),
                                            name="dara-wav-player", daemon=True)
            self._thread.start()
        return True

    def _halt_stream(self):
        """Stop the worker and release the channel; the state field is left
        to the caller.  close() lets the driver drain the short queue, so
        this returns within about a tenth of a second."""
        with self._cond:
            self._stop_requested = True
            self._paused = False
            self._cond.notify_all()
            thread, stream = self._thread, self._stream
            self._thread = None
            self._stream = None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=2.0)
            if thread.is_alive():
                print("dara.audio.WavPlayer: playback worker did not stop")
        if stream is not None:
            # a paused channel never drains, so it has to run again to close
            # quickly; mute first so the ~120 ms still queued is not heard
            if stream.paused:
                stream.set_volume(0)
                stream.resume()
            stream.close()

    def set_volume(self, volume: int) -> bool:
        """Change the output volume; applies immediately if a track is open."""
        with self._cond:
            self._volume = int(volume)
            stream = self._stream
        if stream is None:
            return True
        return self._report(stream, stream.set_volume(self._volume))

    # ---- state ----

    def status(self) -> dict:
        """Snapshot for the UI.  elapsed_ms counts samples handed to the driver
        minus what the driver still holds, so it stops moving while paused."""
        with self._cond:
            state, path, error = self._state, self._path, self._error
            stream, rate, sample_bytes = self._stream, self._rate, self._sample_bytes
            frames, total_ms, volume = self._frames_written, self._total_ms, self._volume
        elapsed_ms = 0
        if rate and frames:
            pending = 0
            if stream is not None and state in (self.PLAYING, self.PAUSED):
                buf = stream.buffer_state()          # busy is in bytes
                if buf is not None and sample_bytes:
                    pending = buf["busy"] // sample_bytes
            elapsed_ms = max(0, (frames - pending) * 1000 // rate)
            if state == self.FINISHED or elapsed_ms > total_ms:
                elapsed_ms = total_ms
        return {"state": state, "path": path, "elapsed_ms": elapsed_ms,
                "total_ms": total_ms, "volume": volume, "error": error}

    @property
    def state(self) -> str:
        with self._cond:
            return self._state

    @property
    def last_error(self) -> str:
        with self._cond:
            return self._error

    # ---- internals ----

    @staticmethod
    def _probe(path):
        """Validate the file and return (rate, channels, sample_width, nframes)."""
        with wave.open(path, "rb") as source:
            if source.getcomptype() != "NONE":
                raise ValueError("only uncompressed PCM WAV is supported")
            rate = source.getframerate()
            channels = source.getnchannels()
            width = source.getsampwidth()
            nframes = source.getnframes()
        if channels not in (1, 2) or width not in (1, 2, 3, 4) or rate <= 0:
            raise ValueError("unsupported WAV layout: %d Hz, %d ch, %d bytes/sample"
                             % (rate, channels, width))
        if nframes <= 0:
            raise ValueError("WAV has no audio data")
        return rate, channels, width, nframes

    def _keep_going(self):
        """Block while paused; False once stop() was requested."""
        with self._cond:
            while self._paused and not self._stop_requested:
                self._cond.wait()
            return not self._stop_requested

    def _pump(self, path, stream, frame_samples, start_frame):
        """Worker: feed blocks of exactly one driver period (frame_samples per
        channel) from ``start_frame`` on, never letting more than
        _QUEUE_PERIODS of them pile up in the library, honouring pause/stop,
        then wait for the driver to play out the tail before reporting
        finished.  On a stop request it returns at once and leaves close to
        the caller of _halt_stream()."""
        try:
            with wave.open(path, "rb") as source:
                rate = source.getframerate()
                frame_bytes = frame_samples * source.getnchannels() * source.getsampwidth()
                queue_bytes = self._QUEUE_PERIODS * frame_bytes
                half_period_s = frame_samples / (2.0 * rate)
                source.setpos(min(start_frame, source.getnframes()))
                while self._keep_going():
                    data = source.readframes(frame_samples)
                    if not data:
                        break
                    if len(data) < frame_bytes:          # last block: zero-pad, as playWav does
                        data += b"\0" * (frame_bytes - len(data))
                    # pace: wait until this block fits under _QUEUE_PERIODS queued
                    while True:
                        buf = stream.buffer_state()
                        if buf is None or buf["busy"] + frame_bytes <= queue_bytes:
                            break
                        time.sleep(half_period_s)
                        if not self._keep_going():
                            return
                    retries = 0
                    while not stream.write(data):
                        # a full driver queue frees a slot within one frame period;
                        # anything longer than _MAX_WRITE_RETRIES writes is a fault
                        retries += 1
                        if retries > self._MAX_WRITE_RETRIES:
                            raise RuntimeError(stream.last_error or "audio output write failed")
                        time.sleep(0.02)
                        if not self._keep_going():
                            return
                    with self._cond:
                        self._frames_written += frame_samples
                else:
                    return                               # stopped while paused
            deadline = time.monotonic() + self._DRAIN_TIMEOUT_S
            while time.monotonic() < deadline:
                if not self._keep_going():
                    return
                buf = stream.buffer_state()
                if buf is None or buf["busy"] == 0:
                    break
                time.sleep(0.02)
            # the library queue is empty; give the hardware FIFO time to play
            # its last periods before the channel is torn down
            time.sleep(self._TAIL_PERIODS * frame_samples / float(rate))
            if not self._keep_going():
                return
            with self._cond:
                if self._stop_requested:
                    return
                self._state = self.FINISHED
                self._stream = None
            stream.close()                # natural end: release the AO channel
        except Exception as error:        # file/driver fault: report, keep the object usable
            with self._cond:
                if self._stop_requested:
                    return
                self._state = self.ERROR
                self._error = str(error)
                self._stream = None
            stream.close()

    def _set_error(self, message):
        with self._cond:
            self._state = self.ERROR
            self._error = message
            self._path = None
            self._stream = None
            self._thread = None

    def _report(self, stream, ok):
        if not ok:
            with self._cond:
                self._error = stream.last_error
        return ok


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
            if self._keywords_path:
                ok = self._native.load(self._model_spec, self._keywords_path,
                                       self._threshold, self._beam_width)
            elif not self._load_registered_keywords():
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
    "AudioOutputStream",
    "WavPlayer",
    "SpeakerRecognizer",
    "StreamingAsr",
    "SpeechRecognizer",
    "KeywordSpotter",
]
