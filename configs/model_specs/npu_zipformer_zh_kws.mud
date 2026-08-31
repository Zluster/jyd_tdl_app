[basic]
type = kws_bundle
model = ../../../third_party/cv184x/models/cv184x/kws_encoder_bf16_cv184x.bmodel

[extra]
runtime = bmrt
task = streaming_keyword_spotting
encoder_model = ../../../third_party/cv184x/models/cv184x/kws_encoder_bf16_cv184x.bmodel
decoder_model = ../../../third_party/cv184x/models/cv184x/kws_decoder_bf16_cv184x.bmodel
joiner_model = ../../../third_party/cv184x/models/cv184x/kws_joiner_bf16_cv184x.bmodel
tokens = ../../../third_party/cv184x/models/cv184x/kws_tokens.txt
sample_rate = 16000
feature = kaldi_fbank_80_high_freq_minus_400
encoder_input_frames = 45
decode_chunk_frames = 32
