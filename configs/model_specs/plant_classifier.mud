[basic]
type = bmodel
model = ../../models/cv184x/plant_mobilenetv2_cv184x_int8_sym.bmodel

[extra]
runtime = classifier
task = classify
model_type = PLANT_CLASSIFIER
input_type = rgb
mean = 123.5,123.5,123.5
scale = 0.017124753831663668,0.017124753831663668,0.017124753831663668
apply_softmax = true
labels = yujinxiang,shuixianhua,luhui,juhua,jingmiancao
