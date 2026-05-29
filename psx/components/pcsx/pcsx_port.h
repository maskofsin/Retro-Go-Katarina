#pragma once

#include "psx_core.h"

/*
 * This header is the Retro-Go/Katarina side of the future PCSX ReARMed port.
 * Keep emulator-core glue isolated behind this boundary so we can iterate
 * on BIOS, disc, input, audio, and video wiring without spreading PSX-specific
 * assumptions across the rest of the firmware.
 */

typedef struct
{
    const psx_boot_config_t *boot;
    const psx_disc_info_t *disc;
    const psx_bios_info_t *bios;
} pcsx_port_context_t;

typedef struct
{
    bool vendor_tree_present;
    bool frontend_present;
    bool core_present;
    bool plugin_set_present;
} pcsx_port_probe_t;

bool pcsx_port_probe_vendor(pcsx_port_probe_t *out_probe);
bool pcsx_port_boot(const pcsx_port_context_t *ctx);
bool pcsx_port_run_frame(void);
void pcsx_port_shutdown(void);
void pcsx_port_set_perf_hint(bool behind_frame);
bool retrogo_psx_submit_frame(void);
bool retrogo_psx_redraw(void);
