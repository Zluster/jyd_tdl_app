#!/usr/bin/env python3
"""tdl_py 模型反复加载/释放压力测试.

在 CV184X 大核上运行: 每轮按顺序 load -> reset 五个模型, 每 N 轮打印一次
进程与系统内存 (VmRSS/VmSize/MemAvailable/MemFree/CmaFree/ION), 用于观察
模型加载路径是否泄漏 (泄漏表现为报告值随轮数单调漂移).

每次 load 后断言 model.initialized 为真, 并统计各模型 load() 的真实耗时
(load avg 行): 真实 bmodel 加载为几十 ms 量级, 若只有 <1ms 说明该路径
没有真正触碰 NPU.

用法 (板端):
    python3 tdl_py_model_reload_stress.py                # 无限循环, Ctrl-C 结束
    python3 tdl_py_model_reload_stress.py --rounds 100
    python3 tdl_py_model_reload_stress.py --only face_dense
"""

import argparse
import gc
import glob
import os
import sys
import time

try:
    import tdl_py
except ImportError:
    sys.path.insert(0, "/root")
    import tdl_py

DEFAULT_MODEL_DIR = "/root/tdl_app_sdk_cv184x/configs/model_specs/"

# 类名用字符串在运行时解析: media-minimal 构建缺少 NPU 类时能给出明确报错.
MODEL_TABLE = [
    ("scrfd_det", "Detector", "scrfd_real.mud"),
    ("yolov8n_det", "Detector", "yolov8n_det_coco80.mud"),
    ("yolov8n_seg", "InstanceSegmenter", "yolov8n_seg_coco80.mud"),
    ("hand_keypoint", "KeypointDetector", "keypoint_hand_128.mud"),
    ("face_dense", "FaceDenseLandmark", "face_dense_real.mud"),
]

FIELDS = ("rss", "vsz", "avail", "free", "cma", "ion")


def read_kv_kb(path, keys):
    """解析 /proc 下 `Key:   123 kB` 形式的行, 返回 {key: kB}."""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                name, sep, rest = line.partition(":")
                if sep and name in keys:
                    out[name] = int(rest.split()[0])
    except (OSError, ValueError, IndexError):
        pass
    return out


def ion_alloc_kb():
    """尽力读取 ION debugfs 的已分配字节数; 不可用时返回 None."""
    total = 0
    found = False
    for path in glob.glob("/sys/kernel/debug/ion/*/alloc_mem"):
        try:
            with open(path) as f:
                total += int(f.read().split()[0])
            found = True
        except (OSError, ValueError, IndexError):
            pass
    return total / 1024.0 if found else None


def snapshot():
    status = read_kv_kb("/proc/self/status", ("VmRSS", "VmSize"))
    meminfo = read_kv_kb("/proc/meminfo", ("MemFree", "MemAvailable", "CmaFree"))
    return {
        "rss": status.get("VmRSS"),
        "vsz": status.get("VmSize"),
        "avail": meminfo.get("MemAvailable"),
        "free": meminfo.get("MemFree"),
        "cma": meminfo.get("CmaFree"),
        "ion": ion_alloc_kb(),
    }


def fmt_mb(kb):
    return "%.1fM" % (kb / 1024.0)


def fmt_delta(kb):
    return "%+.2fM" % (kb / 1024.0)


def report(tag, snap, base, prev):
    parts = []
    for key in FIELDS:
        value = snap.get(key)
        if value is None:
            continue
        text = "%s %s" % (key, fmt_mb(value))
        if base is not None and base.get(key) is not None:
            text += " (%s" % fmt_delta(value - base[key])
            if prev is not None and prev.get(key) is not None:
                text += "|%s" % fmt_delta(value - prev[key])
            text += ")"
        parts.append(text)
    print("[%s] %s" % (tag, "  ".join(parts)), flush=True)


def report_load_stats(stats):
    parts = ["%s %.0fms" % (label, 1000.0 * total / count)
             for label, (total, count) in stats.items() if count > 0]
    if parts:
        print("        load avg: %s" % "  ".join(parts), flush=True)


def main():
    parser = argparse.ArgumentParser(
        description="tdl_py model load/unload stress test")
    parser.add_argument("--rounds", type=int, default=0,
                        help="total rounds, 0 = run until Ctrl-C (default 0)")
    parser.add_argument("--report-every", type=int, default=10,
                        help="print memory every N rounds (default 10)")
    parser.add_argument("--model-dir", default=DEFAULT_MODEL_DIR)
    parser.add_argument("--only", default="",
                        help="only test models whose label contains this text")
    args = parser.parse_args()

    models = []
    for label, class_name, mud in MODEL_TABLE:
        if args.only and args.only not in label:
            continue
        cls = getattr(tdl_py, class_name, None)
        if cls is None:
            sys.exit("tdl_py.%s missing: module built with "
                     "TDL_APP_MEDIA_MINIMAL=ON?" % class_name)
        path = os.path.join(args.model_dir, mud)
        if not os.path.isfile(path):
            sys.exit("model spec not found: %s" % path)
        models.append((label, cls, path))
    if not models:
        sys.exit("no model matches --only %r" % args.only)

    tdl_py.init()

    print("testing %d model(s): %s" % (len(models),
                                       ", ".join(m[0] for m in models)))
    print("legend: value (delta vs baseline | delta vs previous report); "
          "load avg = real bmodel load time per model", flush=True)

    gc.collect()
    base = snapshot()
    report("baseline", base, None, None)

    prev = base
    prev_time = time.monotonic()
    load_stats = {label: [0.0, 0] for label, _, _ in models}
    round_no = 0
    try:
        while args.rounds <= 0 or round_no < args.rounds:
            round_no += 1
            for label, cls, path in models:
                model = cls()
                try:
                    begin = time.monotonic()
                    model.load(path)
                    load_stats[label][0] += time.monotonic() - begin
                    load_stats[label][1] += 1
                    if not model.initialized:
                        raise RuntimeError(
                            "%s.load() returned but initialized is False"
                            % label)
                    # time.sleep(0.4)
                    model.reset()
                    # time.sleep(0.4)
                except Exception:
                    print("FAILED at round %d, model %s" % (round_no, label),
                          flush=True)
                    report("failure", snapshot(), base, prev)
                    raise
                del model
            if round_no % args.report_every == 0:
                gc.collect()
                snap = snapshot()
                now = time.monotonic()
                per_round = (now - prev_time) / args.report_every
                report("round %d, %.2fs/round" % (round_no, per_round),
                       snap, base, prev)
                report_load_stats(load_stats)
                load_stats = {label: [0.0, 0] for label, _, _ in models}
                prev = snap
                prev_time = now
    except KeyboardInterrupt:
        print("\ninterrupted at round %d" % round_no)
    gc.collect()
    report("final round %d" % round_no, snapshot(), base, prev)
    report_load_stats(load_stats)


if __name__ == "__main__":
    main()
