#include "aw9106.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aw9106 {

static const char *const TAG = "aw9106";

// 原厂上电序列（key_lights 完整版，含 0x23/0x24=0x80 背光开 + 0x02=0x02 输出使能）
// 第一条写后延时 5ms，其余 1ms（MicroPython 实测参数）
static const uint8_t FACTORY_INIT[][2] = {
    {0x7F, 0x00}, {0x11, 0x10}, {0x12, 0x02}, {0x04, 0x00}, {0x02, 0x00},
    {0x13, 0x00}, {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00}, {0x24, 0x00},
    {0x24, 0x00}, {0x25, 0x00}, {0x14, 0x07}, {0x05, 0x07}, {0x17, 0x01},
    {0x18, 0x01}, {0x19, 0x01}, {0x15, 0x1B}, {0x16, 0x08}, {0x23, 0x80},
    {0x24, 0x80}, {0x02, 0x02}, {0x14, 0x00}, {0x11, 0x12},
};

bool Aw9106Component::write_reg(uint8_t reg, uint8_t value) {
  return this->write_byte(reg, value);
}

void Aw9106Component::factory_init_() {
  const size_t n = sizeof(FACTORY_INIT) / sizeof(FACTORY_INIT[0]);
  for (size_t i = 0; i < n; i++) {
    this->write_reg(FACTORY_INIT[i][0], FACTORY_INIT[i][1]);
    delay(i == 0 ? 5 : 1);  // 首条 5ms，其余 1ms
  }
}

void Aw9106Component::setup() {
  // 探测设备：写一条无害指令（回到页 0），失败说明芯片不在
  if (!this->write_reg(0x7F, 0x00)) {
    ESP_LOGE(TAG, "AW9106 not found on I2C bus, LCD power/backlight unavailable");
    this->mark_failed();
    return;
  }
  this->present_ = true;
  this->factory_init_();
  // RGB 灯（本项目不用）确保熄灭
  this->write_reg(0x20, 0x00);
  this->write_reg(0x21, 0x00);
  this->write_reg(0x22, 0x00);
  ESP_LOGI(TAG, "AW9106 ready: LCD power/backlight on (0x23/0x24=0x80)");
}

void Aw9106Component::dump_config() {
  ESP_LOGCONFIG(TAG, "AW9106 @ 0x%02X", this->address_);
  LOG_I2C_DEVICE(this);
}

void Aw9106Component::backlight(bool enabled) {
  if (!this->present_)
    return;
  const uint8_t value = enabled ? 0x80 : 0x00;
  this->write_reg(0x23, value);
  delay(1);
  this->write_reg(0x24, value);
}

void Aw9106Backlight::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  this->parent_->backlight(state);
  this->publish_state(state);
}

}  // namespace aw9106
}  // namespace esphome
