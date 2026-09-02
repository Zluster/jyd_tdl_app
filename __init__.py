"""dara：CV184x 板端 Python SDK。

    from dara import *        # camera / image / nn / lv 四个子模块
    from dara import camera, image, nn, lv   # 或按需导入

- camera  VPSS 通道取帧（zero-copy Frame / 紧凑 RGB Image）
- image   图像处理（_maix_image 直通：find_qrcodes/find_blobs/...）
- nn      .mud 模型统一加载推理（六族自动识别，nn.load 即用）
- audio   AI/AO 录放、声纹识别、流式 ASR 与关键词检测
- lv      嵌入 LVGL 代理（UI 由 jyd-ui 线程自转，lv 任意线程可用）

原生依赖（mpy.so / tdl_py.so / _maix_image*.so）捆绑在 jyd/_libs/，
import jyd 时注册加载器：进程内这三个模块名一律解析到包内自带副本
（不依赖部署目录、不受系统里其他副本干扰）。要单独用底层接口，
先 import jyd 再 import tdl_py 即可。MicroPython 侧驱动（fs_driver +
drive 显示/触摸）

import 本身不碰硬件：首次用到哪块才初始化哪块（首次触碰 lv 时建
显示通路，首次取相机时 open 通道），进程退出自动清理。

与 launcher / ai_cycle 互斥运行（VO/OSD/相机通道独占），跑 jyd
脚本前先停掉它们。

最小示例（二维码扫描 + 屏幕显示）：

    from dara import camera, lv

    label = lv.label(lv.screen_active())
    label.set_style_text_color(lv.color_white(), 0)
    while True:
        img = camera.read_image()       # rgb 通道 RGB 图（剥填充的拷贝）
        mks = img.find_qrcodes()
        label.set_text(mks[0]["payload"] if mks else "scanning...")
        lv.show()                       # 相机 read 已按帧率起搏，无需限速
"""

import glob
import importlib
import importlib.util
import os
import sys

_SUBMODULES = (
    "camera",
    "image",
    "nn",
    "audio",
    "lv",
    "bus",
    "core",
    "device",
    "iot",
    "peripheral",
)

#: from dara import * 导出的名字（经下方 __getattr__ 懒加载子模块）
__all__ = list(_SUBMODULES)

_LIB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_libs")
#: 捆绑的原生扩展模块名（文件在 jyd/_libs/，模块名是编译期定死的顶层名）
_BUNDLED_LIBS = ("mpy", "tdl_py", "tdl_audio", "_maix_image")


class _BundledLibFinder:
    """把 mpy / tdl_py / tdl_audio / _maix_image 的 import 定向到 jyd/_libs 内的副本。

    插在 sys.meta_path 最前（先于 PathFinder）：无论 jyd 被放在哪、
    sys.path 上有没有同名 .so（如 /root/launcher 下的旧副本），进程内
    始终加载包内捆绑版本，杜绝版本错乱。_libs 缺文件时返回 None，
    回退常规 import（开发机等场景）。"""

    def find_spec(self, name, path=None, target=None):
        if name not in _BUNDLED_LIBS:
            return None
        exact = os.path.join(_LIB_DIR, name + ".so")
        if os.path.exists(exact):
            return importlib.util.spec_from_file_location(name, exact)
        tagged = sorted(glob.glob(os.path.join(_LIB_DIR, name + ".*.so")))
        if tagged:
            return importlib.util.spec_from_file_location(name, tagged[0])
        return None


def _install_lib_finder():
    for f in sys.meta_path:
        if type(f).__name__ == "_BundledLibFinder":
            return
    sys.meta_path.insert(0, _BundledLibFinder())


_install_lib_finder()


def __getattr__(name):
    if name in _SUBMODULES:
        return importlib.import_module("." + name, __name__)
    raise AttributeError("module %r has no attribute %r" % (__name__, name))


def __dir__():
    return sorted(set(globals()) | set(_SUBMODULES))
