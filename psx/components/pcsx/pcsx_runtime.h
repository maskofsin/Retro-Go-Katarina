#pragma once

#include <stdint.h>

#include <rg_system.h>

typedef struct
{
    const char *bios_path;
    const char *disc_path;
    const char *memcard0_path;
    const char *memcard1_path;
    int sample_rate;
    int frame_rate;
} pcsx_runtime_config_t;

void pcsx_runtime_set_config(const pcsx_runtime_config_t *config);
const pcsx_runtime_config_t *pcsx_runtime_get_config(void);

void pcsx_runtime_set_last_message(const char *message);
const char *pcsx_runtime_get_last_message(void);

void pcsx_runtime_set_pad_mask(unsigned port, uint16_t mask);
uint16_t pcsx_runtime_get_pad_mask(unsigned port);

void pcsx_runtime_set_host_pad(uint32_t pad);
uint32_t pcsx_runtime_get_host_pad(void);
