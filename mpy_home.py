import lvgl as lv
import time
import os
import mpy_embed
# import os_ffi

font_cn = lv.binfont_create('L:./res/font/siyun_blod.bin')
font_symbol = lv.binfont_create('L:./res/font/symbol.bin')

dip = lv.display_get_default()
theme = lv.theme_default_init(dip, lv.palette_main(lv.PALETTE.BLUE), lv.palette_main(lv.PALETTE.RED), True, font_cn)
dip.set_theme(theme)


def print_obj(obj):
    print(obj, type(obj), dir(obj))

class AppTypeFlags:
    """Application type and classification flags."""
    
    # Main type flags (occupy bits 0-5)
    TYPE_NONE = 0
    TYPE_SETTINGS = 1 << 0      # 1,  0b00000001
    TYPE_SYSTEM_AI = 1 << 1     # 2,  0b00000010
    TYPE_TEACHING_AI = 1 << 2   # 4,  0b00000100
    TYPE_MY_APPLICATIONS = 1 << 3 # 8,  0b00001000
    TYPE_MATERIAL_MANAGER = 1 << 4 # 16, 0b00010000
    TYPE_FILE_MANAGER = 1 << 5   # 32, 0b00100000
    
    # Sub-type classification flags (bits 0-11)
    # Note: When combining with type flags, shift these left by 8+ bits
    SUB_TYPE_NONE = 0
    SUB_TYPE_CLASS_1 = 1 << 0   # 1,    0b0000000000000001
    SUB_TYPE_CLASS_2 = 1 << 1   # 2,    0b0000000000000010
    SUB_TYPE_CLASS_3 = 1 << 2   # 4,    0b0000000000000100
    SUB_TYPE_CLASS_4 = 1 << 3   # 8,    0b0000000000001000
    SUB_TYPE_CLASS_5 = 1 << 4   # 16,   0b0000000000010000
    SUB_TYPE_CLASS_6 = 1 << 5   # 32,   0b0000000000100000
    SUB_TYPE_CLASS_7 = 1 << 6   # 64,   0b0000000001000000
    SUB_TYPE_CLASS_8 = 1 << 7   # 128,  0b0000000010000000
    SUB_TYPE_CLASS_9 = 1 << 8   # 256,  0b0000000100000000
    SUB_TYPE_CLASS_10 = 1 << 9  # 512,  0b0000001000000000
    SUB_TYPE_CLASS_11 = 1 << 10 # 1024, 0b0000010000000000
    SUB_TYPE_CLASS_12 = 1 << 11 # 2048, 0b0000100000000000

class AppInfo:
    """应用信息类"""
    def __init__(self, **kwargs):
        self.id: str = kwargs.get('id', '')
        self.name: str = kwargs.get('name', '')
        self.icon: str = kwargs.get('icon', '')
        self.version = kwargs.get('version', '0,0,0')
        self.exec: str = kwargs.get('exec', '')
        self.author: str = kwargs.get('author', '')
        self.desc: str = kwargs.get('desc', '')
        self.names = kwargs.get('names', {})
        self.descs = kwargs.get('descs', {})
        self.type: int = kwargs.get('type', None)
        self.subtype: int = kwargs.get('subtype', None)

    def __str__(self):
        """用户友好的简洁输出"""
        return f"AppInfo(id='{self.id}', name='{self.name}', version={self.version})"
    
    def __repr__(self):
        """开发者调试的详细输出"""
        return (
            f"AppInfo(\n"
            f"  id={self.id!r},\n"
            f"  name={self.name!r},\n"
            f"  icon={self.icon!r},\n"
            f"  version={self.version!r},\n"
            f"  exec={self.exec!r},\n"
            f"  author={self.author!r},\n"
            f"  desc={self.desc!r},\n"
            f"  names={self.names!r},\n"
            f"  descs={self.descs!r},\n"
            f"  type={self.type!r},\n"
            f"  subtype={self.subtype!r}\n"
            f")"
        )

def _parse_int_str(value: str) -> int:
    """解析支持二进制/十六进制/十进制的字符串"""
    if not value:
        return 0
    value = value.strip()
    if value.startswith("0b"):
        return int(value[2:], 2)
    elif value.startswith("0x"):
        return int(value[2:], 16)
    return int(value)

def _parse_ini_file(filepath: str):
    """
    轻量级 INI 文件解析器
    返回: {section: {key: value}} 结构
    """
    result = {}
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            current_section = None
            
            for line in f:
                line = line.strip()
                
                # 跳过空行和注释
                if not line or line.startswith('#') or line.startswith(';'):
                    continue
                
                # 解析节 [section]
                if line.startswith('[') and line.endswith(']'):
                    current_section = line[1:-1].strip()
                    result[current_section] = {}
                    continue
                
                # 解析键值对 key = value
                if '=' in line and current_section:
                    key, value = line.split('=', 1)
                    key = key.strip()
                    value = value.strip()
                    
                    # 支持多行值（以 '\' 结尾）
                    while value.endswith('\\'):
                        value = value[:-1]
                        next_line = f.readline().strip()
                        if next_line:
                            value += next_line
                    
                    result[current_section][key] = value
    
    except Exception as e:
        print(f"Error reading INI file {filepath}: {e}")
        return {}
    
    return result

