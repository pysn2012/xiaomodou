/*
 * 通用 MicroPython OTA 模块（完整升级 + 回滚保护）
 * 封装 ESP-IDF esp_ota_ops：begin/write/finish(=end+set_boot)/abort + info
 * （移植自 stamp-p4/ota_mod，剥离差分升级；stamp-p4/ota_mod 保持不变）
 *
 * 用法（REPL）：
 *   import ota
 *   ota.info()          # (运行分区, 更新分区, 版本, 构建日期, 构建时间, IDF版本, ELF SHA256, 镜像状态)
 *   ota.begin()         # 打开更新槽（esp_ota_begin，OTA_SIZE_UNKNOWN）
 *   ota.write(b"....")  # 流式写入
 *   ota.finish()        # esp_ota_end + esp_ota_set_boot_partition
 *   ota.abort()         # 放弃本次升级（otadata 不动，重启后仍跑旧固件）
 *   ota.mark_valid()    # 确认当前 OTA 新固件有效（回滚保护下升级后必须调用；
 *                       # 建议在设备自检通过后自动调用，如联网成功 + 服务就绪）
 *   ota.mark_invalid_reboot() # 标记当前固件无效并重启（bootloader 自动回退旧分区）
 * 然后 machine.reset() 即切换到新固件。
 *
 * 组件依赖：app_update / esp_app_format / esp_partition 均在 esp32 端口
 * 默认 IDF_COMPONENTS 链接列表内（esp32_common.cmake），无需额外链接。
 */

#include <string.h>
#include <stdio.h>
#include "py/runtime.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"

static esp_ota_handle_t s_handle = 0;
static bool s_active = false;

static const esp_partition_t *ota_get_target(void) {
    return esp_ota_get_next_update_partition(NULL);
}

// 镜像状态字符串（esp_ota_img_states_t）：升级后未确认=pending_verify，确认后=valid
static const char *img_state_str(esp_ota_img_states_t st) {
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "unknown";
    }
}

// info() -> (run_label, next_label, version, date, time, idf_ver, elf_sha256, img_state)
static mp_obj_t ota_info(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *upd = ota_get_target();
    const esp_app_desc_t *d = esp_app_get_description();

    mp_obj_t items[8];
    items[0] = mp_obj_new_str(run->label, strlen(run->label));
    items[1] = upd != NULL ? mp_obj_new_str(upd->label, strlen(upd->label)) : mp_const_none;
#ifdef MICROPY_OTA_VERSION
    // 编译期注入的版本号（workflow 传入，每版唯一）；micropython 未设 PROJECT_VER 时
    // app_desc.version 为空串，故优先用注入值
    items[2] = mp_obj_new_str(MICROPY_OTA_VERSION, strlen(MICROPY_OTA_VERSION));
#else
    items[2] = mp_obj_new_str(d->version, strlen(d->version));
#endif
    items[3] = mp_obj_new_str(d->date, strlen(d->date));
    items[4] = mp_obj_new_str(d->time, strlen(d->time));
    items[5] = mp_obj_new_str(d->idf_ver, strlen(d->idf_ver));
    // app_elf_sha256 是 32 字节原始二进制（IDF 5.5.x，非 hex 字符串），转 hex 输出
    static char sha_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(&sha_hex[i * 2], 3, "%02x", d->app_elf_sha256[i]);
    }
    items[6] = mp_obj_new_str(sha_hex, 64);
    // 运行分区镜像状态：回滚保护的行为观察口（无回滚保护的旧固件显示 undefined）
    esp_ota_img_states_t img_state;
    if (esp_ota_get_state_partition(run, &img_state) == ESP_OK) {
        items[7] = mp_obj_new_str(img_state_str(img_state),
                                  strlen(img_state_str(img_state)));
    } else {
        items[7] = mp_obj_new_str("unknown", 7);
    }
    return mp_obj_new_tuple(8, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_info_obj, ota_info);

static mp_obj_t ota_begin(void) {
    if (s_active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ota session already started"));
    }
    const esp_partition_t *upd = ota_get_target();
    if (upd == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no OTA update partition"));
    }
    esp_err_t err = esp_ota_begin(upd, OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_ota_begin failed: 0x%x"), err);
    }
    s_active = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_begin_obj, ota_begin);

static mp_obj_t ota_write(mp_obj_t buf_in) {
    if (!s_active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ota session not started"));
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len == 0) {
        return mp_const_true;
    }
    esp_err_t err = esp_ota_write(s_handle, bufinfo.buf, bufinfo.len);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_ota_write failed: 0x%x"), err);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ota_write_obj, ota_write);

// finish(): esp_ota_end（镜像校验）+ esp_ota_set_boot_partition（切换启动槽）
static mp_obj_t ota_finish(void) {
    if (!s_active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ota session not started"));
    }
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    s_active = false;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("image validation failed (corrupt)"));
    }
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_ota_end failed: 0x%x"), err);
    }
    const esp_partition_t *upd = ota_get_target();
    err = esp_ota_set_boot_partition(upd);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_ota_set_boot_partition failed: 0x%x"), err);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_finish_obj, ota_finish);

// abort(): 放弃升级，otadata 不动，重启后仍运行旧固件
static mp_obj_t ota_abort(void) {
    if (s_active) {
        esp_ota_abort(s_handle);
        s_handle = 0;
        s_active = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_abort_obj, ota_abort);

// mark_valid(): 确认当前运行的 OTA 新固件有效、取消回滚。
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 开启后，升级重启进入的新固件处于
// pending_verify 状态；确认前若复位，bootloader 判定新固件不可靠，回退旧分区。
static mp_obj_t ota_mark_valid(void) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("mark_app_valid failed: 0x%x"), err);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_mark_valid_obj, ota_mark_valid);

// mark_invalid_reboot(): 标记当前固件无效并立即重启（不会返回），
// bootloader 回退到上一个有效分区。用于应用自检失败时主动回滚。
static mp_obj_t ota_mark_invalid_reboot(void) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ota_mark_invalid_reboot_obj, ota_mark_invalid_reboot);

// ---- 差分 OTA（esp_delta_ota 组件，heatshrink 解压还原）---- 已剥离：
// 差分升级需求集中在 stamp-p4/ota_mod，本通用模块只保留完整升级 ----

static const mp_rom_map_elem_t ota_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ota) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&ota_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_begin), MP_ROM_PTR(&ota_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&ota_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_finish), MP_ROM_PTR(&ota_finish_obj) },
    { MP_ROM_QSTR(MP_QSTR_abort), MP_ROM_PTR(&ota_abort_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_valid), MP_ROM_PTR(&ota_mark_valid_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_invalid_reboot), MP_ROM_PTR(&ota_mark_invalid_reboot_obj) },
};
static MP_DEFINE_CONST_DICT(ota_module_globals, ota_module_globals_table);

const mp_obj_module_t mp_module_ota = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ota_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ota, mp_module_ota);
