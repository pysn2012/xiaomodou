# 魔豆底座控车模块（UART0 与 REPL 共用，底座只认 {AT+...} 帧，REPL 输出它自动忽略）
#
# 协议来源：Flower_modou docs/MOTOR_REMOTE_PROTOCOL_2026-07-30.md（实机验证）
#   唤醒：AT+Pon（每次开机首次发送，之后不用重发）
#   运动：{AT+CMD=(9,1,0,1),(模式,时长ms,转向,动力,0),灯组x4}
#     模式码 2026-11-22 实车标定：0=前进 1=后退 2=左转 3=右转
#     （Flower_modou 源码 remote_motion_code 的 3=前/2=后/0,1=转 与实车不符，勿再抄）
#     转向 0..180：对前进/后退可能是弧线转弯参数（未标定，90=保守取值）
#     时长为 200ms 整数倍；运动帧有效期约 800ms，超时自动停
#   灯组（每帧必带 4 组，全零=每帧都在关灯）：第4参数=段数 1=常亮 2=闪烁，
#     第3参数=时长 ms；行进灯光与运动同帧携带（22:09 板测：独立灯效帧会被
#     后续运动帧覆盖）。关灯帧 = (7,色,60000,0) 0 段。
#
# 用法（WebREPL 或串口均可）：
#   import car_ctrl
#   car_ctrl.probe()                 # 探测：发 AT+Ver，打印底座应答（不动电机）
#   car_ctrl.wake()                  # 唤醒底座（听见灯效/上电动作）
#   car_ctrl.forward(800, 60)        # 前进 800ms 动力 60
#   car_ctrl.backward(400, 50)       # 后退
#   car_ctrl.spin_left(400, 50)      # 原地左转
#   car_ctrl.spin_right(400, 50)     # 原地右转
#   car_ctrl.stop()                  # 急停 + 灯光复位
#   car_ctrl.front_light() / tail_light() / flash_light()   # 灯光效果
#   car_ctrl.demo()                  # 全套演示

import time
from machine import UART

BAUD = 115200

# 固件已关闭 UART REPL（MICROPY_HW_ENABLE_UART_REPL=0）：
#   UART0 完全空闲、专供底座通信，这里独占初始化是安全的。
#   旧固件保留 REPL 时重建 UART0 会与 REPL IRQ 冲突卡死主任务——那正是
#   上午"WebREPL 全挂"的根因，关 REPL 后一并消失。
_uart = UART(0, baudrate=BAUD, tx=1, rx=3, timeout=0)

Zeros = "(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)"


def _send(cmd_text):
    """发送一帧 {AT+...}\r\n。底座 AT 解析器只认这种帧。"""
    _uart.write("{%s}\r\n" % cmd_text)


def wake():
    """唤醒底座供电域。首次发一次即可，运动帧不会重新锁电机。"""
    _send("AT+Pon")
    time.sleep_ms(300)


def probe(timeout_ms=1200):
    """链路探测（不动电机）：发 {AT+Ver} 询问帧，打印底座在 timeout 内
    回的一切字节。有回话 = TX/RX 双向都通；无回话 = 底座没上电或没收到。
    返回 bytes（空 = 无应答）。"""
    _uart.read()  # 清掉缓冲里的旧数据
    _send("AT+Ver")
    buf = b""
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < timeout_ms:
        r = _uart.read(128)
        if r:
            buf += r
        time.sleep_ms(20)
    if buf:
        print("[car] base reply %d bytes: %r" % (len(buf), buf))
    else:
        print("[car] no reply within %dms" % timeout_ms)
    return buf


