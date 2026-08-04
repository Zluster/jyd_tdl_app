"""麦克风 RTP PCM 的 FFT 频谱 demo。

UDP 127.0.0.1:9999 收 RTP（12 字节标准头 + s16le mono 16kHz PCM，
160 字节负载/包 = 5ms 音频，200 包/秒），攒够 _SAMPLES 点做
numpy rfft，幅值转 dB 后画成 _BARS 根竖条（LinePool 粗线）。

显示量程自适应：固定 dB 范围对不上麦克风电平时所有柱子会一样高——
天花板快攻慢放、地板跟踪噪声中位数、竖条时间平滑防闪烁。

socket 非阻塞：on_update 只排空当前已到的数据报，绝不阻塞 UI；
积压过多时丢旧数据，保证显示近实时频谱。

不继承 VisionApp：本应用不需要摄像头。
"""

import socket

from appfw import App, AppContext
from apps.vision import LinePool

_RTP_PORT = 9999
_RTP_HDR = 12            # 标准 RTP 头（无 CSRC/扩展）
_SAMPLE_RATE = 16000     # PLAT_FREQ：按发送端实际改
_SAMPLES = 1024          # FFT 点数（2 的幂）；1024/16000 = 64ms 音频
_BARS = 48               # 48 条 × 15px 步进铺满 720 宽
_BAR_BASE = 440          # 竖条底边 y
_BAR_MAX_H = 400

_DEBUG = True            # 校准用：打印原始样本与 dB 统计，定版后改 False


class SpectrumApp(App):
    name = "spectrum"

    def on_create(self, ctx: AppContext):
        
        import numpy as np
        self._np = np
        self._ctx = ctx
        lv, scr = ctx.lv, ctx.screen

        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)
        lv.layer_bottom().set_style_bg_opa(255, lv.STATE.DEFAULT)

        self._label = lv.label(scr)
        self._label.set_text("spectrum: waiting rtp...")
        self._label.set_style_text_color(lv.color_white(), 0)
        self._label.align(lv.ALIGN.TOP_MID, 0, 8)

        # 条宽 12、步进 15：竖直粗线即柱状条
        self._pool = LinePool(ctx, _BARS, width=12)

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", _RTP_PORT))
        self._sock.setblocking(False)
        self._pcm = bytearray()

        self._hi = 120.0              # 量程天花板（快攻慢放）
        self._lo = 60.0               # 量程地板（噪声中位数跟踪）
        self._smooth = np.zeros(_BARS)   # 竖条时间平滑缓冲
        self._dbg_n = 0

    def on_update(self, dt_ms: int):
        while True:
            try:
                pkt, _ = self._sock.recvfrom(4096)
            except BlockingIOError:
                break
            if len(pkt) > _RTP_HDR:
                self._pcm += pkt[_RTP_HDR:]

        need = _SAMPLES * 2   # s16le 每样本 2 字节
        if len(self._pcm) < need:
            return
        if len(self._pcm) > need * 4:
            # 消费跟不上就丢旧数据，把延迟压在 ~64ms
            del self._pcm[:len(self._pcm) - need]
        frame = self._pcm[:need]
        del self._pcm[:need]

        samples_i16 = self._np.frombuffer(frame, dtype=self._np.int16)
        if _DEBUG and self._dbg_n == 0:
            # 语音应是连续变化的小数值（±几十到几千）；
            # 若是 ±几千剧烈交替，说明字节序/格式不对
            print("[spectrum] raw:", samples_i16[:12].tolist(), flush=True)
        samples = samples_i16.astype(self._np.float64)
        samples -= samples.mean()   # 去 DC：麦克风直流偏置会让 bin 0 常年顶格
        mags = self._np.abs(self._np.fft.rfft(samples * self._np.hanning(_SAMPLES)))
        db = 20 * self._np.log10(mags + 1)

        # 每根条合并 k 个 bin 取最大，铺满 0~7.5kHz
        k = len(db) // _BARS
        bars = db[:k * _BARS].reshape(_BARS, k).max(axis=1)

        fmax = float(bars.max())
        fmed = float(self._np.median(bars))
        self._hi += (fmax - self._hi) * (0.5 if fmax > self._hi else 0.01)
        self._lo += (fmed - self._lo) * 0.05
        self._lo = min(self._lo, self._hi - 10)
        span = max(self._hi - self._lo, 1.0)

        self._smooth = 0.6 * self._smooth + 0.4 * bars
        h_arr = (self._smooth - self._lo) * (_BAR_MAX_H / span)
        self._np.clip(h_arr, 0, _BAR_MAX_H, out=h_arr)

        step = 720 // _BARS
        # 一次 bytes 调用更新全部竖条；逐根 set 会把 25Hz tick 拖到 5FPS
        self._pool.set_bars(step // 2, step, _BAR_BASE,
                            h_arr.astype("<i2").tobytes())

        peak = int(self._np.argmax(mags[1:])) + 1   # 跳过直流 bin
        self._label.set_text("peak %d Hz" % (peak * _SAMPLE_RATE // _SAMPLES))

        if _DEBUG:
            self._dbg_n += 1
            if self._dbg_n % 30 == 1:
                print("[spectrum] db min/med/max: %.1f/%.1f/%.1f  lo %.1f hi %.1f"
                      % (float(bars.min()), fmed, fmax, self._lo, self._hi),
                      flush=True)

    def on_destroy(self):
        try:
            self._sock.close()
        except Exception:
            pass
