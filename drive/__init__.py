import lvgl as lv
from .mpy_display import Display
display = Display(1280, 720, False)


# from .mpy_mouse import mouse_indev
# mouse = mouse_indev(lv.layer_sys())


from .mpy_net_mouse import net_mouse_indev
net_mouse = net_mouse_indev(lv.layer_sys(), port=5555)