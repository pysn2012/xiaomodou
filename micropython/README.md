# Modou（魔豆小车）MicroPython 移植

目标：为魔豆小车（ESP32-D0WD / 16MB Flash / 8MB SPI PSRAM）构建 MicroPython 固件，
支持 OTA 网页升级 + WebREPL 在线编程。

硬件资料：https://github.com/FlowerCat-chen/Flower_modou （其 IDF 固件已实机验证可行）

## 目录结构

```
modules/                    # 仓库根目录公共 C 模块（跨板复用）
├── user_modules.cmake      # USER_C_MODULES 聚合入口
├── st7789/                 # ST7789 屏驱动（vendor 自 russhughes/st7789_mpy，MIT）
└── ota/                    # esp_ota_ops 封装（移植自 stamp-p4/ota_mod，带回滚保护）

modou/                      # 魔豆小车板相关（本目录）
├── board/                  # MicroPython 自研板 MODOU
│   ├── mpconfigboard.h     # 板名 / MCU 名
│   ├── mpconfigboard.cmake # SDKCONFIG 叠加：base+ble -> spiram_esp32 -> modou
│   └── sdkconfig.modou     # 16MB Flash + 双 OTA 分区表 + 回滚保护
├── modules/
│   └── dsp/                # ZL38063 DSP HBI 启动 C 模块（含固件 blob，modou 专属）
├── user_modules.cmake      # modou 构建聚合入口（公共 st7789/ota + dsp）
├── partitions-16mb-ota.csv # 双 OTA 乒乓槽 + FAT vfs 分区表（工作流复制到 ports/esp32/）
└── py/                     # 设备端 Python
    ├── boot.py             # 开机自启：Wi-Fi + WebREPL + OTA 回滚自动确认（拷到设备根目录）
    ├── aw9106.py           # AW9106 灯控/背光（必须在屏幕/DSP 初始化前调用 init）
    ├── test_screen.py      # 屏幕点亮测试（st7789 + 原厂初始化序列）
    ├── audio_test.py       # 音频录放测试（dsp.init + machine.I2S 48kHz 立体声）
    ├── ota_web.py          # 网页 OTA 升级（浏览器上传 micropython.bin）
    └── ota_update.py       # HTTP 拉取式 OTA 升级（REPL 运行）
```

## 构建方式

GitHub Actions 手动触发：`build mpy modou ota`（配方基于 `build mpy stamp-p4 ota.yml`）。

产物（只打包 bin）：
- `*_firmware.bin` — 合并镜像（bootloader+分区表+app），首烧用（经典 ESP32 引导偏移 0x1000）：
  `esptool.py --chip esp32 --port COMx --baud 460800 write_flash -z 0x1000 <OUT>_firmware.bin`
- `*_micropython.bin` — OTA 升级载荷（app image），网页升级用，**不能**用于首烧
- `*_bootloader.bin` / `*_partition-table.bin` — 分区烧录备用

## 版本记录

> 版本号在板上验证通过后才打标签；当前 = v0.1 已验证（编译通过），v0.2~v0.4 代码就绪待板测。

- v0.1 工作流 + MODOU 板目录 + st7789/ota 模块注入，Actions 编译验证通过
- v0.2/v0.3 代码就绪（待板上验证）：AW9106 驱动 + 屏幕点亮（test_screen.py）；
  ZL38063 DSP C 模块（dsp.init 固件下载启动）+ I2S 录放（audio_test.py）
- v0.4 代码就绪（待板上验证）：boot.py（Wi-Fi + WebREPL + OTA 回滚自动确认）、
  ota_web.py（网页升级）、ota_update.py（HTTP 拉取式升级）

## 板测顺序（v0.2 → v0.4）

1. 首烧 firmware.bin（0x1000）→ REPL 正常
2. `import st7789, ota, dsp` → 三模块可导入
3. test_screen.py → 屏显四色块 + RGB 灯亮
4. audio_test.py → 录放出声
5. boot.py（改 Wi-Fi 后上传）→ 重启 → WebREPL 可连
6. ota_web.py 上传 micropython.bin → 重启 → `ota.info()` 状态 valid
