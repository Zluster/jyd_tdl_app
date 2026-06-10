[basic]
type = cvimodel
model = ../../models/cv184x/replace_with_face_dense_keypoint.bmodel

[extra]
runtime = face_dense_landmark
task = landmark
model_type = FACE_LANDMARKS_468
input_type = bgr
input_channel = nhwc
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
