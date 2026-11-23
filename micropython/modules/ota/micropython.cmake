# Stamp-P4 OTA 用户 C 模块（USER_C_MODULES）
# 用法：make BOARD=STAMP_P4_C6 USER_C_MODULES=<本文件绝对路径>
add_library(usermod_ota INTERFACE)

target_sources(usermod_ota INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/ota_mod.c
)

target_include_directories(usermod_ota INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# IDF 组件头文件兜底（app_update/esp_app_format/esp_partition 已在端口
# IDF_COMPONENTS 链接列表内，正常情况无需此段；显式声明以防包含路径缺失）
if(DEFINED ENV{IDF_PATH})
    target_include_directories(usermod_ota INTERFACE
        $ENV{IDF_PATH}/components/app_update/include
        $ENV{IDF_PATH}/components/esp_app_format/include
        $ENV{IDF_PATH}/components/esp_partition/include
    )
endif()

target_link_libraries(usermod INTERFACE usermod_ota)

# 固件版本号：由 workflow 环境变量 OTA_VERSION 注入（每版唯一，用于 OTA 新旧区分）
if(DEFINED ENV{OTA_VERSION})
    target_compile_definitions(usermod_ota INTERFACE
        MICROPY_OTA_VERSION="$ENV{OTA_VERSION}"
    )
endif()
