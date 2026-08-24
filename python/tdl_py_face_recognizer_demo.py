#!/usr/bin/env python3
"""Bounded MaixPy-style online face-recognition demo for CV184X."""

import argparse
import os

import tdl_py


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--detect-model", required=True)
    parser.add_argument("--feature-model", required=True)
    parser.add_argument("--faces", default="/root/faces.bin")
    parser.add_argument("--enroll", metavar="NAME")
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--conf", type=float, default=0.5)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--match", type=float, default=0.85)
    args = parser.parse_args()

    recognizer = tdl_py.FaceRecognizer(
        detect_model=args.detect_model,
        feature_model=args.feature_model,
    )
    if os.path.exists(args.faces):
        recognizer.load_faces(args.faces)

    camera = tdl_py.VpssCamera.live(timeout_ms=1000)
    enrolled = False
    try:
        for index in range(max(1, args.frames)):
            with camera.read() as frame:
                faces = recognizer.recognize(
                    frame, args.conf, args.iou, args.match,
                    get_feature=bool(args.enroll),
                )
            for face in faces:
                print(
                    "frame=%d name=%s score=%.3f matched=%s "
                    "rect=(%.1f,%.1f,%.1f,%.1f)"
                    % (index, face.name, face.score, face.matched,
                       face.x, face.y, face.w, face.h)
                )
            if args.enroll and not enrolled and faces:
                target = max(faces, key=lambda face: face.w * face.h)
                recognizer.add_face(target, args.enroll)
                recognizer.save_faces(args.faces)
                print("enrolled %s, saved %s" % (args.enroll, args.faces))
                enrolled = True
                break
    finally:
        camera.close()

    if args.enroll and not enrolled:
        raise RuntimeError("no face enrolled within %d frames" % args.frames)


if __name__ == "__main__":
    main()
