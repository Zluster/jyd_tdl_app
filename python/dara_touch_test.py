"""Print touchscreen events exported by tdl_py.Touch."""

import sys

import tdl_py


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    timeout_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    rotation = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    touch = tdl_py.Touch(rotation=rotation)
    print("touch: Ctrl+C to stop" if count == 0 else
          "touch: waiting for %d event(s)" % count)
    received = 0
    while count == 0 or received < count:
        event = touch.read(timeout_ms)
        if event is None:
            continue
        received += 1
        print("%s x=%d y=%d pressure=%d id=%d time_us=%d" % (
            event.phase, event.x, event.y, event.pressure,
            event.tracking_id, event.timestamp_us))


if __name__ == "__main__":
    main()
