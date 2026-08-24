#!/usr/bin/env python3
"""Bounded online hand-gesture demo for the CV184X Python API."""

import argparse
import tdl_py


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--detector-model", required=True)
    p.add_argument("--keypoint-model", required=True)
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--threshold", type=float, default=0.35)
    p.add_argument("--max-hands", type=int, default=2)
    args = p.parse_args()

    recognizer = tdl_py.HandGestureRecognizer(
        args.detector_model, args.keypoint_model,
        hand_threshold=args.threshold, max_hands=args.max_hands,
    )
    # The hand models are trained for the 640x640 AI channel, matching the
    # C++ tdl_camera_hand_gesture_test path (grp0/ch1).
    camera = tdl_py.VpssCamera.ai(timeout_ms=1000)
    try:
        for index in range(max(1, args.frames)):
            with camera.read() as frame:
                hands = recognizer.recognize(frame)
            for hand in hands:
                b = hand.box
                print("frame=%d gesture=%s score=%.3f rect=(%.1f,%.1f,%.1f,%.1f) points=%d"
                      % (index, hand.label, hand.score, b.x1, b.y1, b.x2, b.y2,
                         len(hand.keypoints.points)))
    finally:
        camera.close()


if __name__ == "__main__":
    main()
