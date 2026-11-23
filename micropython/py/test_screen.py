# Modou 屏幕点亮测试（st7789 C 模块 + 原厂初始化序列）
#
# 使用步骤：
#   1. 把 modou/py/ 下的 aw9106.py 和本脚本拷到设备（或 SD 根目录）
#   2. REPL 运行：import test_screen
#   3. 预期：屏幕先黑后显示四色块 + 彩条；顶部 RGB 灯依次变色
#
# 依赖：固件含 st7789 C 模块（USER_C_MODULES 已注入）
#
# 初始化序列说明：
#   - aw9106.init() 必须在 SPI 初始化之前（LCD 电源/背光挂在 AW9106 上）
#   - st7789 用 custom_init 重放原厂逆向出的 18 条初始化命令（modou_lcd.c s_factory_init）
#   - rotation=3 -> MADCTL=0xA0，逻辑画布 160x128（原厂启动态）
#   - factory flag 语义：(flags & 0x11) != 0 时延时 20ms；0x11(SLPOUT)/0x29(DISPON)
#     原厂无延时，这里按 ST7789 datasheet 给足 120ms/50ms

import time
from machine import Pin, SPI

import aw9106
import st7789

# ---- 原厂初始化表（modou_lcd.c s_factory_init，指令+参数+延时 ms）----
FACTORY_INIT = [
    (bytes([0xC0, 0x00]), 20),
    (bytes([0xC2, 0x01, 0xFF]), 0),
    (bytes([0xC3, 0x13]), 20),
    (bytes([0xC4, 0x20]), 20),
    (bytes([0xC6, 0x0F]), 20),
    (bytes([0xCA, 0x0F]), 20),
    (bytes([0xC8, 0x08]), 20),
    (bytes([0x55, 0xB0]), 20),
    (bytes([0x21, 0x00]), 20),          # 原厂即带参数写法，按原样重放
    (bytes([0x36, 0x00]), 20),
    (bytes([0x3A, 0x55]), 20),          # RGB565
    (bytes([0xB1, 0x40, 0x02, 0x14]), 20),
    (bytes([0x26, 0x01]), 20),
    (bytes([0x2A, 0x00, 0x00, 0x00, 0xEF]), 0),
    (bytes([0x2B, 0x00, 0x00, 0x01, 0x3F]), 0),
    (bytes([0x2C, 0x00]), 20),
    (bytes([0xE0, 0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43,
            0x47, 0x09, 0x15, 0x12, 0x16, 0x19]), 0),
    (bytes([0xE1, 0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44,
            0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19]), 0),
    (bytes([0x11]), 120),               # SLPOUT（原厂无延时，这里给足）
    (bytes([0x29]), 50),                # DISPON
]


def run():
    # 1) AW9106 上电（必须最先做：LCD 电源/背光挂在它上面）
    aw9106.init()
    aw9106.rgb(0, 0, 0)          # 顶部灯先灭
    aw9106.backlight(True)
    print("[screen] AW9106 power enabled, backlight on")

    # 2) SPI2 总线（与 DSP HBI 共享；40MHz）
    spi = SPI(2, baudrate=40_000_000, polarity=0, phase=0,
              sck=Pin(32), mosi=Pin(33))

    # 3) st7789 + 原厂初始化序列；rotation=3 -> MADCTL 0xA0，160x128
    tft = st7789.ST7789(
        spi,
        128,
        160,
        reset=Pin(22),
        dc=Pin(21),
        cs=Pin(27),
        rotation=3,
        custom_init=FACTORY_INIT,
        color_order=st7789.MADCTL_RGB,
        inversion=True,
    )
    tft.init()
    print("[screen] st7789 init done (160x128, MADCTL=0xA0)")

    # 4) 测试图案：四色块 + 顶部彩条
    RED = st7789.color565(255, 0, 0)
    GREEN = st7789.color565(0, 255, 0)
    BLUE = st7789.color565(0, 0, 255)
    WHITE = st7789.color565(255, 255, 255)
    YELLOW = st7789.color565(255, 255, 0)
    CYAN = st7789.color565(0, 255, 255)
    MAGENTA = st7789.color565(255, 0, 255)
    BLACK = 0

    tft.fill(BLACK)
    tft.fill_rect(0, 0, 80, 64, RED)
    tft.fill_rect(80, 0, 80, 64, GREEN)
    tft.fill_rect(0, 64, 80, 64, BLUE)
    tft.fill_rect(80, 64, 80, 64, WHITE)
    for i, color in enumerate([RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA]):
        tft.fill_rect(i * 27, 0, 27, 8, color)
    print("[screen] test pattern drawn")

    # 5) 顶部 RGB 灯变色演示（0x20=绿 0x21=红 0x22=蓝）
    for g, r, b in ((255, 0, 0), (0, 255, 0), (0, 0, 255)):
        aw9106.rgb(g, r, b)
        time.sleep_ms(500)
    aw9106.rgb(0, 0, 0)
    print("[screen] done")
    return tft


tft = run()
