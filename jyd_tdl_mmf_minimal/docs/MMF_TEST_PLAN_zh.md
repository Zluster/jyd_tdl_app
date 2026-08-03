# jyd_tdl_mmf_minimal 功能测试计划

本文档用于验证 `jyd_tdl_mmf_minimal` 当前版本的 camera、display、JPEG、HTTP 图传、audio、3A、相册应用，以及重入、压力、多进程互不干扰能力。

## 0. 测试前准备

板端进入安装目录：

```sh
cd /root/jyd_tdl_app_mmf_install
. ./env.sh
chmod +x bin/*
mkdir -p /mnt/sd/photos /tmp/mmf_test
```

如果要测试实时 ISP get，也就是 `camera-control` 中的 AE/AWB/exposure/gain/WB get，需要先升级包含 `MSG_CMD_ISP_SOPHPI_3A_PROXY` setter 支持的小核 yoc。

## 1. 基础系统链路

```sh
./bin/mmf_smoke_test system
```

测试内容：大核库初始化、CVI_MSG 初始化、大小核消息通信状态。

预期现象：输出 `system: failed=0`，并且能看到 `local=1 remote=1 msg=1` 或等价 ready 状态。

备注：每次运行独立进程都会打印 `CVI_MSG client...`，这是大核 CVI_MSG 客户端初始化日志，不是小核错误。

```sh
./bin/mmf_smoke_test list
```

测试内容：列举当前 VPSS 输出能力。

预期现象：能看到 `main/ai/live/subrgb/screen`，包含 `grp/chn/width/height/fmt/scale/depth`。

```sh
./bin/mmf_smoke_test frame live
./bin/mmf_smoke_test frame main
./bin/mmf_smoke_test frame ai
./bin/mmf_smoke_test frame subrgb
```

测试内容：不同 camera source 的 get frame 和 put frame。

预期现象：打印对应分辨率、格式、seq，例如 `frame ai 640x640 fmt=... seq=...`。

## 2. Camera Snapshot

```sh
./bin/mmf_smoke_test snapshot ai /tmp/ai.jpg
./bin/mmf_smoke_test snapshot live /tmp/live.jpg
./bin/mmf_smoke_test snapshot main /tmp/main.jpg
./bin/mmf_smoke_test snapshot subrgb /tmp/subrgb.jpg
ls -lh /tmp/ai.jpg /tmp/live.jpg /tmp/main.jpg /tmp/subrgb.jpg
```

测试内容：各路 camera source 保存 JPEG。

预期现象：命令返回成功，文件存在且大小非 0。

## 3. JPEG / VENC / VDEC

```sh
./bin/mmf_smoke_test jpg-roundtrip live
./bin/mmf_smoke_test jpg-roundtrip main
./bin/mmf_smoke_test jpg-roundtrip subrgb
```

测试内容：取帧、VENC JPEG 编码、VDEC JPEG 解码、one-shot encode/decode。

预期现象：看到 `jpg encode ok`、`jpg decode ok`、`jpg one-shot encode ok`、`jpg one-shot decode ok`。

说明：当前约定 one-shot JPEG 使用 `VENC ch0`，HTTP 图传使用 `VENC ch1`，VDEC JPEG decode 使用 `VDEC ch0`。代码已加入进程间资源锁和 VENC/VDEC 操作锁，避免多进程互相打挂底层 codec。

## 4. Display / OSD / 截图

```sh
./bin/mmf_smoke_test display-frame live
```

测试内容：把 live frame 发送到 VO。

预期现象：屏幕显示 live 或短暂显示一帧，终端输出 `display frame live: ok`。

```sh
./bin/mmf_smoke_test display-control
ls -lh /tmp/mmf_display_snapshot.jpg /tmp/mmf_display_snapshot_osd.jpg
```

测试内容：display bind/unbind、OSD 显示、`include_osd=1` 截图合成。

预期现象：`display control: failed=0`，两个 JPG 文件存在；`mmf_display_snapshot_osd.jpg` 应包含测试 OSD 图块。

```sh
./bin/mmf_display_show live 1
```

