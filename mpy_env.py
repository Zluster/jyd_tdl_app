import lvgl as lv
import sys
sys.path.append('/root/mpy')
import fs_driver

lv.init()
fs_drv = lv.fs_drv_t()
fs_driver.fs_register(fs_drv, 'L')

import drive
import ujson as json

lv.screen_active().set_style_bg_opa(0,lv.STATE.DEFAULT)
lv.layer_bottom().set_style_bg_opa(0,lv.STATE.DEFAULT)

import mpy_home
ms = mpy_home.MainScreen(page=lv.screen_active())

# btn = lv.button(lv.screen_active())
# btn.set_size(100, 50)
# btn.set_pos(100, 100)
# label = lv.label(btn)
# label.set_text("Hello")