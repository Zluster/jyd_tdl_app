#!/usr/bin/env python3
"""Sipeed/Maix-style sample-bank self-learning classifier demo.

Use a compact INT8 feature model such as feature_cviface.mud for --camera.
feature_clip_image.mud is supported for image-bank workflows only on CV184X.
"""

import argparse
import os
import tdl_py


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", required=True)
    p.add_argument("--sample", action="append", nargs=2, metavar=("LABEL", "IMAGE"),
                   help="add one labeled image; may be repeated")
    p.add_argument("--bank", default="/tmp/tdl_classes.bank")
    p.add_argument("--image", help="classify one image file")
    p.add_argument("--camera", action="store_true",
                   help="classify live AI-channel frames")
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--top-k", type=int, default=3)
    args = p.parse_args()

    classifier = tdl_py.SelfLearningClassifier()
    classifier.load(args.model)
    for label, image in args.sample or []:
        classifier.add_sample(label, image)
    if args.sample:
        classifier.learn()  # compatibility no-op; prototypes are online
        classifier.save(args.bank)
        print("saved bank=%s classes=%d samples=%d" %
              (args.bank, classifier.class_count, classifier.sample_count))
    elif os.path.exists(args.bank):
        classifier.load_bank(args.bank)

    # Feature models use the same 640x640 AI source as the C++ classifier.
    if classifier.sample_count == 0:
        raise RuntimeError("feature bank is empty; pass --sample or an existing --bank")

    if args.image:
        result = classifier.classify_image(args.image, top_k=args.top_k)
        print("image=%s feature_dim=%d" % (args.image, result.feature_dim))
        for item in result.classes:
            print("  label=%s score=%.4f samples=%d" %
                  (item.label, item.score, item.sample_count))

    if args.camera:
        camera = tdl_py.VpssCamera.ai(timeout_ms=1000)
        try:
            for index in range(max(1, args.frames)):
                with camera.read() as frame:
                    result = classifier.classify(frame, top_k=args.top_k)
                print("frame=%d feature_dim=%d" % (index, result.feature_dim))
                for item in result.classes:
                    print("  label=%s score=%.4f samples=%d" %
                          (item.label, item.score, item.sample_count))
        finally:
            camera.close()


if __name__ == "__main__":
    main()
