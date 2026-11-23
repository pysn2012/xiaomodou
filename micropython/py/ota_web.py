# Modou 网页 OTA 升级器（设备端）
# ------------------------------------------------------------------
# 用法：
#   1. 把本脚本传到板子，REPL 运行：
#        import ota_web; ota_web.run()
#   2. 串口会打印 http://<板子IP>/ —— 用局域网内任意电脑/手机浏览器打开
#   3. 页面显示当前分区和固件版本；点「选择固件」选新版 micropython.bin
#      （注意是 Actions artifact 里的 micropython.bin，不是 firmware.bin）
#   4. 点「开始升级」→ 进度条走完 → 设备自动重启进新固件
#   5. 重启后新固件处于 pending_verify（回滚保护），联网成功后由
#      boot.py / 本脚本自动 mark_valid 固化；确认前复位会回退旧固件
#
# 原理：设备起 HTTP 服务；GET / 返回升级页面；POST /upload 收裸文件流，
#       边收边写入 OTA 更新槽（ota.begin/write/finish）；完成即切槽重启。
#       （差分升级已剥离，只接受 0xE9 开头的完整 app image）
# ------------------------------------------------------------------

import usocket as socket
import network
import time
import ota
import machine

# ====== 配置部分 ======
SSID = "SSID"
PASSWORD = "PASSWORD"
PORT = 80
# 更新槽 2.875MB，留余量
MAX_PAYLOAD = 2800000

