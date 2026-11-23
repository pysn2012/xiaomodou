# Modou AW9106 灯控/背光驱动（I2C 0x5b，SDA=18 SCL=23）
# 序列移植自 Flower_modou modou_key_lights.c / modou_lcd.c（原厂逆向，已实机验证）
#
# 通道映射（2026-07-28 板上验证）：0x20=绿 0x21=红 0x22=蓝（顶部 RGB 灯）
# LCD 电源/背光：0x23/0x24（0x80 开 / 0x00 关），不要动 0x20/0x21/0x22
#
# 重要：aw9106.init() 必须在屏幕 SPI 初始化和 dsp.init() 之前调用！

import time
from machine import Pin, SoftI2C

ADDR = 0x5B
SDA = 18
SCL = 23

_i2c = None
_present = False

# 原厂上电序列（key_lights 完整版，含 0x23/0x24=0x80 背光开 + 0x02=0x02 输出使能）
_FACTORY_INIT = [
    (0x7F, 0x00), (0x11, 0x10), (0x12, 0x02), (0x04, 0x00), (0x02, 0x00),
    (0x13, 0x00), (0x20, 0x00), (0x21, 0x00), (0x22, 0x00), (0x24, 0x00),
    (0x24, 0x00), (0x25, 0x00), (0x14, 0x07), (0x05, 0x07), (0x17, 0x01),
    (0x18, 0x01), (0x19, 0x01), (0x15, 0x1B), (0x16, 0x08), (0x23, 0x80),
    (0x24, 0x80), (0x02, 0x02), (0x14, 0x00), (0x11, 0x12),
]


def _write(reg, value):
    _i2c.writeto_mem(ADDR, reg, bytes([value]))


def init(i2c=None):
    """探测 AW9106 并重放原厂上电序列。必须最先调用。"""
    global _i2c, _present
    _i2c = i2c if i2c is not None else SoftI2C(sda=Pin(SDA), scl=Pin(SCL), freq=100000)
    _present = ADDR in _i2c.scan()
    if not _present:
        raise OSError("AW9106 (0x5b) not found on I2C bus")
    for i, (reg, value) in enumerate(_FACTORY_INIT):
        _write(reg, value)
        time.sleep_ms(5 if i == 0 else 1)
    return True


def present():
    return _present


def rgb(green, red, blue):
    """顶部 RGB 灯：0x20=绿 0x21=红 0x22=蓝，0~255。"""
    if not _present:
        raise OSError("AW9106 not initialized, call init() first")
    # 常亮模式寄存器（原厂 set_steady 前置写法）
    _write(0x14, 0x00)
    _write(0x11, 0x12)
    _write(0x20, green & 0xFF)
    _write(0x21, red & 0xFF)
    _write(0x22, blue & 0xFF)


def set_one(channel, brightness):
    """单通道调节：0x20/0x21/0x22。"""
    if not _present:
        raise OSError("AW9106 not initialized, call init() first")
    _write(channel, brightness & 0xFF)


def backlight(enabled):
    """LCD 电源/背光开关：0x23/0x24 = 0x80(开) / 0x00(关)。不动 RGB 灯。"""
    if not _present:
        raise OSError("AW9106 not initialized, call init() first")
    value = 0x80 if enabled else 0x00
    _write(0x23, value)
    time.sleep_ms(1)
    _write(0x24, value)
