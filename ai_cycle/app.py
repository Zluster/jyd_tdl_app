"""独立 AI 模型轮换开机应用：python3 app.py 直接运行，不依赖 launcher 菜单。

宿主显示通路复制自 launcher/launcher.py（VPSS/VO 链路 + OSD 双缓冲 +
flush 回调 + 30Hz tick 循环）；MicroPython 环境是 mpy_env.py 去掉
mpy_home 菜单的最小版（内嵌源码，不会拉起主菜单 UI）；
模型轮换/推理/绘制逻辑见 ai_app.AiCycler。

注意：与 launcher 互斥（都独占 VO/OSD/相机通道），跑本应用前先停 launcher。
"""

import os
import sys

# 复用上级 launcher 目录里的基础库：mpy.so / tdl_py.so / mpyc 包
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import asyncio
import faulthandler
import signal
import time
import traceback

import mpy      # 仅用于显示通路（set_flush_buf / set_flush_callback）
import tdl_py
from mpyc import Mpyc

from ai_app import AiCycler

#: 模型 spec 目录与轮换列表（顺序即播放顺序；kwargs 目前只有 dense）
_AI_MODEL_PATH = "/root/tdl_app_sdk_cv184x/configs/model_specs/"
_MODELS = [
    ("scrfd_real.mud", {}),
    ("yolov8n_det_coco80.mud", {}),
    ("yolov8n_seg_coco80.mud", {}),
    ("keypoint_hand_128.mud", {}),
    ("face_dense_real.mud", {"dense": False}),   # 只画 SCRFD 五点，与 launcher 一致
]

#: MicroPython 最小环境 = launcher/mpy_env.py 去掉 mpy_home 菜单
_MP_ENV = r"""
import lvgl as lv
import sys
sys.path.append('/root/mpy')
import fs_driver

lv.init()
fs_drv = lv.fs_drv_t()
fs_driver.fs_register(fs_drv, 'L')

import drive

lv.screen_active().set_style_bg_opa(0, lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
"""


def _read_mem_mb():
    """本进程 RSS 内存（MB），读 /proc/self/status（板端无 psutil，自读 /proc）。"""
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except Exception:
        pass
    return 0.0


def _read_cpu_time():
    """本进程累计 CPU 时间（秒，utime+stime，含全部线程），读 /proc/self/stat。"""
    try:
        with open("/proc/self/stat") as f:
            # comm 字段可能含空格，从右按 ')' 切开后再按空格分
            parts = f.read().rsplit(")", 1)[1].split()
        hz = os.sysconf("SC_CLK_TCK")
        return (int(parts[11]) + int(parts[12])) / hz   # utime + stime
    except Exception:
        return 0.0


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
    cycler = None
    live_cam = None
    try:
        osd_screen.create()
        osd_screen.attach(group=1, channel=0, layer=1)
        back, front = osd_screen.persistent_pair()
        mpy.set_flush_buf(back.data, front.data)
        osd_screen.set_visible(True)

        is_flush = False
        # 仿 drive/mpy_display.py calc_fps：每帧累计，每 ~1.235s 打一次
        # 帧率，同一行附内存/CPU（1235ms 阈值照抄，避免与整秒打印混叠）
        flush_fcnt = 0
        flush_ts = time.monotonic()
        cpu_prev = _read_cpu_time()

        def on_flush(x1, y1, x2, y2, data):
            nonlocal is_flush, media_links, flush_fcnt, flush_ts, cpu_prev
            osd_screen.update()

            try:
                flush_fcnt += 1
                now = time.monotonic()
                elapsed = now - flush_ts
                if elapsed >= 1.235:
                    cpu_now = _read_cpu_time()
                    cpu_pct = (cpu_now - cpu_prev) / elapsed * 100.0
                    print("flush => %.1f FPS | RSS %.1f MB | CPU %.1f%%"
                          % (flush_fcnt / elapsed, _read_mem_mb(), cpu_pct))
                    flush_fcnt = 0
                    flush_ts = now
                    cpu_prev = cpu_now
            except Exception:
                pass   # 统计打印失败不能影响 flush 通路

            if (not is_flush) and (not tdl_py.vo_is_enabled(0)):
                is_flush = True
                vo = tdl_py.VoOutput()
                vo.open()
                media_links.append(vo)
        mpy.set_flush_callback(on_flush, bytes_per_pixel=4)

        with Mpyc(heap_size=4 * 1024 * 1024) as m:
            m.exec(_MP_ENV, capture=False)

            # 预览通路保活：照搬 AppFramework 的框架级 live 相机（只 open 不读）
            live_cam = tdl_py.VpssCamera.live()
            live_cam.open()

            lv = m.env.lv
            cycler = AiCycler(m, lv, lv.screen_active(),
                              [(_AI_MODEL_PATH + name, kw)
                               for name, kw in _MODELS])
            cycler.start()

            # 30Hz tick：LVGL 心跳 + 轮换调度（节奏控制复制自 lv_tick_loop）
            freq = 30
            delay = 1.0 / freq
            ms = int(delay * 1000)
            upd_err = 0
            next_tick = time.perf_counter()
            while True:
                try:
                    m.tick(ms)
                except RuntimeError as e:
                    # MicroPython 侧异常不终止 UI 循环，打印后继续
                    print("Exception in task_handler:", e)
                try:
                    cycler.update(ms)
                except Exception as e:
                    # 开机应用无处回退：调度异常只节流打印，不退出
                    upd_err += 1
                    if upd_err % 25 == 1:
                        print("ai_cycle: update error (%d): %s" % (upd_err, e))
                if cycler.fatal:
                    # 全部模型加载失败：退出（非零码），交给外部看门狗拉起
                    raise RuntimeError("all models failed to load, giving up")
                next_tick += delay
                now = time.perf_counter()
                if next_tick < now - delay:   # 欠账超过一帧：不补，直接对齐到现在
                    next_tick = now
                sleep_time = next_tick - now
                await asyncio.sleep(sleep_time if sleep_time > 0 else 0)
    finally:
        if cycler is not None:
            cycler.close()   # 停 worker + 释放模型/ai 相机（不走桥，安全）
        if live_cam is not None:
            try:
                live_cam.close()
            except Exception as e:
                print("ai_cycle: live camera close error:", e)
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            loop.remove_signal_handler(sig)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        mpy.set_flush_callback(None)
        mpy.set_flush_buf(None)
        osd_screen.destroy()


if __name__ == "__main__":
    # 死因打印两道防线：
    # - faulthandler：段错误/abort 等原生崩溃（tdl_py/mpy 是 C 扩展，崩溃时
    #   Python 异常机制不起作用），向 stderr 转储崩溃时的 Python 栈；
    # - 顶层 try/except：Python 异常打印完整 traceback 后以非零码退出；
    #   信号取消（main_task.cancel）与正常返回分别给出退出原因。
    # 注意：SIGKILL / OOM-killer 无法被进程内捕获，只能靠外部日志（dmesg）。
    faulthandler.enable()
    try:
        asyncio.run(main())
        print("ai_cycle: exit normally")
    except (KeyboardInterrupt, asyncio.CancelledError):
        print("ai_cycle: exit (cancelled by SIGINT/SIGTERM/SIGHUP)")
    except BaseException:
        print("ai_cycle: FATAL EXIT, traceback below:")
        traceback.print_exc()
        sys.exit(1)