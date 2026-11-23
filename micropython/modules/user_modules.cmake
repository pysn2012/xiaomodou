# 公共用户 C 模块（USER_C_MODULES 入口，跨开发板复用）
# 用法：make BOARD=<板名> USER_C_MODULES=<本文件绝对路径>
# 包含：
#   st7789  —— ST7789/ST77xx SPI 屏驱动（vendor 自 russhughes/st7789_mpy，MIT，含 JPG/PNG 解码）
#   ota     —— esp_ota_ops 封装（移植自 stamp-p4/ota_mod，芯片无关，带回滚保护）

include(${CMAKE_CURRENT_LIST_DIR}/st7789/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/ota/micropython.cmake)
