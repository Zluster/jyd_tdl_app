# 音频接口（CV184X 双系统）

本接口不依赖 Sherpa、ONNX Runtime 或 `.onnx` 模型。声纹、流式语音识别和关键词检测均使用 CPU Kaldi Fbank 前处理与 CV184X BMRT 直接加载的 BF16 bmodel。

## 公共 PCM 格式

`SpeakerRecognizer`、`StreamingAsr` 和 `KeywordSpotter` 的 PCM 参数均为 Python `bytes`：16 kHz、单声道、signed 16-bit little-endian PCM。可由 `tdl_py.Audio().capture_pcm()` 直接取得。

## 基础录音/播放

```python
import tdl_py

audio = tdl_py.Audio()
audio.record_wav("/tmp/test.wav", 3.0)
audio.play_wav("/tmp/test.wav")
pcm = audio.capture_pcm(3.0)
```

- `record_wav(path, seconds=3.0, ...) -> bool`
- `play_wav(path, ...) -> bool`：默认构建只支持标准 PCM WAV，不带 FFmpeg。
- `capture_pcm(seconds=3.0, ...) -> bytes | None`
- `loopback(seconds=3.0, ...) -> bool`
- `set_input_volume()` / `set_output_volume()` / `input_volume()` / `output_volume()`
- `status() -> dict`、`last_error`

硬件错误不向实时界面抛异常；`bool` 方法返回 `False`，对象方法返回 `None`，再查看 `last_error`。

## 声纹识别

模型：`configs/model_specs/speaker_campplus_sv.mud`（CAMPPlus BF16）。

```python
speaker = tdl_py.SpeakerRecognizer()
assert speaker.load("/root/tdl_app_sdk_cv184x/configs/model_specs/speaker_campplus_sv.mud")
pcm = audio.capture_pcm(4.0)
speaker.enroll("alice", pcm)
speaker.save_database("/tmp/speakers.db")
print(speaker.recognize(pcm, threshold=0.60))
```

- `enroll(label, pcm) -> bool`：同名标签覆盖此前样本。
- `recognize(pcm, threshold=0.60) -> {label, score, matched} | None`
- `save_database(path)` / `load_database(path)` / `clear()` / `labels()`

## 流式 ASR

模型：`configs/model_specs/npu_zipformer_zh_14m_asr.mud`，由 encoder / decoder / joiner 三个 BF16 bmodel 组成。

```python
asr = tdl_py.StreamingAsr()
asr.load("/root/tdl_app_sdk_cv184x/configs/model_specs/npu_zipformer_zh_14m_asr.mud")
partial = asr.accept(audio.capture_pcm(0.8))
final = asr.finish()
print(asr.text)
```

- `accept(pcm) -> str | None`：返回本次新解出的文本。
- `finish() -> str | None`：补尾并结束当前语句。
- `reset()`：清除当前流状态，模型保持已加载。
- `text`、`initialized`、`last_error`

## 关键词检测

模型：`configs/model_specs/npu_zipformer_zh_kws.mud`；关键词词表由 `configs/kws_keywords.default.txt` 配置。它使用 RNNT beam search 加关键词前缀约束，不依赖 Sherpa/ONNX。

```python
kws = tdl_py.KeywordSpotter()
kws.load(
    "/root/tdl_app_sdk_cv184x/configs/model_specs/npu_zipformer_zh_kws.mud",
    "/root/tdl_app_sdk_cv184x/configs/kws_keywords.default.txt",
    beam_width=6,
)
hits = kws.accept(audio.capture_pcm(0.8))
print(kws.scores())
```

- `accept(pcm) -> list[dict] | None`：只返回本次新触发的关键词。
- `finish() -> list[dict] | None`
- `scores() -> list[dict]`：每个关键词均含 `name`、`confidence`、`threshold`、`matched_tokens`、`total_tokens`、`matched_text`、`complete`、`triggered`。
- `reset()` / `initialized` / `last_error`

## 板端测试程序

在 SDK 根目录执行，确保同一时刻只有一个程序占用 AI 音频输入：

```sh
. ./env.sh
python3 python/dara_audio_test.py 3
python3 python/dara_speaker_recognition_test.py enroll alice
python3 python/dara_speaker_recognition_test.py recognize
python3 python/dara_asr_test.py 8
python3 python/dara_keyword_spotter_test.py 12
```

也可直接测试 C++ KWS demo：

```sh
./bin/tdl_npu_direct_kws_demo \
  --model-spec configs/model_specs/npu_zipformer_zh_kws.mud \
  --keywords configs/kws_keywords.default.txt \
  --print-scores
```
