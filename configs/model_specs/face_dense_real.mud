[basic]
type = cvimodel
model = ../../models/cv184x/face_landmarks_int8_sym.bmodel

[extra]
runtime = face_dense_landmark
task = landmark
model_type = FACE_LANDMARKS_DENSE
input_type = bgr
input_channel = nhwc
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
