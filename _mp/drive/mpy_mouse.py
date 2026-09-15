# LVGL indev driver for evdev mouse device
# (for the unix micropython port)
path = __file__[:__file__.rfind('/')+1]
import ustruct
import select
import sys
import lvgl as lv

# struct input_event 尺寸随用户态位数变化：64 位为 24 字节（QQHHi），
# 32 位为 16 字节（IIHHi）。读长度不是事件尺寸整数倍时内核返回 EINVAL。
if sys.maxsize > 2**32:
    EV_FMT, EV_SIZE = 'QQHHi', 24
else:
    EV_FMT, EV_SIZE = 'IIHHi', 16

#import lv_pm
#pm = lv_pm.pm()
# LVGL pointer driver for the physical touchscreen and the remote uinput
# touchscreen.  The physical GT911 currently uses the historical reversed
# coordinate convention; browser/uinput events are already LCD coordinates.
class mouse_indev:
    def __init__(self, scr=None, devices=None):
        if devices is None:
            devices = (
                ("/dev/input/event0", True, "screen"),
                ("/dev/input/event1", False, "remote"),
            )
        self.device_specs = devices
        self.poll = select.poll()
        self.sources = []
        self._scan_ticks = 0
        self._open_configured_sources()
        if not self.sources:
            raise OSError("no touchscreen input device is available")
        self.scr = scr if scr else lv.screen_active()
        self.hor_res = self.scr.get_width()
        self.ver_res = self.scr.get_height()

        # Register LVGL indev driver
        self.indev = lv.indev_create()
        self.indev.set_type(lv.INDEV_TYPE.POINTER)
        self.indev.set_read_cb(self.mouse_read)

        self.timer = self.indev.get_read_timer()
        self.timer.set_period(16)

        self.owner = None       # source that owns the current one-finger touch
        self.last_source = self.sources[0]

    def _open_configured_sources(self):
        for path, invert, name in self.device_specs:
            already_open = False
            for source in self.sources:
                if source["path"] == path:
                    already_open = True
                    break
            if already_open:
                continue
            try:
                evdev = open(path, 'rb')
            except OSError:
                # event1 is created by the WebSocket/uinput service.  The
                # normal touchscreen remains usable before that service runs.
                continue
            source = {
                "file": evdev,
                "fd": evdev.fileno(),
                "path": path,
                "invert": invert,
                "name": name,
                "x": 50,
                "y": 50,
            }
            self.poll.register(source["fd"])
            self.sources.append(source)

    def _press(self, source):
        # LVGL pointer indev is single-touch.  The first source to press owns
        # the gesture until it releases; the other source cannot release it.
        if self.owner is None or self.owner is source:
            self.owner = source
            self.last_source = source

    def _release(self, source):
        if self.owner is source:
            self.last_source = source
            self.owner = None

    def _read_event(self, source):
        time_sec, time_usec, event_type, code, value = ustruct.unpack(
            EV_FMT, source["file"].read(EV_SIZE))
        if event_type == 0x03:
            # event0 normally uses ABS_MT_POSITION_X/Y (53/54).  Virtual
            # uinput producers commonly use either those codes or ABS_X/Y
            # (0/1), so accept both forms.
            if code == 53 or code == 0:
                source["x"] = value
                if self.owner is None or self.owner is source:
                    self.last_source = source
            elif code == 54 or code == 1:
                source["y"] = value
                if self.owner is None or self.owner is source:
                    self.last_source = source
            elif code == 57:  # ABS_MT_TRACKING_ID: -1 is contact release.
                if value >= 0:
                    self._press(source)
                else:
                    self._release(source)
        elif event_type == 0x01 and (code == 330 or code == 272):
            # BTN_TOUCH is normal for a touchscreen; BTN_LEFT makes a
            # mouse-style uinput producer usable for temporary diagnostics.
            if value:
                self._press(source)
            else:
                self._release(source)

    def _drain_source(self, source):
        self._read_event(source)
        # Drain only the same fd.  Checking the global poll set before every
        # read avoids blocking on a source while another source is ready.
        while True:
            ready = False
            for fd, flags in self.poll.poll(0):
                if fd == source["fd"] and flags & select.POLLIN:
                    ready = True
                    break
            if not ready:
                break
            self._read_event(source)

    def mouse_read(self, indev, data) -> int:
        # The WebSocket service can create its uinput device after launcher
        # starts.  Retry unopened configured paths about once per second.
        self._scan_ticks += 1
        if self._scan_ticks >= 64:
            self._scan_ticks = 0
            self._open_configured_sources()
        # Read every currently-ready input source.  Each source has a separate
        # file descriptor, so real and remote touches do not consume each
        # other's evdev stream.
        events = self.poll.poll(0)
        if not events:
            return 0
        for fd, flags in events:
            if not flags & select.POLLIN:
                continue
            for source in self.sources:
                if source["fd"] == fd:
                    self._drain_source(source)
                    break

        source = self.owner if self.owner is not None else self.last_source
        x = source["x"]
        y = source["y"]
        if source["invert"]:
            x = self.hor_res - 1 - x
            y = self.ver_res - 1 - y
        data.point.x = max(0, min(self.hor_res - 1, x))
        data.point.y = max(0, min(self.ver_res - 1, y))
        data.state = (
            lv.INDEV_STATE.PRESSED
            if self.owner is not None
            else lv.INDEV_STATE.RELEASED
        )

        return 0

    def delete(self):
        for source in self.sources:
            source["file"].close()
        self.indev.enable(False)
