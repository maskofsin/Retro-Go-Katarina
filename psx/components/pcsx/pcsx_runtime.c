#include "pcsx_runtime.h"

#include <stdio.h>
#include <string.h>

static pcsx_runtime_config_t runtime_config;
static char last_message[160];
static uint16_t pad_masks[2];
static uint32_t host_pad_state;

void pcsx_runtime_set_config(const pcsx_runtime_config_t *config)
{
    memset(&runtime_config, 0, sizeof(runtime_config));
    if (config)
        runtime_config = *config;
}

const pcsx_runtime_config_t *pcsx_runtime_get_config(void)
{
    return &runtime_config;
}

void pcsx_runtime_set_last_message(const char *message)
{
    snprintf(last_message, sizeof(last_message), "%s", message ?: "");
}

const char *pcsx_runtime_get_last_message(void)
{
    return last_message;
}

void pcsx_runtime_set_pad_mask(unsigned port, uint16_t mask)
{
    if (port < RG_COUNT(pad_masks))
        pad_masks[port] = mask;
}

uint16_t pcsx_runtime_get_pad_mask(unsigned port)
{
    return port < RG_COUNT(pad_masks) ? pad_masks[port] : 0;
}

void pcsx_runtime_set_host_pad(uint32_t pad)
{
    host_pad_state = pad;
}

uint32_t pcsx_runtime_get_host_pad(void)
{
    return host_pad_state;
}
