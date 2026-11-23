# Modou 设备端 boot.py（拷到设备根目录，开机自动运行）
# ------------------------------------------------------------------
# 流程：
#   1. WebREPL 密码初始化（webrepl_cfg.py 不存在时自动生成）
#   2. 连接 Wi-Fi（STA 模式，关闭省电——见 _connect_wifi 注释）
#   3. OTA 回滚保护自检（新固件联网成功即自动固化）
#   4. 启动 WebREPL（浏览器/Thonny ws://<IP>:8266）
#
# 使用约定：
#   - 控车：WebREPL 连上后手动 `import car_web; car_web.start()`
#   - 不放 main.py：UART0 顶针接底座后被占用，boot 流程不依赖业务代码
#   - 开机时不要插充电线（充电状态会阻塞 WiFi/WebREPL，2026-11-22 板测实证）
# ------------------------------------------------------------------

import network
import time
import os
import webrepl

try:
    import ota
    _HAS_OTA = True
except ImportError:
    _HAS_OTA = False

# ====== 配置部分 ======
SSID = "SSID"
PASSWORD = "PASSWORD"
WEBREPL_PASSWORD = "1234"


def _ensure_webrepl_cfg():
    """webrepl_cfg.py 不存在时自动生成（webrepl 模块只识别变量名 PASS）。"""
    try:
        os.stat("webrepl_cfg.py")
    except OSError:
        with open("webrepl_cfg.py", "w") as f:
            f.write('PASS = "%s"\n' % WEBREPL_PASSWORD)
        print("[boot] webrepl_cfg.py created")


def _connect_wifi(timeout_s=20):
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    # 关闭 WiFi 省电：ESP32 端口 pm 取值 0/1/2（0=WIFI_PS_NONE）。
    # 默认省电模式空闲后 modem sleep，WebREPL 会"TCP 握手成功但应用层无响应"。
    # 注意不能用 ESP8266 的位掩码值（如 0xA11140，会报 Wifi Unknown Error 0x0102）。
    wlan.config(pm=0)
    if not wlan.isconnected():
        print("[boot] connecting WiFi: %s ..." % SSID)
        wlan.connect(SSID, PASSWORD)
        t = timeout_s
        while not wlan.isconnected() and t > 0:
            print(".", end="")
            time.sleep(1)
            t -= 1
        print()
    if wlan.isconnected():
        print("[boot] WiFi OK, IP: %s" % wlan.ifconfig()[0])
    else:
        print("[boot] WiFi FAILED (WebREPL unavailable this boot)")
    return wlan


def _confirm_ota_if_pending():
    """OTA 新固件联网成功即确认固化（pending_verify -> valid）。"""
    if not _HAS_OTA:
        return
    try:
        info = ota.info()
        state = info[7] if len(info) > 7 else "undefined"
        print("[boot] ota: slot=%s version=%s state=%s" % (info[0], info[2], state))
        if state == "pending_verify":
            ota.mark_valid()
            print("[boot] OTA firmware confirmed")
    except Exception as e:
        print("[boot] ota check skipped: %s" % e)


_ensure_webrepl_cfg()
_wlan = _connect_wifi()
if _wlan.isconnected():
    _confirm_ota_if_pending()
webrepl.start()
if _wlan.isconnected():
    print("[boot] ready: ws://%s:8266" % _wlan.ifconfig()[0])
else:
    print("[boot] ready (WiFi down, WebREPL listening anyway)")


