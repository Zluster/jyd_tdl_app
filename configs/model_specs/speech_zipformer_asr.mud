[basic]
type = asr_bundle
model = ../../models/cv184x/recognition_speech_zipformer_encoder-s_71_80_BF16.bmodel

[extra]
runtime = speech_recognition
task = asr
model_type = RECOGNITION_SPEECH_ZIPFORMER_ENCODER
encoder_model = speech_zipformer_encoder.mud
decoder_model = speech_zipformer_decoder.mud
joiner_model = speech_zipformer_joiner.mud
tokens = tokens.txt

