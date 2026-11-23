#ifndef MODOU_DSP_FIRMWARE_H
#define MODOU_DSP_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODOU_DSP_FW_BLOCK_SIZE 0x2c

extern const uint8_t modou_dsp_fw_blocks[];
extern const size_t modou_dsp_fw_block_count;
extern const uint32_t modou_dsp_fw_program_base;
extern const uint32_t modou_dsp_fw_exec_addr;
extern const uint8_t modou_dsp_fw_flag;
extern const uint16_t modou_dsp_cfg_words[];
extern const size_t modou_dsp_cfg_pair_count;

#ifdef __cplusplus
}
#endif

#endif
