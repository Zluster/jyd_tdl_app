[basic]
type = bmodel
model = ../../models/cv184x/campplus_sv_t300_bf16_cv184x.bmodel

[extra]
runtime = bmrt
task = speaker_recognition
input = float32[1,300,80]
output = float32[1,192]
sample_rate = 16000
feature = kaldi_fbank_80_global_mean
