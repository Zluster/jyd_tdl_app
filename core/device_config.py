"""持久化的板级设备配置。

配置固定保存在 /mnt/data/jyd/device.json。/mnt/data 是本产品约定的用户
数据目录，位于可写 eMMC 文件系统，掉电重启后仍保留且不随应用目录更新。
"""

import json
import os
import tempfile
import threading


CONFIG_PATH = "/mnt/data/jyd/device.json"
CONFIG_VERSION = 1
_LOCK = threading.RLock()


def _read_file(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            value = json.load(handle)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, TypeError):
        return {}


def _read_root():
    value = _read_file(CONFIG_PATH)
    if not isinstance(value.get("version"), int):
        value["version"] = CONFIG_VERSION
    return value


def _write_root(value):
    directory = os.path.dirname(CONFIG_PATH)
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".device.", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, CONFIG_PATH)
        # 尽量把目录项也刷入存储；某些精简 rootfs 没有目录 fsync，忽略即可。
        try:
            directory_fd = os.open(directory, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError:
            pass
    finally:
        try:
            os.unlink(temporary)
        except OSError:
            pass


def load_section(name, defaults=None, legacy_path=None, legacy_filter=None):
    """读一个配置分区；首次使用时可从旧的应用私有 JSON 自动迁移。"""
    with _LOCK:
        root = _read_root()
        section = root.get(name)
        if not isinstance(section, dict) and legacy_path:
            section = _read_file(legacy_path)
            if callable(legacy_filter):
                section = legacy_filter(section)
            if section:
                root[name] = section
                _write_root(root)
        if not isinstance(section, dict):
            section = {}
        result = dict(defaults or {})
        result.update(section)
        return result


def save_section(name, values):
    """原子替换一个配置分区，其他应用的分区保持不变。"""
    if not isinstance(values, dict):
        raise TypeError("device config section must be a dict")
    with _LOCK:
        root = _read_root()
        root["version"] = CONFIG_VERSION
        root[name] = dict(values)
        _write_root(root)


def camera_source(default="front"):
    """返回全局默认相机来源：front 或 rear。"""
    camera = load_section("camera")
    source = camera.get("source")
    if source in ("front", "rear"):
        return source
    # 兼容设置页已经保存但 camera 分区尚未建立的版本。
    settings = load_section("settings")
    return "rear" if settings.get("camera_mode") == "rear_only" else default


def set_camera_source(source):
    if source not in ("front", "rear"):
        raise ValueError("camera source must be 'front' or 'rear'")
    save_section("camera", {"source": source})
