#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <rg_system.h>

typedef enum
{
    PSX_MEDIA_UNKNOWN = 0,
    PSX_MEDIA_CHD,
    PSX_MEDIA_CUE,
    PSX_MEDIA_PBP,
    PSX_MEDIA_M3U,
    PSX_MEDIA_TOC,
    PSX_MEDIA_CCD,
    PSX_MEDIA_MDS,
    PSX_MEDIA_MDF,
    PSX_MEDIA_CBIN,
    PSX_MEDIA_ECM,
    PSX_MEDIA_ISO,
    PSX_MEDIA_BIN,
    PSX_MEDIA_IMG,
} psx_media_type_t;

typedef struct
{
    psx_media_type_t type;
    size_t size;
    bool supported;
    bool needs_companion_files;
    const char *type_name;
} psx_disc_info_t;

typedef struct
{
    bool present;
    const char *path;
} psx_bios_info_t;

typedef struct
{
    const char *rom_path;
    const char *bios_path;
    const char *memcard0_path;
    const char *memcard1_path;
    int sample_rate;
    int frame_rate;
} psx_boot_config_t;

bool psx_core_find_bios(psx_bios_info_t *out_info);
bool psx_core_probe_disc(const char *path, psx_disc_info_t *out_info);
bool psx_core_prepare_boot(const rg_app_t *app, psx_boot_config_t *out_cfg, psx_bios_info_t *out_bios, psx_disc_info_t *out_disc);
bool psx_core_init(const psx_boot_config_t *cfg);
bool psx_core_load_game(const psx_disc_info_t *disc);
void psx_core_shutdown(void);
const char *psx_core_get_last_error(void);