def _get_locale_fields(section_data):
    """获取多语言名称和描述"""
    names, descs = {}, {}
    for key, value in section_data.items():
        if key.startswith("name["):
            lang = key[5:-1]  # 提取括号内的语言代码
            names[lang] = value
        elif key.startswith("desc["):
            lang = key[5:-1]
            descs[lang] = value
    return names, descs

def get_apps_info(ignore_launcher: bool = False, ignore_app_store: bool = True):
    """
    从 /maixapp/apps/app.info 读取应用信息
    使用 MicroPython 原生方式解析 INI 文件
    """
    apps_info = []
    ini_path = "/root/maixapp/apps/app.info"
    
    # 检查文件是否存在
    # if not os_ffi.access(ini_path, os_ffi.F_OK):
    #     print(f"Error: {ini_path} not found")
    #     return apps_info
    
    # 手动解析 INI 文件
    ini_data = _parse_ini_file(ini_path)
    
    # 验证版本信息
    if "basic" not in ini_data:
        print("Error: 'basic' section not found in app.info")
        return apps_info
    
    try:
        version = int(ini_data["basic"].get("version", "0"))
        print(f"App info version: {version}")
    except:
        print("Error: Invalid version in app.info")
        return apps_info
    
    # 删除 basic 节，只保留应用信息
    del ini_data["basic"]
    
    # 遍历所有应用
    for app_id in ini_data.keys():
        if not app_id:
            continue
        
        # 跳过指定应用
        if ignore_launcher and app_id == "launcher":
            continue
        if ignore_app_store and app_id == "app_store":
            continue
        
        section = ini_data[app_id]
        
        try:
            # 获取必需字段
            app_name = section.get("name")
            app_icon = section.get("icon")
            app_exec = section.get("exec")
            app_version_str = section.get("version")
            
            if not all([app_name, app_icon, app_exec, app_version_str]):
                print(f"Warning: App {app_id} missing required fields")
                continue
            
            # 获取可选字段
            app_author = section.get("author", "")
            app_desc = section.get("desc", "")
            
            # 解析类型和子类型
            app_type = _parse_int_str(section.get("type", "0"))
            app_subtype = _parse_int_str(section.get("subtype", "0"))
            
            # 处理图标路径
            if not app_icon.startswith('/'):
                app_icon = f"/root/maixapp/apps/{app_id}/{app_icon}"
            
            # 检查图标文件
            # if not os_ffi.access(app_icon, os_ffi.F_OK):
            # app_icon = "/root/maixapp/share/icon/icon.json"
            
            # 获取多语言字段
            names, descs = _get_locale_fields(section)
            
            # 创建 AppInfo 对象
            info = AppInfo(
                id=app_id,
                name=app_name,
                icon=app_icon,
                version=str(app_version_str),
                exec=app_exec,
                author=app_author,
                desc=app_desc,
                names=names,
                descs=descs,
                type=app_type,
                subtype=app_subtype
            )
            
            apps_info.append(info)
            
        except Exception as e:
            print(f"Error parsing app '{app_id}': {e}")
            continue
    
    print(f"Successfully loaded {len(apps_info)} apps")
    return apps_info

class AppManager:
    """单例管理器"""
    _instance = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance.apps = []
        return cls._instance
    
    @classmethod
    def get_instance(cls):
        return cls()
    
    def load_from_file(self):
        """从文件加载应用列表"""
        self.apps = get_apps_info(ignore_launcher=False, ignore_app_store=True)
    
    def get_apps(self):
        return self.apps

class AppTile:
    def __init__(self, parent, app_info, click_callback=None):
        self.app = app_info
        self.callback = click_callback

        self.w = 80
        self.h = 80
        
        # 创建容器
        self.container = lv.obj(parent)
        self.container.set_size(self.w, self.h)
        self.container.set_style_bg_opa(lv.OPA.TRANSP, 0)
        self.container.set_style_pad_all(0, 0)
        self.container.set_style_pad_row(0, 0)
        self.container.set_style_pad_column(0, 0)
        
        # 图标处理（支持Lottie动画或静态图片）
        if ".json" in app_info.icon:
            self.lottie = lv.lottie(self.container)
            self.lottie.set_size(self.w, self.h)
            self.buf = bytearray(self.w * self.h * 4)
            self.lottie.set_buffer(self.w, self.h, self.buf)
            self.lottie.set_src_file(app_info.icon)
            self.lottie.align(lv.ALIGN.TOP_MID, 0, 44)
            self.icon_obj = self.lottie

            self.anim = self.lottie.get_anim()
            duration = self.anim.get_time()
            self.anim.set_duration(int(duration*3))
            pass
            
            # with open(app_info.icon, "r") as f:
            #     tmp = f.read()
            # self.anim.set_src_data(tmp, len(tmp))
            
        else:
            full_path = "L:" + app_info.icon
            # try:
            self.img = lv.image(self.container)
            self.img.set_src(full_path)
            self.img.align(lv.ALIGN.TOP_MID, 0, 44)
            self.img.set_size(self.w, self.h)
            self.icon_obj = self.img
            # except Exception as e:
            #     print(f"Icon not found: {full_path}")
        
        # 应用名称标签：与 tile 同宽 + 文本居中，与图标同轴。
        # 不用 align_to(图标)：相对兄弟节点的对齐不会随容器重建自动
        # 追踪，构造期（tile 还是 80 宽）算出的 x 会一直偏左
        self.label = lv.label(self.container)
        zh_name = self.app.names["zh"]
        self.label.set_text(zh_name)
        self.label.set_width(lv.pct(100))
        self.label.align(lv.ALIGN.TOP_MID, 0, 130)   # 图标 44+80 下方 6px
        self.label.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        self.label.set_style_pad_all(0, 0)
        
        # 添加点击事件
        self.container.add_event_cb(self.on_click, lv.EVENT.CLICKED, None)
    
    def on_click(self, e):
        """处理点击事件"""
        if self.callback:
            self.callback(self.app)
    
    def get_container(self):
        return self.container

    def pause_anim(self):
        """滚动期间暂停 lottie 播放（定格在当前帧，恢复时不跳变）"""
        if ".json" in self.app.icon:
            self.anim.pause()

    def resume_anim(self):
        """滚动结束从暂停帧继续播放"""
        if ".json" in self.app.icon:
            self.anim.resume()

