/*
 * Modou ZL38063 DSP HBI 启动模块（MicroPython C user module）
 * 移植自 Flower_modou components/modou_drivers/modou_dsp_keys.c（IDF 版，已实机验证）
 *
 * 职责：
 *   1. GPIO19 复位脉冲（低 10ms -> 高 -> 等 500ms）
 *   2. 在 SPI2 总线上挂 DSP HBI 设备（CS=GPIO0, mode 0, 10MHz，与 LCD 共享总线）
 *   3. HBI 16-bit 字读写（线上字节序交换、页选、<=126 字/事务）
 *   4. ZL38063 固件下载 + 启动 + 配置加载 + 出厂 post-boot 寄存器 + start kick
 *   5. 按键状态轮询（GPIO38 中断计数 + HBI status/raw 寄存器）
 *
 * Python API:
 *   import dsp
 *   dsp.init()               # 复位 + 总线挂载 + 固件下载启动（幂等，耗时约 1~2 秒）
 *   dsp.status()             # 读 HBI status 寄存器（int）
 *   dsp.raw_key()            # (status, raw_code, gpio38_level) 按键原始数据
 *   dsp.info()               # dict：loader_ok / boot_status / irq_count / g38
 *   dsp.hbi_read(addr, n=1)  # 读 n 个 16-bit 字（list），调试用
 *   dsp.hbi_write(addr, v)   # 写 1 个字或 1 个 list，调试用
 *
 * 注意：
 *   - 先建屏幕（st7789 对象）再 dsp.init()，两者共享 SPI2；
 *   - dsp.init() 期间独占总线，期间不要刷屏；
 *   - AW9106 上电序列（aw9106.init）必须发生在 dsp.init() 之前。
 */

#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"

#include "dsp_firmware.h"

// ---- 硬件常量（与原厂逆向结果一致，勿改）----
#define DSP_SPI_HOST SPI2_HOST
#define DSP_SPI_CS_GPIO GPIO_NUM_0
#define DSP_SPI_HZ (10 * 1000 * 1000)
#define DSP_SPI_QUEUE_SIZE 6
#define DSP_RESET_GPIO GPIO_NUM_19
#define DSP_KEY_IRQ_GPIO GPIO_NUM_38
// LCD 侧的总线引脚：dsp 先于屏幕初始化总线时使用（与 modou_lcd.c 一致）
#define BUS_MOSI_GPIO GPIO_NUM_33
#define BUS_MISO_GPIO GPIO_NUM_34
#define BUS_SCLK_GPIO GPIO_NUM_32

#define HBI_STATUS_REG 0x0000
#define HBI_KEY_RAW_REG 0x0002
#define HBI_DSP_CONTROL_REG 0x0006
#define HBI_TARGET_ADDR_REG 0x000c
#define HBI_BOOT_MODE_REG 0x0014
#define HBI_CMD_REG 0x0032
#define HBI_RESULT_REG 0x0034
#define HBI_EXEC_ADDR_REG 0x012c
#define HBI_INIT_WORD 0xfd80
#define HBI_DSP_START_WORD 0x0002
#define HBI_BOOT_ENABLE_WORD 0x0001
#define HBI_BOOT_READY_WORD 0xd3d3
#define HBI_READ_DUMMY_WORD 0x0bad
#define HBI_READ_FLAG 0x8000
#define HBI_PAGE_SELECT_BASE 0xfe00
#define HBI_WRITE_FLAG 0x8080

#define HBI_FACTORY_POST_REG_0 0x04c0
#define HBI_FACTORY_POST_REG_1 0x0202
#define HBI_FACTORY_POST_REG_2 0x022a
#define HBI_FACTORY_POST_VAL_0 0x0022
#define HBI_FACTORY_POST_VAL_1 0x1c0f
#define HBI_FACTORY_POST_VAL_2 0x0000

