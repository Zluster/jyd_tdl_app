[basic]
type = bmodel
model = ../../models/cv184x/your_feature.bmodel

[extra]
runtime = feature
task = feature
model_type = FEATURE_CUSTOM
input_type = rgb
preprocess = resize
mean = 0, 0, 0
scale = 0.00392156862745098, 0.00392156862745098, 0.00392156862745098
normalize = l2
