[basic]
type = bmodel
model = ../../models/cv184x/keypoint_hand_128_128_INT8_cv184x.bmodel

[extra]
runtime = keypoint
task = keypoint
model_type = KEYPOINT_HAND
input_type = rgb
mean = 123.675,116.28,103.53
scale = 0.0171247538,0.0175070028,0.0174291939