测试内容：长期 live 预览和 OSD 显示。

预期现象：屏幕显示 live，叠加彩色 OSD；Ctrl+C 退出后 OSD 清理。

回归重点：`mmf_display_open()` 需要完成 VO device/layer/channel 初始化，不能只做 `CVI_SYS_Init()`。如果终端显示 `showing=1` 但屏幕无图，优先检查小核日志中的 `VO-ERR`、`vo_set_videolayerattr`、`vo_recv_frame`。

## 5. HTTP 图传

```sh
./bin/mmf_jpg_http_server 18090 live 92 8
```

测试内容：HTTP MJPEG 图传，8fps。

浏览器访问：

```text
http://板端IP:18090/stream.mjpg
http://板端IP:18090/snapshot.jpg
```

预期现象：浏览器持续显示 live；终端 `clients` 增加，`frames/bytes` 持续增长。

每帧尽量发送：

```sh
./bin/mmf_jpg_http_server 18090 live 92 0
```

测试内容：`fps=0`，producer frame-driven 模式。

预期现象：客户端连接后有帧就发，帧率更高，CPU/VENC 压力也更高。

单次 HTTP smoke：

```sh
./bin/mmf_smoke_test http-snapshot 18080
./bin/mmf_smoke_test http-stream 18081
./bin/mmf_smoke_test http-push 18082
./bin/mmf_smoke_test http-publish-frame 18083
```

或兼容别名：

```sh
./bin/mmf_smoke_test http snapshot 18080
./bin/mmf_smoke_test http stream 18081
```

预期现象：输出 `http ... ok`，bytes 非 0。

## 6. Codec 资源互不干扰

终端 A：

```sh
./bin/mmf_jpg_http_server 18090 live 92 0
```

终端 B：

```sh
for i in $(seq 1 20); do ./bin/mmf_smoke_test jpg-roundtrip live || break; done
```

测试内容：HTTP 图传使用专用 `VENC ch1`，one-shot JPEG 使用 `VENC ch0`，验证多进程 codec 互不干扰。

预期现象：图传不中断，B 端循环成功；小核不应出现连续 `SendFrame fail chn` 或 panic。

如果图传仍被打断，说明底层 VENC 驱动无法接受图传运行时另一进程频繁 one-shot 编码，后续应把 snapshot/JPEG 编码改成常驻编码服务，而不是每个进程 create/destroy VENC。

## 7. 相册复杂应用

```sh
mkdir -p /mnt/sd/photos
./bin/mmf_camera_album_app /mnt/sd/photos
```

交互按键：

```text
c  拍照，保存 main 主图 + subrgb 缩略图
g  进入相册，显示当前照片
t  显示缩略图，并打印当前照片详细信息
n  下一张
p  上一张
d  打印当前照片详细信息
l  返回 live 预览
r  重新扫描相册目录
q  退出
```

预期现象：

```sh
ls -lh /mnt/sd/photos
```

能看到：

```text
photo_YYYYMMDD_HHMMSS.jpg
photo_YYYYMMDD_HHMMSS_thumb.jpg
```

终端详情包含照片路径、缩略图路径、尺寸、格式、seq、pts、文件大小、时间。

## 8. 音频基础功能

```sh
./bin/mmf_smoke_test audio-read 1
./bin/mmf_smoke_test audio-read 2
```

测试内容：单声道/双声道录音 chunk 读取。

预期现象：`audio read: ok`，1ch bytes 约 320，2ch bytes 约 640。

```sh
./bin/mmf_smoke_test audio-control
```

测试内容：录音/播放 open/close、音量 get/set、3A 配置切换。

预期现象：`audio control: failed=0`。

```sh
./bin/mmf_smoke_test audio-codec
```

测试内容：录音帧 G711A encode。

预期现象：`audio codec encode: ok packet=...`。

说明：如果看到 `vqe has not been already inited yet.`，但命令最终 `ok`，通常只是底层关闭时的 VQE 提示，不代表失败。

