# Modou 板专属 USER_C_MODULES 聚合入口（build mpy modou ota 工作流使用）
# 全部模块位于本仓库 micropython/modules/ 下：st7789（屏）+ ota（OTA/回滚）+ dsp（ZL38063）
include(${CMAKE_CURRENT_LIST_DIR}/modules/st7789/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/modules/ota/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/modules/dsp/micropython.cmake)
