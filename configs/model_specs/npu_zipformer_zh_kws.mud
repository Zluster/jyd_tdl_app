[basic]
type = kws_bundle
model = ../../models/cv184x/kws_encoder_bf16_cv184x.bmodel

[extra]
runtime = bmrt
task = streaming_keyword_spotting
encoder_model = ../../models/cv184x/kws_encoder_bf16_cv184x.bmodel
decoder_model = ../../models/cv184x/decoder-epoch-12-avg-2-chunk-16-left-64.onnx
joiner_model = ../../models/cv184x/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx
tokens = ../../models/cv184x/kws_tokens.txt
sample_rate = 16000
feature = kaldi_fbank_80_high_freq_minus_400
encoder_input_frames = 45
decode_chunk_frames = 32
