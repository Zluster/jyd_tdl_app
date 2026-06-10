[basic]
type = cvimodel
model = ../../models/cv184x/yolov8n_det_face_head_person_pet_384_640_INT8_cv184x.bmodel

[extra]
runtime = yolov8
task = detect
model_type = YOLOV8N_DET_FACE_HEAD_PERSON_PET
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
labels = face,head,person,pet