```sh
./bin/mmf_smoke_test audio-loopback 1 100 1 24
./bin/mmf_smoke_test audio-loopback 2 100 1 24
```

测试内容：边录边放，mono/stereo。第四个参数 `enable_3a=1` 时打开 AEC/NS/AGC，并把 AO 标记为 AEC reference；第五个参数是 AI/AO 音量。

对比 3A 开关：

```sh
./bin/mmf_smoke_test audio-loopback 1 100 0 16
./bin/mmf_smoke_test audio-loopback 1 100 1 16
```

预期现象：关闭 3A 时更容易听到回声、滋滋声或啸叫；开启 3A 后回声和自激应明显减弱。如果仍有滋滋声，先把音量从 24 降到 16 或 12，再拉开麦克风和喇叭距离。

预期现象：听到回环声音，终端 `failed=0`。如有啸叫，先降低音量或拉开麦克风/喇叭。

## 9. 音频 3A

```sh
./bin/mmf_smoke_test audio-3a-global
./bin/mmf_smoke_test audio-3a
./bin/mmf_smoke_test audio-3a-effect 1 100 16
```

测试内容：全局 3A 参数接口、会话级 3A 状态。

预期现象：`supported=1`，AEC/NS/AGC 参数可 set/get。`audio-3a-effect` 会先跑裸回环，再跑 3A 回环；第二阶段的回声、滋滋声、自激应明显弱于第一阶段。

```sh
./bin/mmf_smoke_test audio-control
```

测试内容：打开输入/输出会话时的 3A 配置和 AEC reference 状态。

预期现象：`audio control: failed=0`。后续如果需要更明确观察 applied/reference 状态，可以补专门 status 命令。

## 10. 重入测试

```sh
./bin/mmf_smoke_test reentry 10
```

测试内容：同进程反复 open/close camera、jpg、audio 等模块。

预期现象：完成 10 轮，无 `Segmentation fault`，小核无 panic。

更强一点：

```sh
./bin/mmf_smoke_test reentry 50
```

预期现象：仍不崩；如果失败，记录第几轮和小核日志。

## 11. 压力测试

```sh
./bin/mmf_smoke_test stress 50
```

测试内容：组合压力，重复 camera + JPEG encode/decode + audio-read。

预期现象：完成 50 轮，无段错误，小核无 panic。

注意：`stress` 会跑 JPEG 编解码。如果 HTTP 图传正在跑，建议先用 `stress-http-safe` 区分问题。

```sh
./bin/mmf_smoke_test stress-http-safe 50
```

测试内容：图传运行时的安全压力项，只做 camera get/put + audio-read，不做 JPEG codec。

预期现象：HTTP 图传不中断，命令完成 `failed=0`。

长时间 HTTP：

```sh
./bin/mmf_jpg_http_server 18090 live 92 8
```

保持浏览器打开 30 分钟。

预期现象：`frames/bytes` 持续增长，内存不明显上涨，浏览器断开/重连后继续正常。

## 12. 多程序互不干扰

图传 + 相册：

```sh
# 终端 A
./bin/mmf_jpg_http_server 18090 live 92 8

# 终端 B
./bin/mmf_camera_album_app /mnt/sd/photos
```

测试内容：HTTP 长跑时拍照、显示相册。

预期现象：图传不中断，相册能拍照和浏览。

图传 + snapshot：

```sh
# 终端 A
./bin/mmf_jpg_http_server 18090 live 92 0

# 终端 B
for i in $(seq 1 30); do ./bin/mmf_camera_snapshot; ls -lh /tmp/ai.jpg; done
```

预期现象：snapshot 成功，图传继续。

图传 + 音频：

```sh
# 终端 A
./bin/mmf_jpg_http_server 18090 live 92 8

# 终端 B
./bin/mmf_smoke_test audio-loopback 1 200
```

预期现象：图传继续，音频回环正常。

音频录放并发：

```sh
# 终端 A
./bin/mmf_smoke_test audio-loopback 1 300

# 终端 B
./bin/mmf_smoke_test audio-read 1
```

预期现象：如果底层 AI 只允许一个 capture session，B 应明确失败或等待，但不能导致小核 panic；如果支持多 session，则两边都成功。

