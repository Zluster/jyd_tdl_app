"""Local video playback for CV184X.

Video frames never enter Python: FFmpeg decodes H.264 to NV12 in native code,
then hardware VPSS -> VO displays them. LVGL remains available as an overlay.
"""

import tdl_py


class VideoPlayer:
    """Play one local H.264 MP4 file through the board video path."""

    def __init__(self, path=None, *, loop=False):
        self._native = tdl_py.VideoPlayer()
        if path is not None:
            self.play(path, loop=loop)

    def play(self, path, *, loop=False):
        """Start local ``path``; initial support is H.264 at <=720x480."""
        self._native.play(str(path), bool(loop))
        return self

    def pause(self):
        self._native.pause()

    def resume(self):
        self._native.resume()

    def stop(self):
        self._native.stop()

    def set_volume(self, volume_level):
        """Set CV184X speaker level, from 0 (mute) through 32 (maximum)."""
        self._native.set_volume(int(volume_level))

    def close(self):
        self._native.close()

    @property
    def state(self):
        return self._native.state

    @property
    def last_error(self):
        return self._native.last_error

    @property
    def playing(self):
        return self._native.playing

    @property
    def width(self):
        return self._native.width

    @property
    def height(self):
        return self._native.height

    @property
    def duration_ms(self):
        """Media duration in milliseconds, or 0 when the container omits it."""
        return self._native.duration_ms

    @property
    def position_ms(self):
        """Current playback position in milliseconds."""
        return self._native.position_ms

    @property
    def has_audio(self):
        """Whether the selected file has an audio stream."""
        return self._native.has_audio

    @property
    def volume(self):
        """Current CV184X speaker level, from 0 through 32."""
        return self._native.volume

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, trace):
        self.close()