static spi_device_handle_t s_hbi_spi;
static bool s_inited;
static bool s_loader_ok;
static uint16_t s_boot_status;
static volatile uint32_t s_irq_count;

static void IRAM_ATTR gpio38_isr(void *arg)
{
    (void)arg;
    ++s_irq_count;
}

static uint16_t hbi_swap16(uint16_t value)
{
    return (uint16_t)(((value & 0x00ffU) << 8) | (value >> 8));
}

static esp_err_t hbi_write_word(uint16_t word)
{
    if (s_hbi_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t wire = hbi_swap16(word);
    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
    };
    memcpy(trans.tx_data, &wire, sizeof(wire));
    return spi_device_transmit(s_hbi_spi, &trans);
}

static esp_err_t hbi_read_word(uint16_t *out_word)
{
    if (out_word == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_hbi_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t wire = hbi_swap16(HBI_READ_DUMMY_WORD);
    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .rxlength = 16,
    };
    memcpy(trans.tx_data, &wire, sizeof(wire));
    esp_err_t err = spi_device_transmit(s_hbi_spi, &trans);
    if (err == ESP_OK) {
        memcpy(&wire, trans.rx_data, sizeof(wire));
        *out_word = hbi_swap16(wire);
    }
    return err;
}

static esp_err_t hbi_prepare_access(uint16_t addr, uint8_t words, bool is_read)
{
    if (words == 0 || words > 126) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t page = (uint8_t)(addr >> 8);
    const uint8_t word_count_minus_1 = (uint8_t)(words - 1U);
    const uint16_t word_addr = (uint16_t)((addr >> 1) & 0x007fU);
    uint16_t cmd;

    if (page == 0) {
        cmd = (uint16_t)((word_addr << 8) | word_count_minus_1);
        cmd |= is_read ? HBI_READ_FLAG : HBI_WRITE_FLAG;
    } else {
        uint8_t selected_page = page;
        if (selected_page != 0xff) {
            selected_page = (uint8_t)(selected_page - 1U);
        }
        esp_err_t err = hbi_write_word((uint16_t)(HBI_PAGE_SELECT_BASE | selected_page));
        if (err != ESP_OK) {
            return err;
        }
        cmd = (uint16_t)((word_addr << 8) | word_count_minus_1);
        if (!is_read) {
            cmd |= 0x0080;
        }
    }
    return hbi_write_word(cmd);
}

