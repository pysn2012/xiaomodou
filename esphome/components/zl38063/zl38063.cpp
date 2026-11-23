#include "zl38063.h"
#include "esphome/core/log.h"

namespace esphome {
namespace zl38063 {

static const char *const TAG = "zl38063";

// ---- HBI 常量（与 modou_dsp.c 一致，勿改）----
static constexpr uint16_t HBI_STATUS_REG = 0x0000;
static constexpr uint16_t HBI_KEY_RAW_REG = 0x0002;
static constexpr uint16_t HBI_DSP_CONTROL_REG = 0x0006;
static constexpr uint16_t HBI_TARGET_ADDR_REG = 0x000c;
static constexpr uint16_t HBI_BOOT_MODE_REG = 0x0014;
static constexpr uint16_t HBI_CMD_REG = 0x0032;
static constexpr uint16_t HBI_RESULT_REG = 0x0034;
static constexpr uint16_t HBI_EXEC_ADDR_REG = 0x012c;
static constexpr uint16_t HBI_INIT_WORD = 0xfd80;
static constexpr uint16_t HBI_DSP_START_WORD = 0x0002;
static constexpr uint16_t HBI_BOOT_ENABLE_WORD = 0x0001;
static constexpr uint16_t HBI_BOOT_READY_WORD = 0xd3d3;
static constexpr uint16_t HBI_READ_DUMMY_WORD = 0x0bad;
static constexpr uint16_t HBI_READ_FLAG = 0x8000;
static constexpr uint16_t HBI_PAGE_SELECT_BASE = 0xfe00;
static constexpr uint16_t HBI_WRITE_FLAG = 0x8080;

static constexpr uint16_t HBI_FACTORY_POST_REG_0 = 0x04c0;
static constexpr uint16_t HBI_FACTORY_POST_REG_1 = 0x0202;
static constexpr uint16_t HBI_FACTORY_POST_REG_2 = 0x022a;
static constexpr uint16_t HBI_FACTORY_POST_VAL_0 = 0x0022;
static constexpr uint16_t HBI_FACTORY_POST_VAL_1 = 0x1c0f;
static constexpr uint16_t HBI_FACTORY_POST_VAL_2 = 0x0000;

static uint16_t hbi_swap16(uint16_t value) {
  return (uint16_t) (((value & 0x00ffU) << 8) | (value >> 8));
}

static uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// ---- HBI 原语 ----
// 线上字节序：每个 16-bit 字按 big-endian 发送（等价 IDF 版 memcpy(swap16(wire)) 的事务），
// CS 由 SPIDevice 的 enable()/disable() 在每次事务前后拉低/拉高，与 IDF 版逐事务 CS 一致

void Zl38063Component::write_word_(uint16_t word) {
  const uint16_t wire = hbi_swap16(word);
  const uint8_t buf[2] = {(uint8_t) (wire & 0xff), (uint8_t) (wire >> 8)};
  this->write_array(buf, 2);
}

uint16_t Zl38063Component::read_word_(bool *ok) {
  // 读事务：发 dummy 0x0bad 的线上字节序，同时收 16-bit
  const uint16_t wire = hbi_swap16(HBI_READ_DUMMY_WORD);
  uint8_t buf[2] = {(uint8_t) (wire & 0xff), (uint8_t) (wire >> 8)};
  this->transfer_array(buf, 2);
  if (ok != nullptr) {
    *ok = true;
  }
  // 收到的两个字节按 [低, 高] 组装再交换回逻辑序（等价 IDF 版 memcpy+swap16）
  return hbi_swap16((uint16_t) (buf[0] | ((uint16_t) buf[1] << 8)));
}

bool Zl38063Component::prepare_access_(uint16_t addr, uint8_t words, bool is_read) {
  if (words == 0 || words > 126) {
    return false;
  }
  const uint8_t page = (uint8_t) (addr >> 8);
  const uint8_t word_count_minus_1 = (uint8_t) (words - 1U);
  const uint16_t word_addr = (uint16_t) ((addr >> 1) & 0x007fU);
  uint16_t cmd;

  if (page == 0) {
    cmd = (uint16_t) ((word_addr << 8) | word_count_minus_1);
    cmd |= is_read ? HBI_READ_FLAG : HBI_WRITE_FLAG;
  } else {
    uint8_t selected_page = page;
    if (selected_page != 0xff) {
      selected_page = (uint8_t) (selected_page - 1U);
    }
    this->write_word_((uint16_t) (HBI_PAGE_SELECT_BASE | selected_page));
    cmd = (uint16_t) ((word_addr << 8) | word_count_minus_1);
    if (!is_read) {
      cmd |= 0x0080;
    }
  }
  this->write_word_(cmd);
  return true;
}

bool Zl38063Component::hbi_read_(uint16_t addr, uint8_t words, uint16_t *out) {
  if (!this->prepare_access_(addr, words, true)) {
    return false;
  }
  for (uint8_t i = 0; i < words; ++i) {
    bool ok = false;
    out[i] = this->read_word_(&ok);
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool Zl38063Component::hbi_write_(uint16_t addr, uint8_t words, const uint16_t *in) {
  if (!this->prepare_access_(addr, words, false)) {
    return false;
  }
  for (uint8_t i = 0; i < words; ++i) {
    this->write_word_(in[i]);
  }
  return true;
}

// ---- 启动流程 ----
bool Zl38063Component::wait_cmd_idle_() {
  uint16_t cmd = 0x0bad;
  for (int tries = 0; tries < 1000; ++tries) {
    if (!this->hbi_read_(HBI_CMD_REG, 1, &cmd)) {
      return false;
    }
    if (cmd == 0) {
      return true;
    }
    delay(10);
  }
  ESP_LOGE(TAG, "command wait timeout: cmd_reg=0x%04x", cmd);
  return false;
}

bool Zl38063Component::command_(uint16_t cmd) {
  uint16_t control = 0x0bad;
  for (int tries = 0; tries < 1000; ++tries) {
    if (!this->hbi_read_(HBI_DSP_CONTROL_REG, 1, &control)) {
      return false;
    }
    if ((control & 0x0001U) == 0) {
      break;
    }
    delay(10);
    if (tries == 999) {
      ESP_LOGE(TAG, "control busy timeout: ctrl=0x%04x cmd=0x%04x", control, cmd);
      return false;
    }
  }
  if (!this->hbi_write_(HBI_CMD_REG, 1, &cmd)) {
    return false;
  }
  control = (cmd == 0x8005U) ? 0x0004U : 0x0001U;
  if (!this->hbi_write_(HBI_DSP_CONTROL_REG, 1, &control)) {
    return false;
  }
  return this->wait_cmd_idle_();
}

bool Zl38063Component::check_result_zero_() {
  uint16_t result = 0;
  if (!this->hbi_read_(HBI_RESULT_REG, 1, &result)) {
    return false;
  }
  if (result != 0) {
    ESP_LOGE(TAG, "result register not zero: 0x%04x", result);
    return false;
  }
  return true;
}

bool Zl38063Component::load_firmware_() {
  // ---- 链路探针 1：INIT 后连读 STATUS/KEY_RAW，判断 HBI 读路径是否活着 ----
  uint16_t probe = 0;
  for (int i = 0; i < 3; ++i) {
    if (this->hbi_read_(HBI_STATUS_REG, 1, &probe)) {
      ESP_LOGI(TAG, "probe status[%d]=0x%04x", i, probe);
    } else {
      ESP_LOGE(TAG, "probe status[%d] read failed", i);
    }
  }
  if (this->hbi_read_(HBI_KEY_RAW_REG, 1, &probe)) {
    ESP_LOGI(TAG, "probe key_raw=0x%04x", probe);
  }

  const uint16_t one = HBI_BOOT_ENABLE_WORD;
  if (!this->hbi_write_(HBI_BOOT_MODE_REG, 1, &one)) {
    ESP_LOGE(TAG, "boot_mode write failed (HBI dead)");
    return false;
  }

  // ---- 链路探针 2：boot enable 后轮询 RESULT，0xd3d3 出现时间与中间值全记录 ----
  bool ready = false;
  for (int i = 0; i < 10; ++i) {
    delay(50);
    if (!this->hbi_read_(HBI_RESULT_REG, 1, &this->boot_ready_)) {
      ESP_LOGE(TAG, "probe result[%d] read failed", i);
      return false;
    }
    ESP_LOGI(TAG, "probe result[%d] t=%dms = 0x%04x", i, (i + 1) * 50, this->boot_ready_);
    if (this->boot_ready_ == HBI_BOOT_READY_WORD) {
      ready = true;
      break;
    }
  }
  if (!ready) {
    ESP_LOGE(TAG, "boot ready never seen 0xd3d3 (last=0x%04x)", this->boot_ready_);
    return false;
  }

  ESP_LOGI(TAG, "firmware load start: blocks=%u", (unsigned) modou_dsp_fw_block_count);
  for (size_t i = 0; i < modou_dsp_fw_block_count; ++i) {
    const uint8_t *rec = &modou_dsp_fw_blocks[i * MODOU_DSP_FW_BLOCK_SIZE];
    const uint16_t word_count = rd_le16(rec + 0x20);
    const uint32_t target = rd_le32(rec + 0x24);
    const uint8_t set_target_addr = rec[0x28];

    if (word_count == 0) {
      continue;
    }
    if (word_count > 16) {
      ESP_LOGE(TAG, "firmware block %u invalid word_count=%u", (unsigned) i, (unsigned) word_count);
      return false;
    }

    uint16_t words[16];
    for (uint16_t j = 0; j < word_count; ++j) {
      words[j] = rd_le16(rec + j * 2U);
    }

    if (set_target_addr != 0) {
      const uint16_t target_words[2] = {(uint16_t) (target >> 16), (uint16_t) (target & 0xffffU)};
      if (!this->hbi_write_(HBI_TARGET_ADDR_REG, 2, target_words)) {
        ESP_LOGE(TAG, "set target addr failed: block=%u target=0x%08lx", (unsigned) i, (unsigned long) target);
        return false;
      }
    }

    const uint16_t hbi_addr = (uint16_t) (0xff00U | (target & 0x00ffU));
    if (!this->hbi_write_(hbi_addr, (uint8_t) word_count, words)) {
      ESP_LOGE(TAG, "firmware write failed: block=%u target=0x%08lx words=%u",
               (unsigned) i, (unsigned long) target, (unsigned) word_count);
      return false;
    }

    if ((i % 1024U) == 0 || i + 1U == modou_dsp_fw_block_count) {
      ESP_LOGI(TAG, "firmware progress: %u/%u", (unsigned) (i + 1U), (unsigned) modou_dsp_fw_block_count);
    }
  }

  const uint16_t exec_words[2] = {(uint16_t) (modou_dsp_fw_exec_addr >> 16),
                                  (uint16_t) (modou_dsp_fw_exec_addr & 0xffffU)};
  if (!this->hbi_write_(HBI_EXEC_ADDR_REG, 2, exec_words)) {
    ESP_LOGE(TAG, "exec addr write failed");
    return false;
  }
  if (!this->command_(0x000d)) {
    ESP_LOGE(TAG, "cmd 0x000d (fw execute) failed");
    return false;
  }
  return this->check_result_zero_();
}

bool Zl38063Component::load_config_() {
  ESP_LOGI(TAG, "config load start: pairs=%u", (unsigned) modou_dsp_cfg_pair_count);
  for (size_t i = 0; i < modou_dsp_cfg_pair_count; ++i) {
    const uint16_t addr = modou_dsp_cfg_words[i * 2U];
    const uint16_t value = modou_dsp_cfg_words[i * 2U + 1U];
    if (!this->hbi_write_(addr, 1, &value)) {
      ESP_LOGE(TAG, "config write failed: pair=%u addr=0x%04x value=0x%04x", (unsigned) i, addr, value);
      return false;
    }
    if ((i % 256U) == 0 || i + 1U == modou_dsp_cfg_pair_count) {
      ESP_LOGI(TAG, "config progress: %u/%u", (unsigned) (i + 1U), (unsigned) modou_dsp_cfg_pair_count);
    }
  }
  return true;
}

bool Zl38063Component::apply_post_boot_() {
  static const uint16_t seq[3][2] = {
      {HBI_FACTORY_POST_REG_0, HBI_FACTORY_POST_VAL_0},
      {HBI_FACTORY_POST_REG_1, HBI_FACTORY_POST_VAL_1},
      {HBI_FACTORY_POST_REG_2, HBI_FACTORY_POST_VAL_2},
  };
  for (size_t i = 0; i < 3; ++i) {
    if (!this->hbi_write_(seq[i][0], 1, &seq[i][1])) {
      return false;
    }
  }
  return true;
}

bool Zl38063Component::start_kick_() {
  const uint16_t start_word = HBI_DSP_START_WORD;
  if (!this->hbi_write_(HBI_DSP_CONTROL_REG, 1, &start_word)) {
    return false;
  }
  delay(20);
  uint16_t status = 0;
  if (this->hbi_read_(HBI_STATUS_REG, 1, &status)) {
    ESP_LOGI(TAG, "after start kick: status=0x%04x", status);
  }
  return true;
}

void Zl38063Component::hw_reset_pulse_() {
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(false);
  delay(10);
  this->reset_pin_->digital_write(true);
  delay(500);
}

bool Zl38063Component::full_boot_() {
  bool ok = this->load_firmware_();
  if (!ok) {
    this->boot_step_ = "load_firmware";
    return false;
  }
  if (!this->command_(0x0008)) {
    this->boot_step_ = "cmd 0x0008 (fw start)";
    return false;
  }
  if (!this->load_config_()) {
    this->boot_step_ = "load_config";
    return false;
  }
  if (!this->apply_post_boot_()) {
    this->boot_step_ = "post_boot registers";
    return false;
  }
  if (!this->start_kick_()) {
    this->boot_step_ = "start_kick";
    return false;
  }
  this->boot_step_ = "OK";
  ESP_LOGI(TAG, "full boot done: OK");
  return true;
}

void Zl38063Component::attempt_boot_() {
  this->attempt_count_++;
  ESP_LOGI(TAG, "boot attempt #%u start", (unsigned) this->attempt_count_);
  this->hw_reset_pulse_();

  // HBI 初始化字 + 状态探测：INIT 后 DSP 应回 STATUS_REG（读全 0/全 F 说明 HBI 链路有问题）
  delay(2);
  this->enable();
  this->write_word_(HBI_INIT_WORD);
  this->disable();

  if (this->hbi_read_(HBI_STATUS_REG, 1, &this->init_status_)) {
    ESP_LOGI(TAG, "after INIT: status=0x%04x", this->init_status_);
  } else {
    ESP_LOGE(TAG, "after INIT: status read failed (HBI dead)");
  }

  this->boot_ok_ = this->full_boot_();
  if (this->boot_ok_) {
    ESP_LOGI(TAG, "ZL38063 boot attempt #%u OK", (unsigned) this->attempt_count_);
  } else {
    ESP_LOGE(TAG, "ZL38063 boot attempt #%u FAILED at step: %s (boot_ready=0x%04x init_status=0x%04x)",
             (unsigned) this->attempt_count_, this->boot_step_, this->boot_ready_, this->init_status_);
  }
}

void Zl38063Component::setup() {
  this->spi_setup();
  this->attempt_boot_();
  // 失败不 mark_failed：保持组件存活，loop() 里 20 秒重试（日志实时可见）
}

void Zl38063Component::loop() {
  if (this->boot_ok_) {
    return;
  }
  const uint32_t now = millis();
  if (this->last_attempt_ms_ != 0 && now - this->last_attempt_ms_ < 20000) {
    return;
  }
  this->last_attempt_ms_ = now;
  this->attempt_boot_();
}

void Zl38063Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ZL38063 DSP (HBI over SPI)");
  LOG_SPI_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Boot: %s", this->boot_ok_ ? "OK" : "FAIL");
  if (!this->boot_ok_) {
    ESP_LOGCONFIG(TAG, "  Failed step: %s", this->boot_step_);
    ESP_LOGCONFIG(TAG, "  boot_ready=0x%04x (expect 0xd3d3) init_status=0x%04x attempts=%u",
                  this->boot_ready_, this->init_status_, (unsigned) this->attempt_count_);
  }
}

}  // namespace zl38063
}  // namespace esphome
