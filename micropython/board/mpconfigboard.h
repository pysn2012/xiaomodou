// Modou（魔豆小车）板配置
// 主控：ESP32-D0WD（经典款），16MB Flash，8MB SPI PSRAM
// 硬件参考：https://github.com/FlowerCat-chen/Flower_modou

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "MoDu Car"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME "ESP32-D0WD"
#endif

// ------------------------------------------------------------------
// 关闭 UART0 REPL（关键！）：
//   UART0 (TX=GPIO1/RX=GPIO3) 被顶针物理占用，专用于和底座小车通信。
//   若保留 UART REPL，底座发来的每个字节都会经 UART IRQ 进 stdin_ringbuf，
//   而 mp_hal_stdin_rx_chr() 永远先读它再读 WebREPL dupterm —— 后果：
//   底座一接上，AT+StartUp 持续灌进 REPL 输入，Thonny/WebREPL 全部被冲垮。
//   这是 C 层行为，Python 无法绕过，只能在固件层关闭。
//   关闭后：REPL 只走 WebREPL(dupterm)；底座 UART0 用 machine.UART(0) 独占。
// ------------------------------------------------------------------
#define MICROPY_HW_ENABLE_UART_REPL (0)
