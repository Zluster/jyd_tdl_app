import lvgl as lv
from .mpy_display import Display
display = Display(720, 480)


from .mpy_mouse import mouse_indev
mouse = mouse_indev(lv.layer_sys())
