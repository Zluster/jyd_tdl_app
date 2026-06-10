[basic]
type = bmodel
model = ../../models/cv184x/roads_yolov8n_cv184x_int8_sym.bmodel

[extra]
model_type = YOLOV8
input_type = rgb
mean = 0, 0, 0
scale = 1, 1, 1
labels = load,unload,left,right,forward,stop