## 13. 需要新小核的 ISP 实时查询测试

```sh
./bin/mmf_smoke_test camera-control
./bin/mmf_smoke_test isp-control
```

测试内容：AE/AWB/exposure/gain/WB 的 set/get，通过 `CVI_MSG` 的 ISP 3A proxy 在小核侧执行真实 ISP 调用。

预期现象：升级新小核后输出 `camera control: failed=0`。

如果未升级小核，可能出现 3A proxy 命令失败，这是 yoc 版本不匹配，不代表大核 minimal 逻辑错误。

## 14. 复杂组合测试

这些测试在单点测试通过后执行，目标是验证多模块长期运行、资源互不干扰、重复连接断开、显示和图传同时存在。

### 14.1 显示 + 图传 + JPEG + 音频 soak

```sh
./bin/mmf_pipeline_soak_app 18090 0 live
```

浏览器打开：

```text
http://板端IP:18090/stream.mjpg
http://板端IP:18090/snapshot.jpg
```

测试内容：屏幕显示 live、OSD 叠加、HTTP 图传、定期 JPEG roundtrip、定期 audio read。

预期现象：屏幕和浏览器都有 live，终端 status 持续刷新；浏览器断开/重连后继续出图；小核无 panic，无连续 `SendFrame fail`。

### 14.2 AV 监控组合

```sh
mkdir -p /tmp/mmf_av_monitor
./bin/mmf_av_monitor_app 18091 300 /tmp/mmf_av_monitor
```

测试内容：列举 VPSS 输出、采样各路 frame、显示 live、启动 HTTP 图传、周期保存 snapshot。

预期现象：运行 300 秒不退出，`/tmp/mmf_av_monitor` 里周期性出现 jpg；浏览器访问 `http://板端IP:18091/stream.mjpg` 正常。

### 14.3 图传后台 + 其它进程并发

终端 A：

```sh
./bin/mmf_jpg_http_server 18090 live 92 0
```

终端 B：

```sh
./bin/mmf_smoke_test stress-http-safe 100
./bin/mmf_smoke_test snapshot live /tmp/live.jpg
./bin/mmf_smoke_test audio-3a-effect 1 80 16
./bin/mmf_smoke_test isp-control
```

测试内容：HTTP 图传长期占用专用 VENC 时，camera/audio/ISP 控制是否互不干扰。

预期现象：图传不中断，B 端命令成功；如果 `isp-control` 失败，优先确认小核是否包含 ISP 3A proxy setter 支持。

### 14.4 相册复杂应用

```sh
mkdir -p /mnt/sd/photos
./bin/mmf_camera_album_app /mnt/sd/photos
```

测试内容：live 预览、拍照、主图保存、缩略图保存、相册图片显示、缩略图显示、照片信息打印。

预期现象：按 `c/g/t/n/p/d/l/q` 都有响应，屏幕显示切换正常，照片文件非 0。

## 15. 本轮精简说明

已移除默认构建中的低价值样例：

```text
mmf_audio_stream: 被 audio_full_duplex_app 和 smoke audio 测试覆盖，默认不再打包
mmf_touch_read: touch 当前后置，默认不再打包独立样例
```

注意：`mmf_minimal_ini_compat` 不能删除。minimal 自己不直接调用 `ini_parse`，但 vendor `libsensor.so` 链接时需要这个符号，因此它是必要的链接兼容层，不属于可删冗余。

暂不删除 public touch API 和 touch 源文件，原因是后续还要基于 `/dev/touchscreen` 做 Linux 触摸实现；现在只是不作为主测试项。

## 16. 接口覆盖矩阵

当前测试目标不是只验证命令能跑，而是覆盖 public API 的真实路径。下面按模块列出已覆盖程度。

### System

```sh
./bin/mmf_smoke_test system
```

覆盖接口：

```text
mmf_system_init
mmf_system_deinit
mmf_system_get_status
mmf_system_wait_ready
mmf_system_last_error
mmf_system_version
mmf_system_set_vpss_scale_mode
mmf_system_get_vpss_scale_mode
```