_HTML = (
    # 注意：HTTP 响应头由 _send() 统一生成，这里只放正文，
    # 否则头部会被浏览器当成正文渲染在页面顶部
    b"<!DOCTYPE html><html><head><meta charset='utf-8'>"
    b"<meta name='viewport' content='width=device-width,initial-scale=1'>"
    b"<title>Modou OTA</title><style>"
    b"body{font-family:sans-serif;max-width:420px;margin:32px auto;padding:0 16px;color:#333}"
    b"h2{margin:0 0 14px;font-size:20px}"
    b".card{background:#f7f8fa;border:1px solid #e4e5e8;border-radius:12px;padding:14px 16px;margin-bottom:14px}"
    b".row{display:flex;justify-content:space-between;gap:12px;padding:3px 0;font-size:14px}"
    b".k{color:#8a8a8a;flex-shrink:0}"
    b".v{font-weight:500;text-align:right;word-break:break-all}"
    b".lbl{font-size:13px;color:#8a8a8a;margin-bottom:6px}"
    b"input[type=file]{width:100%;margin-bottom:12px;font-size:14px}"
    b"button{width:100%;padding:12px;font-size:16px;background:#2b7de9;color:#fff;border:0;border-radius:8px}"
    b".bar{background:#e9e9ec;height:20px;border-radius:10px;overflow:hidden;margin:14px 0 8px}"
    b".fill{background:#2b7de9;height:100%;width:0%;transition:width .3s}"
    b"#st{font-size:13px;color:#555;white-space:pre-wrap;min-height:16px}"
    b"</style></head><body>"
    b"<h2>Modou OTA \xe5\x8d\x87\xe7\xba\xa7</h2>"
    b"<div class='card' id='cur'>\xe8\xaf\xbb\xe5\x8f\x96\xe5\x9b\xba\xe4\xbb\xb6\xe4\xbf\xa1\xe6\x81\xaf\xe4\xb8\xad...</div>"
    b"<div class='card'>"
    b"<div class='lbl'>\xe5\x8d\x87\xe7\xba\xa7\xe6\x96\x87\xe4\xbb\xb6\xef\xbc\x88micropython.bin\xef\xbc\x89</div>"
    b"<input type='file' id='f' accept='.bin'>"
    b"<button onclick='up()'>\xe5\xbc\x80\xe5\xa7\x8b\xe5\x8d\x87\xe7\xba\xa7</button>"
    b"<div class='bar'><div class='fill' id='p'></div></div>"
    b"<div id='st'></div>"
    b"</div>"
    b"<script>"
    b"function $(id){return document.getElementById(id);}"
    b"function row(k,v){return \"<div class='row'><span class='k'>\"+k+\"</span><span class='v'>\"+v+\"</span></div>\";}"
    b"var sm={pending_verify:'\xe5\xbe\x85\xe9\xaa\x8c\xe8\xaf\x81',valid:'\xe5\xb7\xb2\xe7\xa1\xae\xe8\xae\xa4',invalid:'\xe6\x97\xa0\xe6\x95\x88',aborted:'\xe5\xb7\xb2\xe4\xb8\xad\xe6\xad\xa2',undefined:'\xe9\xa6\x96\xe7\x83\xa7/\xe6\x9c\xaa\xe7\x9f\xa5'};"
    b"fetch('/info').then(r=>r.text()).then(t=>{"
    b"var m={};t.split('\\n').forEach(function(l){var i=l.indexOf('=');if(i>0)m[l.slice(0,i)]=l.slice(i+1);});"
    b"var v=m.version&&m.version.length>24?m.version.slice(0,24)+'\xe2\x80\xa6':m.version;"
    b"$('cur').innerHTML=row('\xe8\xbf\x90\xe8\xa1\x8c\xe5\x88\x86\xe5\x8c\xba',m.partition)"
    b"+row('\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac',v)"
    b"+row('\xe6\x9e\x84\xe5\xbb\xba\xe6\x97\xb6\xe9\x97\xb4',m.build)"
    b"+row('IDF \xe7\x89\x88\xe6\x9c\xac',m.idf)"
    b"+row('\xe9\x95\x9c\xe5\x83\x8f\xe7\x8a\xb6\xe6\x80\x81',sm[m.state]||m.state);});"
    b"$('f').onchange=function(){var f=$('f').files[0];"
    b"$('st').textContent=f?'\xe5\xb7\xb2\xe9\x80\x89\xe6\x8b\xa9: '+f.name+' ('+f.size+' bytes)':'';};"
    b"function up(){"
    b"var el=$('f');var f=el.files[0];"
    b"if(!f){$('st').textContent='\xe8\xaf\xb7\xe5\x85\x88\xe9\x80\x89\xe6\x8b\xa9\xe5\x9b\xba\xe4\xbb\xb6';return;}"
    b"$('st').textContent='\xe6\xad\xa3\xe5\x9c\xa8\xe5\x8d\x87\xe7\xba\xa7\xef\xbc\x8c\xe8\xaf\xb7\xe5\x8b\xbf\xe6\x96\xad\xe7\x94\xb5...';"
    b"var x=new XMLHttpRequest();"
    b"x.open('POST','/upload');"
    b"x.upload.onprogress=function(e){"
    b"var p=Math.round(e.loaded*100/e.total);"
    b"$('p').style.width=p+'%';"
    b"$('st').textContent=p+'%  '+e.loaded+'/'+e.total+' bytes';};"
    b"x.onload=function(){$('st').textContent='OK: '+x.responseText;"
    b"$('st').textContent+='\\n\xe5\x8d\x87\xe7\xba\xa7\xe5\xae\x8c\xe6\x88\x90\xef\xbc\x8c\xe8\xae\xbe\xe5\xa4\x87\xe9\x87\x8d\xe5\x90\xaf\xe4\xb8\xad...'};"
    b"x.onerror=function(){$('st').textContent='\xe4\xb8\x8a\xe4\xbc\xa0\xe5\xa4\xb1\xe8\xb4\xa5';};"
    b"x.send(f);}"
    b"</script></body></html>"
)


def connect_wifi():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    if not wlan.isconnected():
        print("正在连接WiFi:%s..." % SSID)
        wlan.connect(SSID, PASSWORD)
        t = 20
        while not wlan.isconnected() and t > 0:
            print(".", end="")
            time.sleep(1)
            t -= 1
        if not wlan.isconnected():
            raise RuntimeError("WiFi连接超时")
    print()
    print("[web-ota] 设备IP:http://%s/" % wlan.ifconfig()[0])
    return wlan


def _send(conn, status, ctype, body):
    conn.send("HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n"
              % (status, ctype, len(body)))
    for i in range(0, len(body), 1024):
        conn.send(body[i:i + 1024])


def _read_head(conn):
    # 读请求头（\r\n\r\n 截断），返回 (head_bytes, 剩余的首段 body)
    head = b""
    while b"\r\n\r\n" not in head:
        d = conn.recv(512)
        if not d:
            raise OSError("对端提前断开")
        head += d
        if len(head) > 8192:
            raise OSError("请求头过大")
    h, body = head.split(b"\r\n\r\n", 1)
    return h, body


