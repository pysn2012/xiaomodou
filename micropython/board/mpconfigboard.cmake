# Modou（魔豆小车）板构建配置
# SDKCONFIG 叠加顺序：common(base+ble) -> spiram_esp32 -> 本板 modou（后者覆盖前者）

include(boards/mpconfigboard_esp32_common.cmake)

# 经典 ESP32 PSRAM 配置（8MB SPI PSRAM，含 IDF 5.5.x workaround）
list(APPEND SDKCONFIG_DEFAULTS boards/sdkconfig.spiram_esp32)

# 本板专属：16MB Flash + 双 OTA 乒乓分区表 + 回滚保护
list(APPEND SDKCONFIG_DEFAULTS boards/MODOU/sdkconfig.modou)
