# CV184X 中文文字转语音（Piper 中文前端 + 解码器）。
#
# 两个 bmodel 合计约 46 MB，不进版本库；把
#   piper_zh_tts_realdur32_192_bf16.bmodel
#   piper_zh_decoder_len192_v127_bf16.bmodel
# 放到下面引用的目录即可（板端通常放在 /root/models/，与 .mud 同目录）。
[basic]
type = tts_bundle
model = ../../../third_party/cv184x/models/cv184x/piper_zh_tts_realdur32_192_bf16.bmodel

[extra]
runtime = bmrt
task = text_to_speech
frontend_model = ../../../third_party/cv184x/models/cv184x/piper_zh_tts_realdur32_192_bf16.bmodel
decoder_model = ../../../third_party/cv184x/models/cv184x/piper_zh_decoder_len192_v127_bf16.bmodel
sample_rate = 22050
output_sample_rate = 16000