def move(mode, duration_ms, steer, power, light=None):
    """单帧运动。light=None 不带灯；"front"=前灯随行；"tail"=尾灯随行。
    2026-11-22 板测定案：运动帧(9,..)的 4 个灯组槽每帧都会生效，全零灯组
    等于每帧都在关灯——独立发的灯效帧 250ms 后就被运动帧覆盖，所以
    行进灯光必须与运动同帧携带（灯组时长=运动时长，同步到期）。"""
    d = max(200, (int(duration_ms) // 200) * 200)
    if light == "front":
        groups = "(7,0,%d,1),(7,0,%d,1),(0,0,0,0),(0,0,0,0)" % (d, d)
    elif light == "tail":
        groups = "(0,0,0,0),(0,0,0,0),(7,1,%d,1),(7,1,%d,1)" % (d, d)
    else:
        groups = "(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)"
    _send("AT+CMD=(9,1,0,1),(%d,%d,%d,%d,0),%s" % (mode, d, steer, power, groups))


def forward(duration_ms=800, power=60, light=None):
    """前进（模式 0，2026-11-22 实车标定）。light="front" 前灯随行。"""
    move(0, duration_ms, 90, power, light)


def backward(duration_ms=800, power=60, light=None):
    """后退（模式 1）。light="tail" 尾灯随行。"""
    move(1, duration_ms, 90, power, light)


def spin_left(duration_ms=400, power=50):
    """左转（模式 2）。"""
    move(2, duration_ms, 90, power)


def spin_right(duration_ms=400, power=50):
    """右转（模式 3）。"""
    move(3, duration_ms, 90, power)


# ---- 灯光 / 急停帧（逐字取自 Flower_modou car_remote_server.cc，实机验证格式）----

_STOP_FRAMES = (
    "AT+CMD=(1,2,3,2),(255,0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
    "AT+CMD=(1,3,3,3),(255,0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
)


def stop():
    """急停 + 灯光复位：连发两帧停止帧（帧间 18ms，与 Flower_modou 一致）。"""
    for f in _STOP_FRAMES:
        _send(f)
        time.sleep_ms(18)


def front_light():
    """前灯长亮（白色）。2026-11-22 灯光实验矩阵定案：第 4 参数=段数，
    1=常亮一段（60s），2=闪烁（T2 常亮/T6 闪 实测）。松手调 lights_off()。"""
    _send("AT+CMD=(4,1,0,1),(0,0,0,0,0),(7,0,60000,1),(7,0,60000,1),(0,0,0,0),(0,0,0,0)")


def tail_light():
    """尾灯长亮（红色，60s，P4/P5 成对填）。"""
    _send("AT+CMD=(4,1,0,1),(0,0,0,0,0),(0,0,0,0),(0,0,0,0),(7,1,60000,1),(7,1,60000,1)")


def lights_off():
    """前后灯全关：0 段帧（T1 实测不亮）替换正在跑的灯效，无闪烁过渡。
    若板测发现关不掉正在跑的灯效，改用 (7,x,1,1) 1ms 帧再议。"""
    _send("AT+CMD=(4,1,0,1),(0,0,0,0,0),(7,0,60000,0),(7,0,60000,0),(7,1,60000,0),(7,1,60000,0)")


def flash_light():
    """前后灯交替闪烁（500ms）。"""
    _send("AT+CMD=(12,1,3,1),(0,0,0,0,0),(7,0,500,1),(7,0,500,1),(7,1,500,1),(7,1,500,1)")


def demo():
    """完整演示：唤醒 -> 前进(前灯) -> 后退(尾灯) -> 左转 -> 右转 -> 灯光闪烁。"""
    print("[car] wake base...")
    wake()
    time.sleep(1)
    print("[car] forward (front light)")
    forward(800, 60, light="front")
    time.sleep(1.5)
    print("[car] backward (tail light)")
    backward(800, 60, light="tail")
    time.sleep(1.5)
    for name, fn in (("spin_left", spin_left), ("spin_right", spin_right)):
        print("[car] %s" % name)
        fn()
        time.sleep(1.5)
    print("[car] lights flash")
    flash_light()
    time.sleep(1.5)
    print("[car] demo done (最后一次运动帧 800ms 后自动停止)")


print("[car] ready: car_ctrl.wake() / forward() / backward() / spin_left() / spin_right() / demo()")
