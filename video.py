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

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, trace):
        self.close()
