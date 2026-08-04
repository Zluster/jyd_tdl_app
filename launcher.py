"""launcher: CPython 宿主入口。

职责划分：
- 硬件/显示通路（tdl_py 的 VO/VPSS/OSD、framebuffer 注册）在本文件
- 与 MicroPython 的一切交互走 mpyc（typed bridge + 属性代理），
  不再有内联的 MicroPython 封装类和 tick 源码模板
"""

import asyncio
import signal
import time

import mpy      # 仅用于显示通路（set_flush_buf / set_flush_callback）
import tdl_py
from mpyc import Mpyc
from appfw import AppFramework
from apps.demo.app import DemoApp
from apps.qrcode.app import QrApp
# from apps.blobcolor.app import BlobColorApp
# from apps.apriltag.app import AprilTagApp
from apps.barcode.app import BarcodeApp
from apps.findlines.app import FindLinesApp
from apps.spectrum.app import SpectrumApp
from apps.ai.app import AiApp

#: AiApp 默认模型（.mud，按板上实际路径改；
#: [basic] model 的相对路径相对 spec 文件位置解析，bmodel 必须存在）
_AI_MODEL_PATH = "/root/tdl_app_sdk_cv184x/configs/model_specs/"

#: 应用框架实例，main() 里创建。进入应用：fw.launch(SomeApp())
fw: AppFramework = None


async def lv_tick_loop(m: Mpyc, freq: int = 25):
    """LVGL 事件循环 + 应用框架安全点。"""
    delay = 1.0 / freq
    ms = int(delay * 1000)
    next_tick = time.perf_counter()
    while True:
        try:
            m.tick(ms)
        except RuntimeError as e:
            # MicroPython 侧异常不终止 UI 循环，打印后继续
            print("Exception in task_handler:", e)
        if fw is not None:
            fw.process(ms)   # 挂起的 launch/exit 在这里执行（task_handler 之外）
        next_tick += delay
        now = time.perf_counter()
        if next_tick < now - delay:   # 欠账超过一帧：不补，直接对齐到现在
            next_tick = now
        sleep_time = next_tick - now
        await asyncio.sleep(sleep_time if sleep_time > 0 else 0)


def setup_display_path():
    keep = []
    if tdl_py.get_bind_source_vpss(1) is None:          # grp1 输入端没人绑
        preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
        preview.bind()
        keep.append(preview)
    if tdl_py.get_bind_source_vo(0, 0) is None:         # VO layer0/ch0 没人绑
        display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
        display.bind()
        keep.append(display)
    return keep


async def main():
    media_links = setup_display_path()  # 持有到 main 结束，勿删

    osd_screen = tdl_py.Osd(handle=202, canvas_count=2)

    loop = asyncio.get_running_loop()
    main_task = asyncio.current_task()
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        loop.add_signal_handler(sig, main_task.cancel)
    try:
        osd_screen.create()
        osd_screen.attach(group=1, channel=0, layer=1)
        back, front = osd_screen.persistent_pair()
        mpy.set_flush_buf(back.data, front.data)
        osd_screen.set_visible(True)

        is_flush = False
        def on_flush(x1, y1, x2, y2, data):
            nonlocal is_flush, media_links
            osd_screen.update()
            
            if (not is_flush) and (not tdl_py.vo_is_enabled(0)):
                is_flush = True
                vo = tdl_py.VoOutput()
                vo.open()
                media_links.append(vo)
        mpy.set_flush_callback(on_flush, bytes_per_pixel=4)

        def on_app_launch(app_name : str):
            print(app_name)
            if app_name == "Line tracking":
                fw.launch(DemoApp())
            elif app_name == "Speech Synthesis":
                # fw.launch(AprilTagApp())
                fw.launch(QrApp())
            elif app_name == "SPECTRUM ANALYSIS":
                fw.launch(SpectrumApp())
            elif app_name == "Chatbot":
                fw.launch(BarcodeApp())
            elif app_name == "AI Classifier":
                fw.launch(FindLinesApp())
            elif app_name == "Face Recognizer2":
                fw.launch(AiApp(_AI_MODEL_PATH + "scrfd_real.mud"))
            elif app_name == "Face Emotion":
                fw.launch(AiApp(_AI_MODEL_PATH + "yolov8n_det_coco80.mud"))
            elif app_name == "Find blobs":
                fw.launch(AiApp(_AI_MODEL_PATH + "yolov8n_seg_coco80.mud"))  # pose_yolov8
            elif app_name == "Smart DeskLamp":
                fw.launch(AiApp(_AI_MODEL_PATH + "keypoint_hand_128.mud"))
            elif app_name == "Terminal Manager":
                fw.launch(AiApp(_AI_MODEL_PATH + "face_dense_real.mud", dense=False))

        with Mpyc(heap_size=4 * 1024 * 1024) as m:
            m.exec_file("/root/launcher/mpy_env.py", capture=False)
            m.register(on_app_launch)

            global fw
            fw = AppFramework(m)

            tick_task = asyncio.create_task(lv_tick_loop(m, 30))

            while True:
                await asyncio.sleep(0.01)
    finally:
        if fw is not None:
            fw.close()   # 框架级摄像头在程序退出时才关闭
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            loop.remove_signal_handler(sig)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        mpy.set_flush_callback(None)
        mpy.set_flush_buf(None)
        osd_screen.destroy()


if __name__ == "__main__":
    asyncio.run(main())