结论：基础系统接口已覆盖。scale mode 当前主要验证 set/get 链路，实际 VPSS 输出效果还需要配合 `frame ai/subrgb` 和图片肉眼确认。

### Camera

```sh
./bin/mmf_smoke_test list
./bin/mmf_smoke_test frame main
./bin/mmf_smoke_test frame ai
./bin/mmf_smoke_test frame live
./bin/mmf_smoke_test frame subrgb
./bin/mmf_smoke_test snapshot live /tmp/live.jpg
./bin/mmf_smoke_test camera-control
```

覆盖接口：

```text
mmf_camera_get_default_config
mmf_camera_open
mmf_camera_close
mmf_camera_get_status
mmf_camera_list_outputs
mmf_camera_get_frame
mmf_camera_put_frame
mmf_camera_release_frame
mmf_camera_snapshot
mmf_camera_set_scale_mode
mmf_camera_get_scale_mode
mmf_camera_set_ae_mode / get_ae_mode
mmf_camera_set_awb_mode / get_awb_mode
mmf_camera_set_exposure / get_exposure
mmf_camera_set_gain / get_gain
mmf_camera_set_isp_gain / get_isp_gain
mmf_camera_set_wb / get_wb
mmf_camera_set_flip / get_flip
mmf_camera_set_mirror / get_mirror
```

结论：取帧、释放、snapshot 已覆盖。ISP 控制接口由 `camera-control` 覆盖，实时 get/set 依赖小核 `CVI_MSG` ISP 3A proxy；未升级对应 yoc 时不能算完整 ISP 闭环。

### Display

```sh
./bin/mmf_display_show live 1
./bin/mmf_smoke_test display-frame live
./bin/mmf_smoke_test display-control
./bin/mmf_camera_album_app /mnt/sd/photos
```

覆盖接口：

```text
mmf_display_get_default_config
mmf_display_get_default_show_options
mmf_display_open
mmf_display_close
mmf_display_get_status
mmf_display_bind_camera
mmf_display_unbind
mmf_display_show_frame
mmf_display_show_image_file
mmf_display_snapshot
mmf_display_clear
mmf_display_clear_overlay
```

部分覆盖接口：

```text
mmf_display_set_window
```

结论：显示 live、OSD、相册图片显示、截图已覆盖。`set_window` 当前更多是状态/config 级接口，缺少单独测试窗口裁剪/缩放实际显示效果；如果后续不做多窗口显示，可以降级为内部接口或暂不暴露。

### JPEG / VENC / VDEC

```sh
./bin/mmf_smoke_test jpg-roundtrip live
./bin/mmf_smoke_test jpg-roundtrip main
./bin/mmf_smoke_test jpg-roundtrip subrgb
./bin/mmf_camera_album_app /mnt/sd/photos
```

覆盖接口：

```text
mmf_jpg_get_default_encoder_config
mmf_jpg_encoder_open
mmf_jpg_encoder_close
mmf_jpg_encode_frame
mmf_jpg_release_packet
mmf_jpg_encode
mmf_jpg_decode
```

部分覆盖接口：

```text
mmf_jpg_get_default_decoder_config
mmf_jpg_decoder_open
mmf_jpg_decoder_close
mmf_jpg_decode_packet
mmf_jpg_release_frame
```

结论：one-shot encode/decode 和 encoder 对象接口已覆盖。decoder 对象接口目前主要由 roundtrip 间接覆盖，建议后续补一条 `jpg-decoder-session` 专项测试；暂不建议删除，因为它是“流式/会话式解码”的合理主流接口。

### HTTP 图传

```sh
./bin/mmf_jpg_http_server 18090 live 92 8
./bin/mmf_jpg_http_server 18090 live 92 0
./bin/mmf_smoke_test http-snapshot 18080
./bin/mmf_smoke_test http-stream 18081
./bin/mmf_smoke_test http-push 18082
./bin/mmf_smoke_test http-publish-frame 18083
```

覆盖接口：

