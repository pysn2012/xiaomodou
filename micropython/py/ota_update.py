# Modou OTA 完整升级脚本（设备端，REPL 运行）
# ------------------------------------------------------------------
# 准备（PC 端）：
#   1. 下载新固件 artifact，取其中的 micropython.bin（注意是 .bin，不是 firmware.bin！
#      OTA 载荷 = app image；firmware.bin 是含 bootloader 的合并镜像，不能用于 OTA）
#   2. 在 micropython.bin 所在目录起局域网服务器：python -m http.server 8000
#   3. 查 PC 的局域网 IP（ipconfig 里的 IPv4 地址）
#
# 设备端（REPL）：
#   import ota_update
#   ota_update.run("192.168.31.x", "/mpy_1.29_MODOU_OTA_16MB_micropython.bin", 8000)
#
# 升级完成后按提示重启；重启后新固件处于 pending_verify（回滚保护），
# 联网成功后由 boot.py 自动 mark_valid 固化。
# ------------------------------------------------------------------

import usocket as socket
import ota
import machine

CHUNK = 4096


def _http_get(host, path, port):
    addr = socket.getaddrinfo(host, port)[0][-1]
    s = socket.socket()
    s.settimeout(10)
    s.connect(addr)
    s.send(b"GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, host))
    hdr = b""
    while b"\r\n\r\n" not in hdr:
        d = s.recv(512)
        if not d:
            raise OSError("connection closed before header complete")
        hdr += d
    head, body = hdr.split(b"\r\n\r\n", 1)
    status = int(head.split(b"\r\n")[0].split(b" ")[1])
    if status != 200:
        s.close()
        raise OSError("HTTP status %d" % status)
    length = None
    for ln in head.split(b"\r\n"):
        if ln.lower().startswith(b"content-length:"):
            length = int(ln.split(b":")[1])
    return s, body, length


def run(host, path, port=80):
    info = ota.info()
    print("[ota] running=%s  next=%s" % (info[0], info[1]))
    print("[ota] version=%s build=%s %s" % (info[2], info[3], info[4]))

    s, body, length = _http_get(host, path, port)
    if length is None:
        s.close()
        raise OSError("server must return Content-Length")
    print("[ota] payload total: %d bytes" % length)

    ota.begin()
    n = 0
    try:
        while True:
            if body:
                d, body = body, b""
            else:
                d = s.recv(CHUNK)
            if not d:
                break
            ota.write(d)
            n += len(d)
            if (n // 65536) != ((n - len(d)) // 65536):
                print("[ota] %d/%d (%d%%)" % (n, length, n * 100 // length))
        s.close()
        if n != length:
            raise OSError("short read: %d/%d" % (n, length))
        ota.finish()
    except Exception:
        ota.abort()  # 失败放弃升级，otadata 不动，重启后仍跑旧固件
        raise

    print("[ota] OK: payload %d bytes written, boot slot switched" % n)
    print("[ota] reboot to run new firmware:")
    print("      import machine; machine.reset()")
    # 自检式重启（不需要的话注释掉下一行）
    machine.reset()
