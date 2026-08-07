[basic]
type = asr_bundle
model = ../../models/cv184x/asr_encoder_chunk39_bf16_cv184x.bmodel

[extra]
runtime = bmrt
task = streaming_asr
encoder_model = ../../models/cv184x/asr_encoder_chunk39_bf16_cv184x.bmodel
decoder_model = ../../models/cv184x/asr39_decoder_bf16_cv184x.bmodel
joiner_model = ../../models/cv184x/asr39_joiner_bf16_cv184x.bmodel
tokens = ../../models/cv184x/asr39_tokens.txt
sample_rate = 16000
feature = kaldi_fbank_80_high_freq_minus_400
chunk_frames = 39
frame_shift = 32