```text
mmf_jpg_http_get_default_config
mmf_jpg_http_open
mmf_jpg_http_close
mmf_jpg_http_start_stream
mmf_jpg_http_stop_stream
mmf_jpg_http_get_status
mmf_jpg_http_publish_jpeg
mmf_jpg_http_publish_frame
```

结论：pull stream、snapshot、push jpeg、push frame 都已覆盖。需要重点跑断开重连、多客户端、图传同时 snapshot/JPEG 压力，防止 VENC 资源互相影响。

### Audio / 3A / Codec

```sh
./bin/mmf_smoke_test audio-read 1
./bin/mmf_smoke_test audio-read 2
./bin/mmf_smoke_test audio-control
./bin/mmf_smoke_test audio-codec
./bin/mmf_smoke_test audio-3a
./bin/mmf_smoke_test audio-loopback 1 100 1 16
./bin/mmf_audio_full_duplex_app 30 16000 1 16 1 1 1
```

覆盖接口：

```text
mmf_audio_get_default_input_config
mmf_audio_get_default_output_config
mmf_audio_input_open
mmf_audio_input_close
mmf_audio_input_read
mmf_audio_input_release
mmf_audio_input_get_status
mmf_audio_input_set_3a
mmf_audio_input_get_3a
mmf_audio_input_set_volume
mmf_audio_input_get_volume
mmf_audio_output_open
mmf_audio_output_close
mmf_audio_output_write
mmf_audio_output_drain
mmf_audio_output_get_status
mmf_audio_output_set_volume
mmf_audio_output_get_volume
mmf_audio_3a_get_default_config
mmf_audio_3a_set/get_aec
mmf_audio_3a_set/get_ns
mmf_audio_3a_set/get_agc
mmf_audio_3a_set/get_aec_level
mmf_audio_3a_set/get_aec_delay
mmf_audio_3a_set/get_ns_level
mmf_audio_3a_set/get_agc_target
mmf_audio_3a_set/get_agc_max_gain
mmf_audio_3a_set/get_agc_compress
mmf_audio_3a_get_status
mmf_audio_encoder_open
mmf_audio_encoder_close
mmf_audio_encoder_encode
mmf_audio_encoder_release
```

部分覆盖接口：

```text
mmf_audio_decoder_open
mmf_audio_decoder_close
mmf_audio_decoder_decode
mmf_audio_decoder_release
```

结论：录音、播放、双声道读取、loopback、音量、3A 参数 set/get、AEC reference、AENC 已覆盖。3A 效果测试需要做开关对比：同样音量下 `enable_3a=1` 应比 `enable_3a=0` 回声/滋滋声更小。ADEC 还缺专项测试；如果近期没有“下发编码音频再解码播放”的业务，可以标为下一阶段，不建议现在删。

### Touch

```sh
./bin/mmf_smoke_test touch-status
```

覆盖接口：

```text
mmf_touch_get_default_config
mmf_touch_open
mmf_touch_close
mmf_touch_get_status
mmf_touch_read_event
```

结论：touch 当前后置，默认包不再安装 `mmf_touch_read` 独立样例；`touch-status` 只用于确认接口占位状态。后续实现 `/dev/touchscreen` 后，再补真实触摸事件测试，验证 `x/y/down/up`。

## 17. 接口去留建议

建议保留：

```text
camera get/put/snapshot/list/status/ISP set/get
display bind/unbind/show_frame/show_image/snapshot/clear
jpg one-shot + session encoder/decoder
http open/start/stop/status/publish
audio input/output/session 3A/volume/codec
system init/status/scale
touch open/read/status
```

原因：这些接口对应主流音视频 SDK 使用方式，后续抽离 minimal 时仍然成立。

可以暂缓或后续收敛：

```text
mmf_display_set_window
mmf_jpg_decoder_* session 接口
mmf_audio_decoder_* session 接口
touch 模块
```

说明：这些不是“无用”，而是当前测试覆盖和业务优先级低。建议先不要删除，先补专项测试；如果两轮版本后仍没有业务使用，再考虑从 public API 移到 experimental/internal。
