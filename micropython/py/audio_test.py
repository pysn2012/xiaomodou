# Modou 音频录放测试（ZL38063 DSP 启动 + machine.I2S 裸录放）
#
# 链路：三麦 -> ZL38063 DSP -> I2S DIN(GPIO35) -> ESP32
#       ESP32 -> I2S DOUT(GPIO26) -> DSP -> 喇叭
# 使用步骤：
#   1. 把 modou/py/ 下的 aw9106.py 和本脚本拷到设备
#   2. REPL 运行：import audio_test
#   3. 预期：dsp 固件下载启动（打印 boot OK）-> 录 2 秒 -> 回放 2 秒
#
# 注意：
#   - aw9106.init() 与 dsp.init() 有顺序要求（AW9106 先于 DSP）
#   - 录放共用 I2S0，先录后放（RX/TX 顺序重配，不做全双工）
#   - 48kHz/16bit/立体声，Philips 标准格式（参数来自 modou_audio_capture.c）

import time
from machine import Pin, I2C, I2S

import aw9106
import dsp

RATE = 48000
BITS = 16
CH = 2
RECORD_SECONDS = 2


def boot_dsp():
    """AW9106 上电 -> DSP 复位 + 固件下载启动。"""
    aw9106.init()
    aw9106.rgb(0, 255, 0)  # 绿灯=进行中
    ok = dsp.init()
    aw9106.rgb(0, 0, 0)
    if not ok:
        raise RuntimeError("DSP loader failed, check info: dsp.info()")
    print("[audio] DSP ready:", dsp.info())


def record(seconds=RECORD_SECONDS):
    n = RATE * CH * (BITS // 8) * seconds
    buf = bytearray(n)
    # ESP32 端口 I2S 签名：mode 必须关键字传参，数据脚统一叫 sd（无 sdin/sdout），采样率参数名是 rate
    i2s = I2S(
        0,
        mode=I2S.RX,
        sck=Pin(5),
        ws=Pin(25),
        sd=Pin(35),
        bits=BITS,
        format=I2S.STEREO,
        rate=RATE,
        ibuf=20480,
    )
    aw9106.rgb(0, 255, 0)
    t0 = time.ticks_ms()
    i2s.readinto(buf)
    i2s.deinit()
    aw9106.rgb(0, 0, 0)
    print("[audio] recorded %d bytes in %d ms" % (len(buf), time.ticks_diff(time.ticks_ms(), t0)))
    return buf


def play(buf):
    i2s = I2S(
        0,
        mode=I2S.TX,
        sck=Pin(5),
        ws=Pin(25),
        sd=Pin(26),
        bits=BITS,
        format=I2S.STEREO,
        rate=RATE,
        ibuf=20480,
    )
    aw9106.rgb(255, 0, 0)  # 红灯=播放中
    i2s.write(buf)
    i2s.deinit()
    aw9106.rgb(0, 0, 0)
    print("[audio] playback done")


def run():
    boot_dsp()
    print("[audio] recording %d seconds..." % RECORD_SECONDS)
    buf = record()
    print("[audio] playing back...")
    play(buf)
    print("[audio] test done")


run()
