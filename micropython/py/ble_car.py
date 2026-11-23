# 魔豆 BLE 遥控（微信小程序端，按住持续动作/松开停止）
#
# 使用（WebREPL 连上后手动运行）：
#   import ble_car
#   ble_car.remote()
#
#   小程序扫描并连接设备名 "MODOU"：
#     前进=upup  后退=down  左转=left  右转=right   （按住持续，松开停）
#     前进自动亮前灯、后退自动亮尾灯（左/右转不联动），松开全关
#     A=前灯长亮  B=尾灯长亮                          （按住长亮，松开发 stop 关灯）
#     松开任何键小程序统一发 "stop"
#   另兼容经典 5 字符帧 "!B<键号><1按/0松><校验和>"（其他小程序版本）：
#     B5/6/7/8=方向  B1=前灯 B2=尾灯 B3=闪烁 B4=唤醒
#
# 小程序协议（2026-11-22 板测实机确认）：
#   - 文本指令：按住方向键连发（如 upup），松开统一发 stop
#   - 5 字符帧校验和：使 5 字符 ASCII 总和 = 255（全样例验算）
#
# 实现要点（按住持续的关键）：
#   收到方向/灯指令只记状态，主循环自动续帧——运动帧 300ms 刷新
#   （帧时效 400ms）、灯效帧 1.5s 刷新（帧循环 2s）。即使小程序只发
#   一次按下消息，车也会一直走到收到 stop 或蓝牙断开为止。
#   断连/退出均自动急停。

import bluetooth
import time

import car_ctrl

BLE_MSG = ""

# Nordic UART Service UUID（与小程序一致）
UUID_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
UUID_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # 小程序写 -> ESP32 收
UUID_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # ESP32 notify -> 小程序

# 按住持续的指令 -> 刷新函数（灯组与运动同帧：前进带前灯、后退带尾灯）
MOTION_FN = {
    "upup": lambda: car_ctrl.forward(400, 60, light="front"),
    "down": lambda: car_ctrl.backward(400, 60, light="tail"),
    "left": lambda: car_ctrl.spin_left(400, 50),
    "right": lambda: car_ctrl.spin_right(400, 50),
}
LIGHT_FN = {
    "A": car_ctrl.front_light,
    "B": car_ctrl.tail_light,
    "a": car_ctrl.front_light,
    "b": car_ctrl.tail_light,
}
# 单次型指令（发一次执行一次，不保持）
ONESHOT_FN = {
    "flash": car_ctrl.flash_light,
    "act1": car_ctrl.front_light,
    "bac2": car_ctrl.tail_light,
}

MOTION_REFRESH_MS = 300  # 运动帧 400ms 时效，300ms 刷新无缝衔接
LIGHT_REFRESH_MS = 50000  # 灯长亮帧时效 60s，50s 续期兜底

# 共享状态（ble_irq 断连时也要能清零急停）
STATE = {"motion": None, "light": None}


class BLECar:
    """蓝牙外设：注册 NUS 服务、广播设备名、收小程序指令。"""

    def __init__(self, name="MODOU"):
        self.name = name
        self.ble = bluetooth.BLE()
        self.ble.active(True)
        self.ble.config(gap_name=name)
        self.ble.irq(self.ble_irq)
        self.register()
        # 默认只收 20 字节，扩到 100
        self.ble.gatts_write(self.rx, bytes(100))
        self.advertise()

    def register(self):
        services = (
            (
                bluetooth.UUID(UUID_SERVICE),
                (
                    (bluetooth.UUID(UUID_TX), bluetooth.FLAG_NOTIFY),
                    (bluetooth.UUID(UUID_RX), bluetooth.FLAG_WRITE),
                ),
            ),
        )
        ((self.tx, self.rx),) = self.ble.gatts_register_services(services)

    def advertise(self):
        # MicroPython 编码名大小写敏感，"UTF-8" 会 LookupError；用默认 utf-8
        name = self.name.encode()
        adv = bytearray(b"\x02\x01\x02") + bytearray((len(name) + 1, 0x09)) + name
        self.ble.gap_advertise(100, adv)
        print("[ble] advertising: %s" % self.name)

    def ble_irq(self, event, data):
        global BLE_MSG
        if event == 1:  # 手机连上
            print("[ble] connected")
        elif event == 2:  # 手机断开 -> 急停 + 关灯 + 重新广播
            print("[ble] disconnected, emergency stop")
            STATE["motion"] = None
            STATE["light"] = None
            try:
                car_ctrl.stop()
                car_ctrl.lights_off()
            except Exception:
                pass
            self.advertise()
        elif event == 3:  # 收到写入
            BLE_MSG = self.ble.gatts_read(self.rx).decode().strip()


def _apply(msg):
    """处理一条消息。返回 True=已识别，False=未知。"""
    if msg == "stop":
        STATE["motion"] = None
        STATE["light"] = None
        car_ctrl.stop()
        car_ctrl.lights_off()  # 小程序松手统一发 stop：停车 + 关灯
        return True
    if msg in MOTION_FN:
        STATE["motion"] = MOTION_FN[msg]
        STATE["motion"]()  # 灯组已随运动帧携带，无需独立灯效帧
        return True
    if msg in LIGHT_FN:
        STATE["light"] = LIGHT_FN[msg]
        STATE["light"]()
        return True
    if msg in ONESHOT_FN:
        ONESHOT_FN[msg]()
        return True
    # ---- 经典 5 字符帧（其他小程序版本）----
    if msg.startswith("!B") and len(msg) == 5:
        if sum(map(ord, msg)) != 255:  # 校验和：ASCII 总和 = 255
            return False
        btn, state = msg[2], msg[3]
        if state == "0":  # 松开 -> 停车 + 关灯
            STATE["motion"] = None
            STATE["light"] = None
            car_ctrl.stop()
            car_ctrl.lights_off()
            return True
        # 键号映射到文本别名，复用同一套状态机
        alias = {"5": "upup", "6": "down", "7": "left", "8": "right",
                 "1": "A", "2": "B"}
        if btn == "3":
            car_ctrl.flash_light()
        elif btn == "4":
            car_ctrl.wake()
        elif btn in alias:
            return _apply(alias[btn])
        return True
    return False


def remote(name="MODOU"):
    """一键 BLE 遥控：唤醒底座 -> 起广播 -> 状态机 + 自动续帧。
    Ctrl-C 退出（自动急停）。"""
    global BLE_MSG  # 循环里有 BLE_MSG="" 赋值，必须声明全局，否则被当局部变量
    car_ctrl.wake()
    ble = BLECar(name)
    print("[ble] remote ready: open mini-program, connect '%s'" % name)
    t_motion = 0
    t_light = 0
    try:
        while True:
            if BLE_MSG:
                msg = BLE_MSG
                BLE_MSG = ""
                print("[ble] >>%s<<" % msg)
                try:
                    if not _apply(msg):
                        print("[ble] unknown msg: %r" % msg)
                except Exception as e:
                    print("[ble] handle err: %r" % e)
            now = time.ticks_ms()
            if STATE["motion"] and time.ticks_diff(now, t_motion) >= MOTION_REFRESH_MS:
                STATE["motion"]()
                t_motion = now
            if STATE["light"] and time.ticks_diff(now, t_light) >= LIGHT_REFRESH_MS:
                STATE["light"]()
                t_light = now
            time.sleep_ms(50)
    except KeyboardInterrupt:
        STATE["motion"] = None
        STATE["light"] = None
        car_ctrl.stop()
        print("\n[ble] remote exited (car stopped)")


print("[car] ble_car loaded: BLE remote = ble_car.remote()")
