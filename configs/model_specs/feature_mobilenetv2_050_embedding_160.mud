[basic]
type = bmodel
model = ../../models/cv184x/mobilenetv2_050_embedding_160.INT8.cv184x.bmodel

[extra]
runtime = feature
task = feature
model_type = FEATURE_MOBILENETV2_050_EMBEDDING
input_type = rgb
preprocess = resize
mean = 123.675,116.28,103.53
scale = 0.017124582082033157,0.017507003620266914,0.01742919348180294
normalize = l2
