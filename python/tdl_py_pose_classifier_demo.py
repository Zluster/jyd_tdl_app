#!/usr/bin/env python3
"""Bounded MaixPy-style online body-pose classification for CV184X."""

import argparse

import tdl_py


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model", default="configs/model_specs/pose_yolov8.mud"
    )
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--threshold", type=float, default=0.05)
    parser.add_argument("--smooth-frames", type=int, default=5)
    args = parser.parse_args()

    pose = tdl_py.PoseClassifier(
        args.model,
        keypoint_threshold=args.threshold,
        smooth_frames=args.smooth_frames,
    )
    camera = tdl_py.VpssCamera.live(timeout_ms=1000)
    try:
        for index in range(max(1, args.frames)):
            with camera.read() as frame:
                result = pose.classify(frame)
            print(
                "frame=%d pose=%s raw=%s confidence=%.3f points=%d "
                "keypoint_ms=%.3f total_ms=%.3f"
                % (
                    index,
                    result.label,
                    result.raw_label,
                    result.confidence,
                    len(result.keypoints.points),
                    result.keypoint_ms,
                    result.total_ms,
                )
            )
    finally:
        camera.close()


if __name__ == "__main__":
    main()
