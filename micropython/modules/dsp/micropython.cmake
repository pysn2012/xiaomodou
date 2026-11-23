# Modou DSP（ZL38063 HBI）用户 C 模块
# 含固件 blob（dsp_firmware.c，3699 块，移植自 Flower_modou，未改动数据）
add_library(usermod_dsp INTERFACE)

target_sources(usermod_dsp INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modou_dsp.c
    ${CMAKE_CURRENT_LIST_DIR}/dsp_firmware.c
)

target_include_directories(usermod_dsp INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_dsp)
