[basic]
type = cvimodel
model = ../../models/cv184x/scrfd_det_face_432_768_INT8_cv184x.bmodel

[extra]
runtime = scrfd
task = face-detect
model_type = SCRFD_DET_FACE
input_type = rgb
mean = 127.5,127.5,127.5
scale = 0.0078125,0.0078125,0.0078125