static esp_err_t hbi_read(uint16_t addr, uint8_t words, uint16_t *out_words)
{
    esp_err_t err = hbi_prepare_access(addr, words, true);
    if (err != ESP_OK) {
        return err;
    }
    for (uint8_t i = 0; i < words; ++i) {
        err = hbi_read_word(&out_words[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t hbi_write(uint16_t addr, uint8_t words, const uint16_t *in_words)
{
    esp_err_t err = hbi_prepare_access(addr, words, false);
    if (err != ESP_OK) {
        return err;
    }
    for (uint8_t i = 0; i < words; ++i) {
        err = hbi_write_word(in_words[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static esp_err_t hbi_wait_cmd_idle(void)
{
    uint16_t cmd = 0x0bad;
    for (int tries = 0; tries < 1000; ++tries) {
        esp_err_t err = hbi_read(HBI_CMD_REG, 1, &cmd);
        if (err != ESP_OK) {
            return err;
        }
        if (cmd == 0) {
            return ESP_OK;
        }
        mp_hal_delay_ms(10);
    }
    mp_printf(&mp_plat_print, "[dsp] command wait timeout: cmd_reg=0x%04x\n", cmd);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t hbi_command(uint16_t cmd)
{
    uint16_t control = 0x0bad;
    esp_err_t err = ESP_OK;

    for (int tries = 0; tries < 1000; ++tries) {
        err = hbi_read(HBI_DSP_CONTROL_REG, 1, &control);
        if (err != ESP_OK) {
            return err;
        }
        if ((control & 0x0001U) == 0) {
            break;
        }
        mp_hal_delay_ms(10);
        if (tries == 999) {
            mp_printf(&mp_plat_print,
                      "[dsp] control busy timeout: ctrl=0x%04x cmd=0x%04x\n", control, cmd);
            return ESP_ERR_TIMEOUT;
        }
    }

    err = hbi_write(HBI_CMD_REG, 1, &cmd);
    if (err != ESP_OK) {
        return err;
    }

    control = (cmd == 0x8005U) ? 0x0004U : 0x0001U;
    err = hbi_write(HBI_DSP_CONTROL_REG, 1, &control);
    if (err != ESP_OK) {
        return err;
    }
    return hbi_wait_cmd_idle();
}

static esp_err_t hbi_check_result_zero(void)
{
    uint16_t result = 0;
    esp_err_t err = hbi_read(HBI_RESULT_REG, 1, &result);
    if (err != ESP_OK) {
        return err;
    }
    if (result != 0) {
        mp_printf(&mp_plat_print, "[dsp] result register not zero: 0x%04x\n", result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t hbi_load_firmware(void)
{
    const uint16_t one = HBI_BOOT_ENABLE_WORD;
    esp_err_t err = hbi_write(HBI_BOOT_MODE_REG, 1, &one);
    if (err != ESP_OK) {
        return err;
    }

    mp_hal_delay_ms(300);

    uint16_t boot_status = 0;
    err = hbi_read(HBI_RESULT_REG, 1, &boot_status);
    s_boot_status = boot_status;
    if (err != ESP_OK) {
        return err;
    }
    if (boot_status != HBI_BOOT_READY_WORD) {
        mp_printf(&mp_plat_print,
                  "[dsp] boot status unexpected: 0x%04x expected 0x%04x\n",
                  boot_status, HBI_BOOT_READY_WORD);
        return ESP_ERR_INVALID_RESPONSE;
    }

    mp_printf(&mp_plat_print, "[dsp] firmware load start: blocks=%u\n",
              (unsigned)modou_dsp_fw_block_count);

    for (size_t i = 0; i < modou_dsp_fw_block_count; ++i) {
        const uint8_t *rec = &modou_dsp_fw_blocks[i * MODOU_DSP_FW_BLOCK_SIZE];
        const uint16_t word_count = rd_le16(rec + 0x20);
        const uint32_t target = rd_le32(rec + 0x24);
        const uint8_t set_target_addr = rec[0x28];

        if (word_count == 0) {
            continue;
        }
        if (word_count > 16) {
            mp_printf(&mp_plat_print,
                      "[dsp] firmware block %u invalid word_count=%u\n",
                      (unsigned)i, (unsigned)word_count);
            return ESP_ERR_INVALID_SIZE;
        }

        uint16_t words[16];
        for (uint16_t j = 0; j < word_count; ++j) {
            words[j] = rd_le16(rec + j * 2U);
        }

        if (set_target_addr != 0) {
            const uint16_t target_words[2] = {
                (uint16_t)(target >> 16),
                (uint16_t)(target & 0xffffU),
            };
            err = hbi_write(HBI_TARGET_ADDR_REG, 2, target_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        const uint16_t hbi_addr = (uint16_t)(0xff00U | (target & 0x00ffU));
        err = hbi_write(hbi_addr, (uint8_t)word_count, words);
        if (err != ESP_OK) {
            return err;
        }

        if ((i % 1024U) == 0 || i + 1U == modou_dsp_fw_block_count) {
            mp_printf(&mp_plat_print, "[dsp] firmware progress: %u/%u\n",
                      (unsigned)(i + 1U), (unsigned)modou_dsp_fw_block_count);
        }
    }

    const uint16_t exec_words[2] = {
        (uint16_t)(modou_dsp_fw_exec_addr >> 16),
        (uint16_t)(modou_dsp_fw_exec_addr & 0xffffU),
    };
    err = hbi_write(HBI_EXEC_ADDR_REG, 2, exec_words);
    if (err != ESP_OK) {
        return err;
    }

    err = hbi_command(0x000d);
    if (err != ESP_OK) {
        return err;
    }
    return hbi_check_result_zero();
}

static esp_err_t hbi_load_config(void)
{
    mp_printf(&mp_plat_print, "[dsp] config load start: pairs=%u\n",
              (unsigned)modou_dsp_cfg_pair_count);
    for (size_t i = 0; i < modou_dsp_cfg_pair_count; ++i) {
        const uint16_t addr = modou_dsp_cfg_words[i * 2U];
        const uint16_t value = modou_dsp_cfg_words[i * 2U + 1U];
        esp_err_t err = hbi_write(addr, 1, &value);
        if (err != ESP_OK) {
            mp_printf(&mp_plat_print,
                      "[dsp] config write failed: pair=%u addr=0x%04x value=0x%04x\n",
                      (unsigned)i, addr, value);
            return err;
        }
        if ((i % 256U) == 0 || i + 1U == modou_dsp_cfg_pair_count) {
            mp_printf(&mp_plat_print, "[dsp] config progress: %u/%u\n",
                      (unsigned)(i + 1U), (unsigned)modou_dsp_cfg_pair_count);
        }
    }
    return ESP_OK;
}

static esp_err_t hbi_apply_post_boot_registers(void)
{
    static const uint16_t seq[3][2] = {
        {HBI_FACTORY_POST_REG_0, HBI_FACTORY_POST_VAL_0},
        {HBI_FACTORY_POST_REG_1, HBI_FACTORY_POST_VAL_1},
        {HBI_FACTORY_POST_REG_2, HBI_FACTORY_POST_VAL_2},
    };
    for (size_t i = 0; i < 3; ++i) {
        esp_err_t err = hbi_write(seq[i][0], 1, &seq[i][1]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t hbi_start_kick(void)
{
    const uint16_t start_word = HBI_DSP_START_WORD;
    esp_err_t err = hbi_write(HBI_DSP_CONTROL_REG, 1, &start_word);
    if (err != ESP_OK) {
        return err;
    }
    mp_hal_delay_ms(20);

    uint16_t status = 0;
    if (hbi_read(HBI_STATUS_REG, 1, &status) == ESP_OK) {
        mp_printf(&mp_plat_print, "[dsp] after start kick: g38=%d status=0x%04x\n",
                  (int)gpio_get_level(DSP_KEY_IRQ_GPIO), status);
    }
    return ESP_OK;
}

static esp_err_t dsp_full_boot(void)
{
    // 下载期间独占总线（与 LCD 刷屏互斥），固件/配置事务流长且时序敏感
    esp_err_t err = spi_device_acquire_bus(s_hbi_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    err = hbi_load_firmware();
    if (err == ESP_OK) {
        err = hbi_command(0x0008);
    }
    if (err == ESP_OK) {
        err = hbi_load_config();
    }
    if (err == ESP_OK) {
        err = hbi_apply_post_boot_registers();
    }
    if (err == ESP_OK) {
        err = hbi_start_kick();
    }

    spi_device_release_bus(s_hbi_spi);
    s_loader_ok = (err == ESP_OK);
    mp_printf(&mp_plat_print, "[dsp] full boot done: %s boot_status=0x%04x\n",
              err == ESP_OK ? "OK" : "FAIL", s_boot_status);
    return err;
}

static void dsp_drain_startup_pending(void)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint16_t status = 0;
        if (hbi_read(HBI_STATUS_REG, 1, &status) != ESP_OK) {
            return;
        }
        const int gpio38 = (int)gpio_get_level(DSP_KEY_IRQ_GPIO);
        if (status == 0 && gpio38 != 0) {
            mp_printf(&mp_plat_print, "[dsp] startup idle: g38=%d status=0x%04x\n",
                      gpio38, status);
            return;
        }
        if (status != 0) {
            uint16_t raw = 0;
            if (hbi_read(HBI_KEY_RAW_REG, 1, &raw) != ESP_OK) {
                return;
            }
            mp_printf(&mp_plat_print, "[dsp] ignore startup pending: raw=0x%04x\n", raw);
        }
        mp_hal_delay_ms(5);
    }
    mp_printf(&mp_plat_print, "[dsp] startup not idle yet; keys arm after first idle\n");
}

static esp_err_t dsp_hw_reset_pulse(void)
{
    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << DSP_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&reset_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(DSP_RESET_GPIO, 0);
    mp_hal_delay_ms(10);
    gpio_set_level(DSP_RESET_GPIO, 1);
    mp_hal_delay_ms(500);
    return ESP_OK;
}

// dsp.init()
static mp_obj_t dsp_init(void)
{
    if (s_inited) {
        mp_printf(&mp_plat_print, "[dsp] already initialized (loader_ok=%d)\n", s_loader_ok);
        return mp_obj_new_bool(s_loader_ok);
    }

    // GPIO38 输入上拉 + 下降沿中断计数（容忍服务已安装）
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << DSP_KEY_IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&int_cfg) != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("gpio38 config failed"));
    }
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_remove(DSP_KEY_IRQ_GPIO);
        if (gpio_isr_handler_add(DSP_KEY_IRQ_GPIO, gpio38_isr, NULL) == ESP_OK) {
            mp_printf(&mp_plat_print, "[dsp] gpio38 falling-edge IRQ armed\n");
        }
    }

    // DSP 复位脉冲
    esp_err_t err = dsp_hw_reset_pulse();
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("gpio19 reset pulse failed"));
    }

    // SPI 总线：可能已被 st7789/machine.SPI 初始化（容忍 ESP_ERR_INVALID_STATE）；
    // 若尚未初始化则按 LCD 引脚配置建立，保证先后顺序无关
    spi_bus_config_t bus = {
        .mosi_io_num = BUS_MOSI_GPIO,
        .miso_io_num = BUS_MISO_GPIO,
        .sclk_io_num = BUS_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    (void)spi_bus_initialize(DSP_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = DSP_SPI_HZ,
        .mode = 0,
        .spics_io_num = DSP_SPI_CS_GPIO,
        .queue_size = DSP_SPI_QUEUE_SIZE,
    };
    err = spi_bus_add_device(DSP_SPI_HOST, &dev_cfg, &s_hbi_spi);
    if (err != ESP_OK) {
        s_hbi_spi = NULL;
        s_inited = true;
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI spi_bus_add_device failed: 0x%x"), err);
    }

    mp_hal_delay_ms(2);
    err = hbi_write_word(HBI_INIT_WORD);
    if (err != ESP_OK) {
        s_inited = true;
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI init word 0xfd80 failed: 0x%x"), err);
    }
    mp_printf(&mp_plat_print, "[dsp] HBI init word OK, full boot start\n");

    err = dsp_full_boot();
    dsp_drain_startup_pending();

    s_inited = true;
    return mp_obj_new_bool(s_loader_ok);
}
static MP_DEFINE_CONST_FUN_OBJ_0(dsp_init_obj, dsp_init);

// dsp.status() -> int
static mp_obj_t dsp_status(void)
{
    uint16_t status = 0;
    esp_err_t err = hbi_read(HBI_STATUS_REG, 1, &status);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI status read failed: 0x%x"), err);
    }
    return mp_obj_new_int(status);
}
static MP_DEFINE_CONST_FUN_OBJ_0(dsp_status_obj, dsp_status);

// dsp.raw_key() -> (status, raw_code, g38_level)
static mp_obj_t dsp_raw_key(void)
{
    uint16_t status = 0;
    esp_err_t err = hbi_read(HBI_STATUS_REG, 1, &status);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI status read failed: 0x%x"), err);
    }
    const int g38 = (int)gpio_get_level(DSP_KEY_IRQ_GPIO);

    uint16_t raw = 0;
    if (status != 0 || g38 == 0) {
        // 有事件待读或中断线为低：最多重试 6 次（间隔 2ms）
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (attempt != 0) {
                mp_hal_delay_ms(2);
                if (hbi_read(HBI_STATUS_REG, 1, &status) != ESP_OK) {
                    break;
                }
            }
            if (hbi_read(HBI_KEY_RAW_REG, 1, &raw) != ESP_OK) {
                break;
            }
            if ((raw & 0x00ffU) != 0) {
                break;
            }
        }
    }
    mp_obj_t items[3] = {
        mp_obj_new_int(status),
        mp_obj_new_int(raw & 0x00ffU),
        mp_obj_new_int(g38),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(dsp_raw_key_obj, dsp_raw_key);

// dsp.info() -> dict
static mp_obj_t dsp_info(void)
{
    mp_obj_t dict = mp_obj_new_dict(4);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_loader_ok), mp_obj_new_bool(s_loader_ok));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_boot_status), mp_obj_new_int(s_boot_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_irq_count), mp_obj_new_int(s_irq_count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_g38),
                      mp_obj_new_int((int)gpio_get_level(DSP_KEY_IRQ_GPIO)));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(dsp_info_obj, dsp_info);

// dsp.hbi_read(addr, n=1) -> list[int]
static mp_obj_t dsp_hbi_read(size_t n_args, const mp_obj_t *args)
{
    const uint16_t addr = (uint16_t)mp_obj_get_int(args[0]);
    const uint8_t words = n_args > 1 ? (uint8_t)mp_obj_get_int(args[1]) : 1;
    if (words == 0 || words > 126) {
        mp_raise_ValueError(MP_ERROR_TEXT("words must be 1..126"));
    }
    uint16_t buf[126];
    esp_err_t err = hbi_read(addr, words, buf);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI read failed: 0x%x"), err);
    }
    mp_obj_t list = mp_obj_new_list(words, NULL);
    for (uint8_t i = 0; i < words; ++i) {
        mp_obj_list_store(list, MP_OBJ_NEW_SMALL_INT(i), mp_obj_new_int(buf[i]));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(dsp_hbi_read_obj, 1, 2, dsp_hbi_read);

// dsp.hbi_write(addr, value_or_list)
static mp_obj_t dsp_hbi_write(size_t n_args, const mp_obj_t *args)
{
    const uint16_t addr = (uint16_t)mp_obj_get_int(args[0]);
    uint16_t buf[126];
    uint8_t words = 1;
    buf[0] = (uint16_t)mp_obj_get_int(args[1]);

    if (n_args > 2) {
        size_t len = 0;
        mp_obj_t *items = NULL;
        mp_obj_get_array(args[2], &len, &items);
        if (len == 0 || len > 126) {
            mp_raise_ValueError(MP_ERROR_TEXT("list length must be 1..126"));
        }
        for (size_t i = 0; i < len; ++i) {
            buf[i] = (uint16_t)mp_obj_get_int(items[i]);
        }
        words = (uint8_t)len;
    }

    esp_err_t err = hbi_write(addr, words, buf);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
                          MP_ERROR_TEXT("HBI write failed: 0x%x"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(dsp_hbi_write_obj, 2, 3, dsp_hbi_write);

static const mp_rom_map_elem_t dsp_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_dsp) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&dsp_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&dsp_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_raw_key), MP_ROM_PTR(&dsp_raw_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&dsp_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_hbi_read), MP_ROM_PTR(&dsp_hbi_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_hbi_write), MP_ROM_PTR(&dsp_hbi_write_obj) },
};
static MP_DEFINE_CONST_DICT(dsp_module_globals, dsp_module_globals_table);

const mp_obj_module_t mp_module_dsp = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&dsp_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_dsp, mp_module_dsp);
