#!/usr/bin/env python3
"""Generate a deterministic sequence for tracker and line-counting regression."""

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--frames", type=int, default=100)
    return parser.parse_args()


def main():
    args = parse_args()
    source = cv2.imread(args.input, cv2.IMREAD_COLOR)
    if source is None:
        raise SystemExit(f"failed to read input: {args.input}")

    height, width = source.shape[:2]
    output_dir = Path(args.output_dir)
    frames_dir = output_dir / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    # The target starts left of the count line, crosses it, and is briefly occluded.
    target_w = max(48, width // 5)
    target_h = max(48, height // 3)
    crop = source[max(0, height // 2 - target_h // 2):height // 2 + target_h // 2,
                  max(0, width // 2 - target_w // 2):width // 2 + target_w // 2].copy()
    if crop.size == 0:
        raise SystemExit("input image is too small")

    records = []
    for index in range(args.frames):
        frame = cv2.resize(source, (width, height), interpolation=cv2.INTER_LINEAR)
        scale = 0.85 + 0.30 * index / max(1, args.frames - 1)
        resized = cv2.resize(crop, (max(1, int(crop.shape[1] * scale)),
                                    max(1, int(crop.shape[0] * scale))))
        x = int((width - resized.shape[1]) * index / max(1, args.frames - 1))
        y = max(0, height // 2 - resized.shape[0] // 2)
        occluded = args.frames * 45 // 100 <= index < args.frames * 55 // 100
        if not occluded:
            frame[y:y + resized.shape[0], x:x + resized.shape[1]] = resized
        cv2.line(frame, (width // 2, 0), (width // 2, height - 1), (0, 255, 255), 2)
        cv2.imwrite(str(frames_dir / f"{index:04d}.jpg"), frame)
        records.append({
            "frame": index,
            "id": 1,
            "visible": not occluded,
            "bbox": [x, y, x + resized.shape[1], y + resized.shape[0]],
        })

    (output_dir / "ground_truth.json").write_text(
        json.dumps({"count_line_x": width // 2, "tracks": records}, indent=2),
        encoding="utf-8")


if __name__ == "__main__":
    main()
