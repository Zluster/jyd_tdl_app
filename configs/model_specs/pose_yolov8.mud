[basic]
type = bmodel
model = ../../models/cv184x/pose_yolov8n_pose_cv184x_int8_sym.bmodel

[extra]
runtime = keypoint
task = keypoint
model_type = KEYPOINT_YOLOV8POSE
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
labels = dog
