[basic]
type = cvimodel
model = ../../models/cv184x/replace_with_plate_recognizer.bmodel

[extra]
runtime = plate_recognizer
task = ocr
model_type = PLATE_RECOGNIZER
input_type = bgr
mean = 127.5,127.5,127.5
scale = 0.0078125,0.0078125,0.0078125