def handle_upload(conn, head, body):
    lines = head.split(b"\r\n")
    length = None
    for ln in lines:
        if ln.lower().startswith(b"content-length:"):
            length = int(ln.split(b":", 1)[1])
    if length is None or length < 64 or length > MAX_PAYLOAD:
        _send(conn, "400 Bad Request", "text/plain",
              b"bad payload size (need micropython.bin app image)")
        return
    print("[web-ota] 收到数据:%d字节" % length)
    info = ota.info()
    print("[web-ota] 运行槽:%s 目标槽:%s" % (info[0], info[1]))

    # ESP app image 首字节固定 0xE9；差分升级已剥离，只接受整包
    d = b""
    while not d:
        if body:
            d, body = body, b""
        else:
            d = conn.recv(4096)
        if not d:
            raise OSError("数据为空")
    if d[0] != 0xE9:
        _send(conn, "400 Bad Request", "text/plain",
              ("not an app image (head=%s)" % d[0:8].hex()).encode())
        return

    n = 0
    try:
        ota.begin()
        while n < length:
            if not d:
                d = conn.recv(4096)
            if not d:
                raise OSError("读取不完整:%d/%d" % (n, length))
            take = min(len(d), length - n)
            chunk = d[:take]
            d = b""
            n += take
            ota.write(chunk)
            if (n // 65536) != ((n - take) // 65536):
                print("[web-ota] %d/%d (%d%%)" % (n, length, n * 100 // length))
        ota.finish()
    except Exception as e:
        ota.abort()
        print("[web-ota] 升级失败:%s" % e)
        _send(conn, "500 Internal Server Error", "text/plain",
              ("upgrade failed: %s\n(old firmware still running)" % e).encode())
        return
    print("[web-ota] 完成:%d字节,启动槽已切换,重启中..." % n)
    _send(conn, "200 OK", "text/plain",
          b"upgrade done (%d bytes). rebooting to new firmware..." % n)
    time.sleep(1)
    machine.reset()


def run():
    connect_wifi()
    info = ota.info()
    state = info[7] if len(info) > 7 else "undefined"
    print("[web-ota] 运行槽:%s 版本:%s 构建:%s %s 状态:%s" % (
        info[0], info[2], info[3], info[4], state))
    # 回滚保护确认：升级重启后的新固件处于 pending_verify，能联网说明固件基本可用，
    # 自动 mark_valid 解除回滚保护（否则此时复位会被 bootloader 回退到旧分区）。
    # 固件里待确认的分区状态：pending_verify -> valid
    if state == "pending_verify":
        ota.mark_valid()
        print("[web-ota] 新固件已确认固化(回滚保护解除)")

    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT))
    s.listen(1)
    print("[web-ota] HTTP服务已就绪,端口:%d (Ctrl+C停止)" % PORT)

    while True:
        conn, addr = s.accept()
        conn.settimeout(8)
        try:
            head, body = _read_head(conn)
            # 注意：MicroPython 的 bytes.decode() 不支持 errors 关键字参数（与 CPython 不同）
            line = head.split(b"\r\n")[0].decode()
            path = line.split(" ")[1] if " " in line else "/"
            if path.startswith("/info"):
                i = ota.info()
                state = i[7] if len(i) > 7 else "undefined"
                txt = "\n".join([
                    "partition=%s" % i[0],
                    "version=%s" % i[2],
                    "build=%s %s" % (i[3], i[4]),
                    "idf=%s" % i[5],
                    "state=%s" % state,
                ])
                _send(conn, "200 OK", "text/plain", txt.encode())
            elif path.startswith("/upload"):
                handle_upload(conn, head, body)
                continue  # 上传完成后设备已重启
            else:
                _send(conn, "200 OK", "text/html", _HTML)
        except Exception as e:
            print("[web-ota] 请求处理出错:%s" % e)
            import sys
            sys.print_exception(e)
            try:
                _send(conn, "400 Bad Request", "text/plain", str(e).encode())
            except Exception:
                pass
        conn.close()

run()