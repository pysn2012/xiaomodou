#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace aw9106 {

// AW9106B 灯控/背光驱动（魔豆小车，I2C 0x5b）
// 移植自已板测的 MicroPython aw9106.py（原厂上电序列逆向自 Flower_modou）
//
// 通道：0x20=绿 0x21=红 0x22=蓝（顶部 RGB，本项目不用，仅清零）
// LCD 电源/背光：0x23/0x24（0x80 开 / 0x00 关）
// 注意：本组件 setup() 必须先于屏幕 SPI 初始化执行（依赖 esp32 板实测顺序）

class Aw9106Component : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  bool write_reg(uint8_t reg, uint8_t value);
  void backlight(bool enabled);

 protected:
  bool present_{false};
  void factory_init_();
};

// 注意：ESPHome 的 switch 命名空间是 switch_（带下划线，switch 是 C++ 关键字）
class Aw9106Backlight : public switch_::Switch, public Component {
 public:
  void set_parent(Aw9106Component *parent) { this->parent_ = parent; }
  void write_state(bool state) override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  Aw9106Component *parent_;
};

}  // namespace aw9106
}  // namespace esphome