# 图标定义
MY_MATERIAL_ICON = "\ue600"
MY_SETTING_ICON = "\ue68f"
MY_FILE_ICON = "\ue631"
MY_APP_ICON = "\ueb88"
MY_LEARN_ICON = "\ue7f1"
MY_HOME_ICON = "\ue7e4"
MY_BATTERY_ICON = "\ue60f"
MY_SYSTEM_ICON = "\ue658"
MY_ARROW_ICON = "\ue8f1"
MY_SIDER_ICON_1 = "\ue603"
MY_SIDER_ICON_2 = "\ue601"

TOP_LEVEL_BUTTONS = 7
SECONDARY_BUTTONS = 12
TILES_PER_PAGE = 6   # 720x480：3 列 x 2 行
_PAGE_TURN_PX = 48   # 松手翻页阈值：偏移超过它才翻页，否则弹回

import sys
class MainScreen:
    def __init__(self, page):
        # 状态管理
        self.is_sidebar_collapsed = False
        self.is_secondary_visible = False
        self.current_page = 0
        self.total_pages = 0
        self.current_app_list = []
        
        # UI对象引用
        self.main_container = page
        self.sidebar = None
        self.status_bar = None
        self.app_display_area = None
        self.app_grid_container = None
        self.page_indicator = None
        self.fold_btn = None
        self.time_label = None
        self.battery_label = None
        self.battery_container = None
        self.wifi_icon = None
        self.last_button = None
        
        # 按钮数组
        self.other_buttons = [None] * TOP_LEVEL_BUTTONS
        self.secondary_buttons = [None] * SECONDARY_BUTTONS


        # self.handler = dynfunc.get("home_handler")
        
        AppManager().load_from_file()
        self.tiles = []
        self.app_category_map = {}
        self.init_app_categories()
        self.create_main_container()
        self.create_fold_button()
        self.create_status_bar()
        self.create_sidebar()
        self.create_app_display_area()
        self.create_page_indicator()
        self.update_app_display("首页")
    
    def init_app_categories(self):
        """初始化应用分类映射"""
        original_apps = AppManager().get_apps()
        self.app_category_map["首页"] = original_apps

        # return
        # 分类容器
        self.app_category_map["系统应用"] = []
        self.app_category_map["系统AI"] = []
        self.app_category_map["教学AI"] = []
        self.app_category_map["素材管理"] = []
        self.app_category_map["文件管理"] = []
        self.app_category_map["我的应用"] = []
        self.app_category_map["1 人工智能的应用"] = []
        self.app_category_map["2 python 人工智能学习"] = []
        self.app_category_map["3 机器学习基础"] = []
        self.app_category_map["4 图像识别"] = []
        self.app_category_map["5 人脸识别"] = []
        self.app_category_map["6 语音交互"] = []
        self.app_category_map["7 自然语言处理"] = []
        self.app_category_map["8 目标检测"] = []
        self.app_category_map["9 预训练模型与数据标注"] = []
        self.app_category_map["10 基于大语言模型"] = []
        self.app_category_map["11 多模态应用实践"] = []
        self.app_category_map["12 智能体自主设计"] = []

        for app in original_apps:
            if app.type & AppTypeFlags.TYPE_SETTINGS:
                self.app_category_map["系统应用"].append(app)
            elif app.type & AppTypeFlags.TYPE_SYSTEM_AI:
                self.app_category_map["系统AI"].append(app)
            elif app.type & AppTypeFlags.TYPE_TEACHING_AI:
                self.app_category_map["教学AI"].append(app)
            elif app.type & AppTypeFlags.TYPE_MATERIAL_MANAGER:
                self.app_category_map["素材管理"].append(app)
            elif app.type & AppTypeFlags.TYPE_FILE_MANAGER:
                self.app_category_map["文件管理"].append(app)
            elif app.type == AppTypeFlags.TYPE_NONE:
                self.app_category_map["我的应用"].append(app)
            if app.subtype & AppTypeFlags.SUB_TYPE_CLASS_1:
                self.app_category_map["1 人工智能的应用"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_2:
                self.app_category_map["2 python 人工智能学习"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_3:
                self.app_category_map["3 机器学习基础"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_4:
                self.app_category_map["4 图像识别"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_5:
                self.app_category_map["5 人脸识别"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_6:
                self.app_category_map["6 语音交互"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_7:
                self.app_category_map["7 自然语言处理"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_8:
                self.app_category_map["8 目标检测"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_9:
                self.app_category_map["9 预训练模型与数据标注"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_10:
                self.app_category_map["10 基于大语言模型"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_11:
                self.app_category_map["11 多模态应用实践"].append(app)
            elif app.subtype & AppTypeFlags.SUB_TYPE_CLASS_12:
                self.app_category_map["12 智能体自主设计"].append(app)

    
    def create_main_container(self):
        """创建主容器"""
        self.main_container.set_style_radius(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.main_container.set_scrollbar_mode(lv.SCROLLBAR_MODE.OFF)
        self.main_container.set_scroll_dir(lv.DIR.NONE)
        
        # 移除边框和阴影
        self.main_container.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.main_container.set_style_shadow_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.main_container.set_style_pad_all(0, 0)
        
        # 深蓝色背景
        self.main_container.set_style_bg_color(lv.color_hex(0x020B2C), lv.PART.MAIN | lv.STATE.DEFAULT)
        self.main_container.set_style_bg_opa(lv.OPA.COVER, lv.PART.MAIN | lv.STATE.DEFAULT)
    
    def create_fold_button(self):
        """创建折叠按钮"""
        self.fold_btn = lv.obj(self.main_container)
        self.fold_btn.set_style_pad_all(0, 0)
        self.fold_btn.set_style_bg_opa(lv.OPA.TRANSP, 0)
        self.fold_btn.set_size(56, 56)
        self.fold_btn.set_pos(8, 416)
        self.fold_btn.set_style_radius(20, 0)
        self.fold_btn.set_style_shadow_width(0, 0)
        self.fold_btn.set_style_border_width(0, 0)
        
        # 图标
        icon = lv.label(self.fold_btn)
        icon.set_text(MY_SIDER_ICON_1)
        icon.set_style_text_color(lv.color_white(), 0)
        icon.set_style_text_font(font_symbol, 0)
        icon.set_style_transform_scale(500, 0)  # 缩放
        icon.set_style_pad_all(0, 0)
        icon.align(lv.ALIGN.CENTER, 0, 0)
        
        self.fold_btn.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self.fold_btn.add_event_cb(lambda e: self.toggle_sidebar(), lv.EVENT.CLICKED, None)
    
    def toggle_sidebar(self):
        """切换侧边栏显示状态"""
        icon = self.fold_btn.get_child_by_type(0, lv.label_class)
        if not icon:
            print("Icon not found")
            return
        
        if self.is_sidebar_collapsed:
            self.sidebar.remove_flag(lv.obj.FLAG.HIDDEN)
            self.app_display_area.set_x(192)
            icon.set_text(MY_SIDER_ICON_1)
        else:
            self.sidebar.add_flag(lv.obj.FLAG.HIDDEN)
            # 侧栏收起后显示区在整屏居中（不扩宽，宽度仍 512）
            self.app_display_area.set_x((720 - 512) // 2)
            icon.set_text(MY_SIDER_ICON_2)

        self.is_sidebar_collapsed = not self.is_sidebar_collapsed
        self.page_indicator.align_to(self.app_grid_container, lv.ALIGN.OUT_BOTTOM_MID, 0, 12)
    
    def create_status_bar(self):
        """创建状态栏"""
        self.status_bar = lv.obj(self.main_container)
        self.status_bar.set_size(lv.pct(100), 36)
        self.status_bar.align(lv.ALIGN.TOP_MID, 0, 0)
        self.status_bar.set_style_bg_color(lv.color_black(), 0)
        self.status_bar.set_style_bg_opa(lv.OPA.COVER, 0)
        self.status_bar.set_style_pad_hor(16, 0)
        self.status_bar.set_style_pad_ver(4, 0)
        self.status_bar.set_flex_flow(lv.FLEX_FLOW.ROW)
        self.status_bar.set_flex_align(
            lv.FLEX_ALIGN.SPACE_BETWEEN,
            lv.FLEX_ALIGN.CENTER,
            lv.FLEX_ALIGN.CENTER
        )
        self.status_bar.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self.status_bar.set_scroll_dir(lv.DIR.NONE)
        
        # 移除边框
        self.status_bar.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.status_bar.set_style_shadow_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        
        # 时间标签
        self.time_label = lv.label(self.status_bar)
        self.time_label.set_text("00:00 00-00 周三")
        self.time_label.set_style_text_color(lv.color_white(), 0)
        
        # 电池容器
        self.battery_container = lv.obj(self.status_bar)
        self.battery_container.remove_style_all()
        self.battery_container.set_flex_flow(lv.FLEX_FLOW.ROW)
        self.battery_container.set_flex_align(
            lv.FLEX_ALIGN.CENTER,
            lv.FLEX_ALIGN.CENTER,
            lv.FLEX_ALIGN.CENTER
        )
        
        # 电池图标
        battery_icon = lv.label(self.battery_container)
        battery_icon.set_text(lv.SYMBOL.BATTERY_FULL)
        battery_icon.set_style_text_color(lv.color_white(), 0)
        battery_icon.set_style_text_font(lv.font_montserrat_20, 0)
        
        # 电量百分比
        self.battery_label = lv.label(self.battery_container)
        self.battery_label.set_text("100%")
        self.battery_label.set_style_text_color(lv.color_white(), 0)
        self.battery_label.set_style_text_font(lv.font_montserrat_20, 0)
        self.battery_label.set_style_pad_left(8, 0)
        
        # WiFi图标
        self.wifi_icon = lv.label(self.battery_container)
        self.wifi_icon.set_text(lv.SYMBOL.WIFI)
        self.wifi_icon.set_style_text_font(lv.font_montserrat_14, 0)
        self.wifi_icon.set_style_text_color(lv.color_white(), 0)
        self.wifi_icon.set_style_pad_left(20, 0)
        
        self.battery_container.align(lv.ALIGN.TOP_RIGHT, -10, 5)
        
        # 定时更新
        # lv.timer_create(lambda t: self.update_status(), 2000, None)
    
    def get_battery_status(self):
        """获取电池状态"""
        return {"percent": 100, "is_charging": False}
    
    def update_status(self):
        """更新状态栏信息"""
        # 更新时间
        now = time.localtime()
        days = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"]
        time_str = f"{now[3]:02d}:{now[4]:02d} {now[1]:02d}-{now[2]:02d} {days[(now[6] + 1) % 7]}"
        self.time_label.set_text(time_str)
        
        # 更新电量
        battery = self.get_battery_status()
        self.battery_label.set_text(f"{battery['percent']}%")
        
        # 更新电池图标
        battery_icon = self.battery_container.get_child(0)
        if battery["is_charging"]:
            icon = f"{lv.SYMBOL.CHARGE} {lv.SYMBOL.BATTERY_FULL}"
            color = lv.color_hex(0x00FF00)
        else:
            percent = battery["percent"]
            if percent > 90:
                icon = lv.SYMBOL.BATTERY_FULL
            elif percent > 60:
                icon = lv.SYMBOL.BATTERY_3
            elif percent > 30:
                icon = lv.SYMBOL.BATTERY_2
            elif percent > 10:
                icon = lv.SYMBOL.BATTERY_1
            else:
                icon = lv.SYMBOL.BATTERY_EMPTY
            color = lv.color_white()
        
        battery_icon.set_text(icon)
        battery_icon.set_style_text_color(color, 0)
    
    def create_sidebar(self):
        """创建侧边栏"""
        self.sidebar = lv.obj(self.main_container)
        self.sidebar.set_width(176)
        self.sidebar.set_pos(8, 44)
        self.sidebar.set_height(364)
        self.sidebar.set_style_bg_color(lv.color_hex(0x020B2C), 0)
        self.sidebar.set_style_pad_all(8, 0)
        self.sidebar.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.sidebar.set_style_shadow_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        
        # 一级菜单项
        items = [
            ("首页", MY_HOME_ICON),
            ("系统应用", MY_SYSTEM_ICON),
            ("系统AI", MY_SYSTEM_ICON),
            ("教学AI", MY_LEARN_ICON),
            ("我的应用", MY_APP_ICON),
            ("素材管理", MY_MATERIAL_ICON),
            ("文件管理", MY_FILE_ICON)
        ]
        
        for i, (name, symbol) in enumerate(items):
            btn = lv.button(self.sidebar)
            btn.set_height(44)
            btn.set_width(lv.pct(92))
            btn.set_style_pad_all(0, 0)
            btn.set_style_bg_color(lv.color_hex(0x0A163F), lv.PART.MAIN | lv.STATE.DEFAULT)
            btn.align(lv.ALIGN.TOP_LEFT, 0, i * 48)
            
            # 图标
            sym_label = lv.label(btn)
            sym_label.set_text(symbol)
            sym_label.set_style_text_font(font_symbol, 0)
            sym_label.set_style_text_color(lv.color_white(), 0)
            sym_label.align(lv.ALIGN.LEFT_MID, 10, 0)
            
            # 文本
            txt_label = lv.label(btn)
            txt_label.set_text(name)
            txt_label.set_style_text_color(lv.color_white(), 0)
            txt_label.align(lv.ALIGN.LEFT_MID, 40, 0)
            
            # 箭头（教学AI）
            if i == 3:
                arrow = lv.label(btn)
                arrow.set_text(MY_ARROW_ICON)
                arrow.set_style_text_font(font_symbol, 0)
                arrow.set_style_text_color(lv.color_white(), 0)
                arrow.align(lv.ALIGN.RIGHT_MID, -10, 0)
            
            # btn.set_user_data(name)
            btn.add_event_cb(
                self.teaching_ai_handler if i == 3 else self.sidebar_handler,
                lv.EVENT.CLICKED,
                None
            )
            
            if i == 0:
                btn.set_style_bg_color(lv.color_hex(0x1E40AF), lv.PART.MAIN | lv.STATE.DEFAULT)
                self.last_button = btn
            
            if i > 0:
                self.other_buttons[i] = btn
        
        # 二级菜单（教学AI子项）
        sub_items = [
            "1 人工智能的应用", "2 python 人工智能学习", "3 机器学习基础",
            "4 图像识别", "5 人脸识别", "6 语音交互", "7 自然语言处理",
            "8 目标检测", "9 预训练模型与数据标注", "10 基于大语言模型",
            "11 多模态应用实践", "12 智能体自主设计"
        ]
        
        for i, sub_name in enumerate(sub_items):
            sub_btn = lv.button(self.sidebar)
            sub_btn.set_height(44)
            sub_btn.set_width(lv.pct(92))
            sub_btn.set_style_pad_all(0, 0)
            sub_btn.set_style_bg_color(lv.color_hex(0x0A163F), lv.PART.MAIN | lv.STATE.DEFAULT)
            sub_btn.align(lv.ALIGN.TOP_LEFT, 0, (3 + 1) * 48 + i * 48)
            sub_btn.add_flag(lv.obj.FLAG.HIDDEN)
            
            # 图标
            sub_sym = lv.label(sub_btn)
            sub_sym.set_text(MY_LEARN_ICON)
            sub_sym.set_style_text_font(font_symbol, 0)
            sub_sym.set_style_text_color(lv.color_white(), 0)
            sub_sym.align(lv.ALIGN.LEFT_MID, 10, 0)
            
            # 文本（滚动显示）
            sub_txt = lv.label(sub_btn)
            sub_txt.set_text(sub_name)
            sub_txt.set_style_text_color(lv.color_white(), 0)
            sub_txt.set_long_mode(lv.label.LONG_MODE.SCROLL_CIRCULAR)
            sub_txt.set_width(110)
            sub_txt.align(lv.ALIGN.LEFT_MID, 40, 0)
            
            # sub_btn.set_user_data(sub_name)
            sub_btn.add_event_cb(self.sidebar_handler, lv.EVENT.CLICKED, None)
            self.secondary_buttons[i] = sub_btn
    
    def teaching_ai_handler(self, e):
        """教学AI按钮事件：展开/收起二级菜单"""
        btn = e.get_target()
        self.is_secondary_visible = not self.is_secondary_visible
        
        print(f"TeachingAI toggled: {self.is_secondary_visible}")
        
        # 调整后续按钮位置
        offset = len(self.secondary_buttons) * 48 if self.is_secondary_visible else 0
        for i in range(4, TOP_LEVEL_BUTTONS):
            if self.other_buttons[i]:
                base_y = 4 * 48 + (i - 4) * 48
                self.other_buttons[i].set_pos(0, base_y + offset)

        # 显示/隐藏二级按钮
        for i, sub_btn in enumerate(self.secondary_buttons):
            if sub_btn:
                if self.is_secondary_visible:
                    sub_btn.set_pos(0, (4 * 48) + i * 48)
                    sub_btn.remove_flag(lv.obj.FLAG.HIDDEN)
                else:
                    sub_btn.add_flag(lv.obj.FLAG.HIDDEN)
    
    def sidebar_handler(self, e):
        """侧边栏通用点击处理"""
        btn = e.get_target_obj()
        
        # 恢复上一个按钮样式
        if self.last_button and self.last_button != btn:
            self.last_button.set_style_bg_color(
                lv.color_hex(0x0A163F),
                lv.PART.MAIN | lv.STATE.DEFAULT
            )
        
        # 高亮当前按钮
        btn.set_style_bg_color(lv.color_hex(0x1E40AF), lv.PART.MAIN | lv.STATE.DEFAULT)
        self.last_button = btn
        
        # 更新应用显示
        category = btn.get_child(1).get_text()
        self.update_app_display(category)
    
    def create_app_display_area(self):
        """创建应用显示区域"""
        self.app_display_area = lv.obj(self.main_container)
        self.app_display_area.set_size(512, 400)
        self.app_display_area.set_pos(192, 44)
        self.app_display_area.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self.app_display_area.set_style_bg_color(lv.color_hex(0x020B2C), 0)
        self.app_display_area.set_style_opa(lv.OPA._0, lv.PART.SCROLLBAR)
        self.app_display_area.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.app_display_area.set_style_pad_all(0, 0)
        
        # 网格容器
        self.app_grid_container = lv.obj(self.app_display_area)
        self.app_grid_container.set_size(lv.pct(100), lv.pct(100))
        self.app_grid_container.set_style_opa(lv.OPA._0, lv.PART.SCROLLBAR)
        self.app_grid_container.set_style_bg_color(lv.color_hex(0x020B2C), 0)
        self.app_grid_container.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
        self.app_grid_container.set_style_pad_all(0, 0)

        # 翻页事件只注册一次：create_app_grid() 每次切分类都会执行，
        # 在那里注册会导致 SCROLL_END 被重复处理、翻页动画互相打断
        self.app_grid_container.add_event_cb(self.scroll_handler, lv.EVENT.ALL, None)

        self.current_page = 0
        self.total_pages = 0
    
    def update_app_display(self, category):
        """根据分类更新应用显示"""
        self.app_grid_container.clean()
        print(f"Updating display for: {category}")
        
        if category not in self.app_category_map:
            print(f"Category not found: {category}")
            return
        
        self.current_app_list = self.app_category_map[category]
        self.total_pages = (len(self.current_app_list) + TILES_PER_PAGE - 1) // TILES_PER_PAGE
        self.current_page = 0
        
        self.create_app_grid()
        self.update_page_indicator()
    
    def create_app_grid(self):
        """创建应用网格布局"""
        self.tiles.clear()
        
        # 配置滚动和布局
        self.app_grid_container.set_layout(lv.LAYOUT.FLEX)
        self.app_grid_container.set_flex_flow(lv.FLEX_FLOW.ROW)
        # pad_all 不覆盖 pad_column/row；主题若给 flex 子项加间隙 g，
        # 第 i 页实际位于 i*(512+g)，按 512 翻页会逐页右移
        self.app_grid_container.set_style_pad_column(0, 0)
        self.app_grid_container.set_style_pad_row(0, 0)
        self.app_grid_container.set_scroll_dir(lv.DIR.HOR)
        self.app_grid_container.set_style_opa(lv.OPA.COVER, lv.PART.MAIN)
        # 分页器语义：关掉 LVGL 的惯性和吸附（它们按甩速飞多页），
        # 拖动 1:1 跟手，松手由 handle_scroll 按方向翻恰好一页
        self.app_grid_container.set_scroll_snap_x(lv.SCROLL_SNAP.NONE)
        self.app_grid_container.remove_flag(lv.obj.FLAG.SCROLL_MOMENTUM)
        self.app_grid_container.set_style_anim_duration(200, 0)
        self.app_grid_container.add_flag(lv.obj.FLAG.SCROLL_ELASTIC)
        self.app_grid_container.set_scrollbar_mode(lv.SCROLLBAR_MODE.OFF)
        self.app_grid_container.set_style_pad_all(0, 0)
        
        tile_width = 512 // 3    # 170
        tile_height = 400 // 2   # 200
        
        # 创建分页
        for page in range(self.total_pages):
            page_container = lv.obj(self.app_grid_container)
            page_container.set_size(512, 400)
            page_container.set_style_bg_opa(lv.OPA.TRANSP, 0)
            page_container.set_layout(lv.LAYOUT.FLEX)
            page_container.set_flex_flow(lv.FLEX_FLOW.ROW_WRAP)
            page_container.set_style_border_width(0, lv.PART.MAIN | lv.STATE.DEFAULT)
            page_container.set_style_pad_all(0, 0)
            page_container.set_style_pad_row(0, 0)
            page_container.set_style_pad_column(0, 0)
            page_container.remove_flag(lv.obj.FLAG.SCROLLABLE)
            
            start_idx = page * TILES_PER_PAGE
            end_idx = min(start_idx + TILES_PER_PAGE, len(self.current_app_list))
            # 添加应用图标
            for i in range(start_idx, end_idx):
                app_info = self.current_app_list[i]
                tile = AppTile(page_container, app_info, lambda app: self.launch_app(app))
                # tile = AppTile(page_container, app_info)
                
                container = tile.get_container()
                container.remove_style_all()
                container.set_size(tile_width, tile_height)
                container.set_style_pad_all(0, 0)
                container.remove_flag(lv.obj.FLAG.SCROLLABLE)
                container.add_flag(lv.obj.FLAG.CLICKABLE | lv.obj.FLAG.CLICK_FOCUSABLE)
                # container.set_user_data(app_info)
                container.add_event_cb(lambda e: self.handle_tile_event(e, app_info), lv.EVENT.ALL, None)
                
                self.tiles.append(tile)

            # 添加占位符
            #placeholders = TILES_PER_PAGE - (end_idx - start_idx)
            #for _ in range(placeholders):
            #    ph = lv.obj(page_container)
            #    ph.set_size(tile_width, tile_height)
            #    ph.remove_style_all()
            #    ph.set_style_bg_opa(lv.OPA.TRANSP, 0)
    
    def handle_tile_event(self, e, user_data):
        """处理应用图标触摸事件"""
        code = e.get_code()
        target = e.get_target()
        
        # 使用类属性存储临时状态
        if not hasattr(self, '_touch_state'):
            self._touch_state = {
                'press_time': 0,
                'pressed_obj': None,
                'is_long_press': False,
                'start_x': 0,
                'start_y': 0,
                'is_click_valid': True,
                'is_dragging': False,
                'is_vertical_drag': False
            }
        
        state = self._touch_state
        
        if code == lv.EVENT.PRESSED:
            state['press_time'] = time.ticks_ms()
            state['pressed_obj'] = target
            state['is_long_press'] = False
            state['is_dragging'] = False
            state['is_vertical_drag'] = False
            state['is_click_valid'] = True
            
            # get_indev() 只对输入类事件有效，必须在对应分支内调用
            indev = e.get_indev()
            if indev:
                point = lv.point_t()
                indev.get_point(point)
                state['start_x'] = point.x
                state['start_y'] = point.y
            
        elif code == lv.EVENT.PRESSING:
            indev = e.get_indev()
            if indev and state['pressed_obj'] == target:
                point = lv.point_t()
                indev.get_point(point)
                
                diff_x = point.x - state['start_x']
                diff_y = point.y - state['start_y']
                
                DRAG_THRESHOLD = 10
                if abs(diff_x) > DRAG_THRESHOLD or abs(diff_y) > DRAG_THRESHOLD:
                    state['is_dragging'] = True
                    if abs(diff_y) > abs(diff_x):
                        state['is_vertical_drag'] = True
                        state['is_click_valid'] = False
        
        elif code == lv.EVENT.LONG_PRESSED:
            if not state['is_vertical_drag']:
                state['is_long_press'] = True
        
        # elif code == lv.EVENT.SHORT_CLICKED:
        #     if state['is_click_valid'] and not state['is_dragging']:
        #         app_info = user_data
        #         if app_info:
        #             self.launch_app(app_info)
        
        elif code == lv.EVENT.RELEASED:
            state['pressed_obj'] = None
        
        elif code == lv.EVENT.CLICKED:
            # 重置状态
            state['is_dragging'] = False
            state['is_vertical_drag'] = False
            state['is_click_valid'] = True
    
    def launch_app(self, app_info):
        """启动应用"""
        try:
            # if os.path.exists("/tmp/run_app.txt"):
            #     os.remove("/tmp/run_app.txt")
            # self.handler(app_info.name)
            mpy_embed.on_app_launch("/root/maixapp/apps/" + app_info.id + "/" + app_info.exec)
            # print(app_info.id, app_info.name, app_info.exec)
            # app.switch_app(app_info["id"], -1, app_info["exec"])
            pass
        except Exception as e:
            print(f"Launch app failed: {e}")
    
    def scroll_handler(self, e):
        """滚动事件处理。滚动期间（拖动 + 松手归位动画）暂停 lottie 动画：
        每个播放中的图标每帧都要 thorvg 矢量重绘，是滑动卡顿的主要来源"""
        code = e.get_code()
        if code == lv.EVENT.SCROLL_BEGIN:
            for tile in self.tiles:
                tile.pause_anim()
            return
        if code in (lv.EVENT.SCROLL_END, lv.EVENT.SCROLL):
            self.handle_scroll(e)
        if code == lv.EVENT.SCROLL_END:
            # 放在 handle_scroll 之后：里面的保险对齐 scroll_to_x
            # 可能再触发一轮 SCROLL_BEGIN/END，避免刚恢复又被暂停
            for tile in self.tiles:
                tile.resume_anim()
    
    def handle_scroll(self, e):
        """翻页逻辑：惯性/吸附已关（滚动完全跟手），松手时按相对当前页
        的偏移方向翻恰好一页；偏移不足阈值（含单页/边界弹性）弹回原位"""
        if e.get_code() != lv.EVENT.SCROLL_END:
            return

        obj = e.get_target_obj()
        page_width = 512
        delta = obj.get_scroll_x() - self.current_page * page_width
        new_page = self.current_page
        if delta > _PAGE_TURN_PX:
            new_page = min(self.current_page + 1, self.total_pages - 1)
        elif delta < -_PAGE_TURN_PX:
            new_page = max(self.current_page - 1, 0)

        if new_page != self.current_page:
            print(f"Scroll to page: {new_page + 1}/{self.total_pages}")
            self.current_page = new_page
            self.update_page_indicator()
        # 归位动画到页边界；归位本身会再触发一轮 SCROLL 事件，
        # 结束时 delta=0，自然收敛不会再翻页
        obj.scroll_to_x(new_page * page_width, True)
    
    def create_page_indicator(self):
        """创建页面指示器"""
        self.page_indicator = lv.obj(self.main_container)
        self.page_indicator.set_size(120, 12)
        self.page_indicator.set_layout(lv.LAYOUT.FLEX)
        self.page_indicator.set_flex_flow(lv.FLEX_FLOW.ROW)
        self.page_indicator.set_flex_align(
            lv.FLEX_ALIGN.CENTER,
            lv.FLEX_ALIGN.CENTER,
            lv.FLEX_ALIGN.CENTER
        )
        self.page_indicator.set_style_border_width(0, 0)
        self.page_indicator.set_style_bg_opa(lv.OPA.TRANSP, 0)
        self.page_indicator.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self.page_indicator.remove_flag(lv.obj.FLAG.SCROLL_CHAIN_VER)
        
        self.page_indicator.align_to(self.app_grid_container, lv.ALIGN.OUT_BOTTOM_MID, 0, 12)
    
    def update_page_indicator(self):
        """更新页面指示器"""
        self.page_indicator.clean()
        
        for i in range(self.total_pages):
            dot = lv.obj(self.page_indicator)
            dot.set_size(12, 12)
            dot.set_style_radius(5, 0)
            
            if i == self.current_page:
                dot.set_style_bg_color(lv.color_hex(0xFFFFFF), 0)
            else:
                dot.set_style_bg_color(lv.color_hex(0x020B2C), 0)
                dot.set_style_border_width(1, 0)
                dot.set_style_border_color(lv.color_hex(0xFFFFFF), 0)
            
            if i < self.total_pages - 1:
                dot.set_style_pad_right(8, 0)
        
        print(f"Page: {self.current_page + 1}/{self.total_pages}")

