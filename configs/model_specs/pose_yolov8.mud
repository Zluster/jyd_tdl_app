[basic]
type = bmodel
model = ../../models/cv184x/keypoint_yolov8pose_person17_384_640_INT8_cv184x.bmodel

[extra]
runtime = keypoint
task = keypoint
model_type = KEYPOINT_YOLOV8POSE
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
labels = dog
