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

// 继承写法照抄官方 adc128s102：SPIDevice 在 2026.8.2 是四参数模板（BIT_ORDER/POLARITY/PHASE/DATA_RATE），
// MODE0 = 极性低 + 相位前沿，MSB 先行，10MHz 与 yaml data_rate 一致
class Zl38063Component : public Component,
                         public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                               spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_10MHZ> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  bool is_boot_ok() const { return this->boot_ok_; }

 protected:
  GPIOPin *reset_pin_{nullptr};
  bool boot_ok_{false};
  // 诊断状态：失败步骤名 + boot ready 探测值 + INIT 后状态寄存器值
  const char *boot_step_{"not started"};
  uint16_t boot_ready_{0};    // load_firmware 第一步 RESULT_REG 读值（期望 0xd3d3）
  uint16_t init_status_{0};   // INIT 字后 STATUS_REG 读值
  uint32_t last_attempt_ms_{0};
  uint16_t attempt_count_{0};

  // ---- HBI 原语（16-bit 字，线上 big-endian）----
  void write_word_(uint16_t word);
  uint16_t read_word_(bool *ok = nullptr);
  bool prepare_access_(uint16_t addr, uint8_t words, bool is_read);
  bool hbi_read_(uint16_t addr, uint8_t words, uint16_t *out);
  bool hbi_write_(uint16_t addr, uint8_t words, const uint16_t *in);

  // ---- 启动流程 ----
  void attempt_boot_();  // 完整一次引导尝试（复位脉冲 + INIT + 固件/配置）
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
