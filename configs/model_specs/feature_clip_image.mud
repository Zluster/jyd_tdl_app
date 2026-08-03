[basic]
type = bmodel
model = ../../models/cv184x/feature_clip_image_224_224_W4BF16_cv184x.bmodel

[extra]
runtime = feature
task = feature
model_type = FEATURE_CLIP_IMAGE
input_type = rgb
preprocess = resize
mean = 0,0,0
scale = 1,1,1
normalize = l2
