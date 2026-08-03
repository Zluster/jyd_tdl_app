[basic]
type = bmodel
model = ../../models/cv184x/yolov8n_det_hand_384_640_INT8_cv184x.bmodel

[extra]
runtime = yolov8
task = detect
model_type = YOLOV8_DET_HAND
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
labels = hand
