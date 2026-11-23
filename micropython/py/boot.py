# Modou 设备端 boot.py（拷到设备根目录，开机自动运行）
# ------------------------------------------------------------------
# 流程：
#   1. WebREPL 密码初始化（webrepl_cfg.py 不存在时自动生成，密码见下方配置）
#   2. 连接 Wi-Fi（STA 模式，SSID/PASSWORD 见下方配置）
#   3. 启动 WebREPL（Thonny/浏览器 ws://<IP>:8266 在线编程）
#   4. 回滚保护自检：OTA 升级后的新固件处于 pending_verify，
#      能连上 Wi-Fi 说明固件基本可用，自动 ota.mark_valid() 固化；
#      若连不上 Wi-Fi 就不确认，手动复位会回退旧固件（回滚保护生效）
#
# 注意：
#   - boot.py 里 SSID/PASSWORD 改成你自己的
#   - WebREPL 密码建议修改（WebREPL_PASSWORD），长度 4~9 字符
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
SSID = "shami"
PASSWORD = "cnas2020"
WEBREPL_PASSWORD = "1234"


def _ensure_webrepl_cfg():
    """webrepl_cfg.py 不存在时自动生成（WebREPL 密码配置文件）。"""
    try:
        os.stat("webrepl_cfg.py")
        return
    except OSError:
        with open("webrepl_cfg.py", "w") as f:
            f.write('PASSWD = "%s"\n' % WEBREPL_PASSWORD)
        print("[boot] webrepl_cfg.py created (password=%s)" % WEBREPL_PASSWORD)


def _connect_wifi(timeout_s=20):
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
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
            print("[boot] OTA firmware confirmed (rollback protection released)")
    except Exception as e:
        # 首烧无 ota_0/ota_1 或模块异常时不阻塞启动
        print("[boot] ota check skipped: %s" % e)


_ensure_webrepl_cfg()
_wlan = _connect_wifi()
if _wlan.isconnected():
    _confirm_ota_if_pending()
webrepl.start()
print("[boot] WebREPL started on ws://%s:8266" % _wlan.ifconfig()[0])
