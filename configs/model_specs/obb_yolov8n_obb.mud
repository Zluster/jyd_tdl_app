[basic]
type = bmodel
model = ../../models/cv184x/obb_yolov8n_obb_cv184x_int8_sym.bmodel

[extra]
runtime = yolov8
task = detect
model_type = YOLOV8_OBB
type = obb
input_type = rgb
mean = 0, 0, 0
scale = 0.00392156862745098, 0.00392156862745098, 0.00392156862745098
labels = plane,ship,storage tank,baseball diamond,tennis court,basketball court,ground track field,harbor,bridge,large vehicle,small vehicle,helicopter,roundabout,soccer ball field,swimming pool
