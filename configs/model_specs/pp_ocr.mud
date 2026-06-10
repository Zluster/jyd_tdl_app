[basic]
type = bmodel
model = ../../models/cv184x/ch_PP_OCRv3_det_int8_sym.bmodel

[extra]
runtime = pp_ocr
task = ocr
model_type = PP_OCR
input_type = bgr
det = true
mean = 123.675,116.28,103.53
scale = 0.01712475,0.017507,0.01742919
rec_model = ../../models/cv184x/ch_PP_OCRv4_rec_int8_sym.bmodel
rec_mean = 127.5,127.5,127.5
rec_scale = 0.00784313725490196,0.00784313725490196,0.00784313725490196
labels = ../ppocr_keys_v1.txt
det_thresh = 0.3
det_box_thresh = 0.6
det_min_size = 3
