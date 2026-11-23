# xiaomodou
小魔豆STEM机器人（魔豆小车）固件仓库：MicroPython OTA 固件 + ESPHome 语音助手固件。

硬件：ESP32-D0WD / 16MB Flash / 8MB PSRAM，ST7789 160×128 屏、ZL38063 三麦 DSP、AW9106 灯控/背光。

## 目录

```
micropython/          # MicroPython OTA 固件（mpy 1.29 + IDF 5.5.4）
├── board/            # 自研板 MODOU（sdkconfig：16MB + 双 OTA 分区 + 回滚保护）
├── modules/          # C 模块：st7789（屏）/ ota（OTA+回滚）/ dsp（ZL38063 HBI 启动）
├── partitions-16mb-ota.csv
├── py/               # 设备端程序（boot.py / 屏 / 音频 / OTA 网页）
└── user_modules.cmake

esphome/              # ESPHome 语音助手固件
├── modou.yaml
├── components/aw9106/  # 自研背光组件（LCD 电源/背光挂在 AW9106 上）
└── secrets.yaml
```

## 在线编译（GitHub Actions，手动触发）

| 工作流 | 产物 | 用途 |
|---|---|---|
| build mpy modou ota | mpy_1.29_MODOU_OTA_16MB（4 个 bin） | firmware.bin 首烧（0x1000）；micropython.bin 网页 OTA |
| build esphome modou | esphome_modou_factory.bin | USB 首烧；app bin 走 ESPHome OTA |

## 微信公众号

## 微信公众号

业余时间搞硬件、写固件，相关的折腾笔记都发在公众号里，欢迎扫码关注交流：

<p align="center">
  <img src="pysnqr.jpg" width="220" alt="微信公众号二维码">
</p>
