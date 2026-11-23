#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"
#include "zl38063_firmware.h"

namespace esphome {
namespace zl38063 {

// ZL38063 DSP HBI 启动组件（魔豆小车）
// 逐行移植自已板测的 MicroPython C 模块 modou/modules/dsp/modou_dsp.c（原厂逆向自 Flower_modou）
//
// 职责（setup() 内完成，约 2~3 秒，与 LCD 共享 SPI2 总线）：
//   1. GPIO19 复位脉冲（低 10ms -> 高 -> 等 500ms）
//   2. HBI 初始化字 0xfd80（CS=GPIO0，mode 0，10MHz）
//   3. 固件下载（3699 块 x 0x2c 字节）+ cmd 0x000d 执行
//   4. cmd 0x0008 -> 配置对加载（1042 对）-> 出厂 post-boot 寄存器 -> start kick 0x0006=0x0002
//
// 注意：
//   - setup 优先级 HARDWARE：先于屏幕（600 档）执行；
//     AW9106（HARDWARE，声明在 yaml 前面）必须先跑——LCD/DSP 电源挂在它上面
//   - boot 时序来自原厂逆向，逐行对应 modou_dsp.c，勿改数值

class Zl38063Component : public Component, public spi::SPIDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  bool is_boot_ok() const { return this->boot_ok_; }

 protected:
  GPIOPin *reset_pin_{nullptr};
  bool boot_ok_{false};

  // ---- HBI 原语（16-bit 字，线上 big-endian）----
  void write_word_(uint16_t word);
  uint16_t read_word_(bool *ok = nullptr);
  bool prepare_access_(uint16_t addr, uint8_t words, bool is_read);
  bool hbi_read_(uint16_t addr, uint8_t words, uint16_t *out);
  bool hbi_write_(uint16_t addr, uint8_t words, const uint16_t *in);

  // ---- 启动流程 ----
  bool wait_cmd_idle_();
  bool command_(uint16_t cmd);
  bool check_result_zero_();
  bool load_firmware_();
  bool load_config_();
  bool apply_post_boot_();
  bool start_kick_();
  bool full_boot_();
  void hw_reset_pulse_();
};

}  // namespace zl38063
}  // namespace esphome
