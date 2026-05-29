#include "psx_core.h"
#include "pcsx_port.h"
#include "pcsx_runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char last_error[160];
static char resolved_rom_path[RG_PATH_MAX + 32];
static char memcard0_path[RG_PATH_MAX + 32];
static char memcard1_path[RG_PATH_MAX + 32];
static psx_boot_config_t active_boot;
static psx_bios_info_t active_bios;
static psx_disc_info_t active_disc;

static void set_error(const char *message)
{
    snprintf(last_error, sizeof(last_error), "%s", message ?: "Unknown error");
}

static size_t get_file_size(const char *path)
{
    struct stat st;

    if (!path || stat(path, &st) != 0)
        return 0;

    return st.st_size > 0 ? (size_t)st.st_size : 0;
}

static bool psx_path_dirname(const char *path, char *out, size_t out_size)
{
    const char *slash;
    size_t len;

    if (!path || !out || out_size == 0)
        return false;

    slash = strrchr(path, '/');
    if (!slash)
    {
        snprintf(out, out_size, ".");
        return true;
    }

    len = slash - path;
    if (len == 0)
        len = 1;
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}


static const char *psx_path_basename(const char *path)
{
    const char *slash;

    if (!path)
        return "";

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void psx_normalize_slashes(char *path)
{
    if (!path)
        return;

    for (; *path; path++)
        if (*path == '\\')
            *path = '/';
}

static bool psx_replace_extension(const char *path, const char *ext, char *out, size_t out_size)
{
    const char *dot;
    size_t stem_len;

    if (!path || !ext || !out || out_size == 0)
        return false;

    dot = strrchr(path, '.');
    if (!dot)
        return false;

    stem_len = (size_t)(dot - path);
    if (stem_len + strlen(ext) + 1 > out_size)
        return false;

    memcpy(out, path, stem_len);
    strcpy(out + stem_len, ext);
    return true;
}

static bool psx_path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    int written;

    if (!out || out_size == 0 || !dir || !name)
        return false;

    if (dir[0] == '\0' || strcmp(dir, ".") == 0)
        written = snprintf(out, out_size, "%s", name);
    else
        written = snprintf(out, out_size, "%s/%s", dir, name);

    return written >= 0 && (size_t)written < out_size;
}

static bool psx_find_casefold_in_dir(const char *wanted_path, char *out, size_t out_size)
{
    char wanted[RG_PATH_MAX + 32];
    char dir[RG_PATH_MAX + 32];
    const char *base;
    DIR *dh;
    struct dirent *de;

    if (!wanted_path || !out || out_size == 0)
        return false;

    snprintf(wanted, sizeof(wanted), "%s", wanted_path);
    psx_normalize_slashes(wanted);

    snprintf(out, out_size, "%s", wanted);
    if (rg_storage_exists(out))
        return true;

    if (!psx_path_dirname(wanted, dir, sizeof(dir)))
        return false;

    base = psx_path_basename(wanted);
    dh = opendir(dir);
    if (!dh)
        return false;

    while ((de = readdir(dh)) != NULL)
    {
        if (strcasecmp(de->d_name, base) != 0)
            continue;
        if (psx_path_join(out, out_size, dir, de->d_name) && rg_storage_exists(out))
        {
            closedir(dh);
            return true;
        }
    }

    closedir(dh);
    return false;
}

static bool psx_resolve_sidecar_with_ext(const char *path, const char *ext, char *out, size_t out_size)
{
    static char candidate[RG_PATH_MAX + 32];

    if (!psx_replace_extension(path, ext, candidate, sizeof(candidate)))
        return false;

    return psx_find_casefold_in_dir(candidate, out, out_size);
}

static bool psx_resolve_preferred_launch_path(const char *path, psx_media_type_t type, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0)
        return false;

    /* Prefer cue sheets for raw images so CDDA/multitrack metadata is kept. */
    if (type == PSX_MEDIA_BIN || type == PSX_MEDIA_IMG || type == PSX_MEDIA_ISO)
    {
        if (psx_resolve_sidecar_with_ext(path, ".cue", out, out_size) ||
            psx_resolve_sidecar_with_ext(path, ".CUE", out, out_size))
            return true;
    }

    if (type == PSX_MEDIA_IMG)
    {
        if (psx_resolve_sidecar_with_ext(path, ".ccd", out, out_size) ||
            psx_resolve_sidecar_with_ext(path, ".CCD", out, out_size))
            return true;
    }

    if (type == PSX_MEDIA_MDF)
    {
        if (psx_resolve_sidecar_with_ext(path, ".mds", out, out_size) ||
            psx_resolve_sidecar_with_ext(path, ".MDS", out, out_size))
            return true;
    }

    snprintf(out, out_size, "%s", path);
    return rg_storage_exists(out);
}

static char *psx_trim_line(char *line)
{
    char *end;

    while (*line && isspace((unsigned char)*line))
        line++;

    if ((unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf)
        line += 3;

    end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1]))
        *--end = '\0';

    if ((line[0] == '"' && end > line + 1 && end[-1] == '"') ||
        (line[0] == '\'' && end > line + 1 && end[-1] == '\''))
    {
        line++;
        *--end = '\0';
    }

    psx_normalize_slashes(line);
    return line;
}

static bool psx_parse_cue_file_token(const char *line, char *out, size_t out_size)
{
    const char *p = line;
    size_t len = 0;

    if (!line || !out || out_size == 0)
        return false;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncasecmp(p, "FILE", 4) != 0 || !isspace((unsigned char)p[4]))
        return false;
    p += 4;
    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p == '"')
    {
        p++;
        while (p[len] && p[len] != '"' && len + 1 < out_size)
            len++;
    }
    else
    {
        while (p[len] && !isspace((unsigned char)p[len]) && len + 1 < out_size)
            len++;
    }

    if (len == 0)
        return false;

    memcpy(out, p, len);
    out[len] = '\0';
    psx_normalize_slashes(out);
    return true;
}

static bool psx_resolve_cue_entry_path(const char *cue_path, const char *entry, char *out, size_t out_size)
{
    char dir[RG_PATH_MAX + 32];
    const char *base;

    if (!cue_path || !entry || !out || out_size == 0)
        return false;

    if (entry[0] == '/')
    {
        snprintf(out, out_size, "%s", entry);
        psx_normalize_slashes(out);
        if (psx_find_casefold_in_dir(out, out, out_size))
            return true;
    }
    else if (psx_path_dirname(cue_path, dir, sizeof(dir)) &&
             psx_path_join(out, out_size, dir, entry) &&
             psx_find_casefold_in_dir(out, out, out_size))
    {
        return true;
    }

    base = strrchr(entry, '/');
    if (base && base[1] && psx_path_dirname(cue_path, dir, sizeof(dir)) &&
        psx_path_join(out, out_size, dir, base + 1) &&
        psx_find_casefold_in_dir(out, out, out_size))
        return true;

    return false;
}

static bool psx_validate_cue_file(const char *cue_path, char *resolved_first_file, size_t resolved_size)
{
    static char line[RG_PATH_MAX + 64];
    static char entry[RG_PATH_MAX + 32];
    static char resolved[RG_PATH_MAX + 32];
    bool saw_file = false;
    FILE *fp;

    if (!cue_path || !cue_path[0])
    {
        set_error("No CUE file selected.");
        return false;
    }

    fp = fopen(cue_path, "r");
    if (!fp)
    {
        snprintf(last_error, sizeof(last_error), "Unable to open CUE file: %s", strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), fp))
    {
        if (!psx_parse_cue_file_token(line, entry, sizeof(entry)))
            continue;

        saw_file = true;
        if (!psx_resolve_cue_entry_path(cue_path, entry, resolved, sizeof(resolved)))
        {
            fclose(fp);
            snprintf(last_error, sizeof(last_error), "CUE references missing BIN: %.120s", psx_path_basename(entry));
            return false;
        }

        if (resolved_first_file && resolved_size > 0 && !resolved_first_file[0])
            snprintf(resolved_first_file, resolved_size, "%s", resolved);
    }

    fclose(fp);

    if (!saw_file)
    {
        set_error("CUE file did not contain any FILE entries.");
        return false;
    }

    return true;
}

static bool psx_resolve_m3u_first_disc(const char *m3u_path, char *out, size_t out_size)
{
    static char line[RG_PATH_MAX + 32];
    char dir[RG_PATH_MAX + 32];
    char candidate[RG_PATH_MAX + 32];
    FILE *fp;

    if (!m3u_path || !out || out_size == 0)
        return false;

    fp = fopen(m3u_path, "r");
    if (!fp)
    {
        snprintf(last_error, sizeof(last_error), "Unable to open M3U playlist: %s", strerror(errno));
        return false;
    }

    psx_path_dirname(m3u_path, dir, sizeof(dir));

    while (fgets(line, sizeof(line), fp))
    {
        char *entry = psx_trim_line(line);

        if (!entry[0] || entry[0] == '#')
            continue;

        if (entry[0] == '/')
        {
            if (snprintf(candidate, sizeof(candidate), "%s", entry) >= (int)sizeof(candidate))
            {
                RG_LOGW("M3U entry too long, skipping.");
                continue;
            }
        }
        else if (!psx_path_join(candidate, sizeof(candidate), dir, entry))
        {
            RG_LOGW("M3U entry too long, skipping.");
            continue;
        }
        psx_normalize_slashes(candidate);

        if (psx_find_casefold_in_dir(candidate, out, out_size))
        {
            fclose(fp);
            return true;
        }

        RG_LOGW("M3U entry not readable, skipping: %s", candidate);
    }

    fclose(fp);
    return false;
}

static psx_media_type_t get_media_type(const char *path, const char **type_name)
{
    const char *ext = strrchr(path ?: "", '.');
    ext = ext ? ext + 1 : "";
    RG_LOGI("PSX disc path='%s' ext='%s'", path ?: "(null)", ext);

    if (strcasecmp(ext, "chd") == 0)
    {
        *type_name = "CHD";
        return PSX_MEDIA_CHD;
    }
    if (strcasecmp(ext, "cue") == 0)
    {
        *type_name = "BIN/CUE";
        return PSX_MEDIA_CUE;
    }
    if (strcasecmp(ext, "pbp") == 0)
    {
        *type_name = "PBP";
        return PSX_MEDIA_PBP;
    }
    if (strcasecmp(ext, "m3u") == 0)
    {
        *type_name = "M3U";
        return PSX_MEDIA_M3U;
    }
    if (strcasecmp(ext, "toc") == 0)
    {
        *type_name = "TOC";
        return PSX_MEDIA_TOC;
    }
    if (strcasecmp(ext, "ccd") == 0)
    {
        *type_name = "CCD";
        return PSX_MEDIA_CCD;
    }
    if (strcasecmp(ext, "mds") == 0)
    {
        *type_name = "MDS";
        return PSX_MEDIA_MDS;
    }
    if (strcasecmp(ext, "mdf") == 0)
    {
        *type_name = "MDF";
        return PSX_MEDIA_MDF;
    }
    if (strcasecmp(ext, "cbn") == 0 || strcasecmp(ext, "cbin") == 0)
    {
        *type_name = "CBIN";
        return PSX_MEDIA_CBIN;
    }
    if (strcasecmp(ext, "ecm") == 0)
    {
        *type_name = "ECM";
        return PSX_MEDIA_ECM;
    }
    if (strcasecmp(ext, "iso") == 0)
    {
        *type_name = "ISO";
        return PSX_MEDIA_ISO;
    }
    if (strcasecmp(ext, "bin") == 0)
    {
        *type_name = "BIN";
        return PSX_MEDIA_BIN;
    }
    if (strcasecmp(ext, "img") == 0)
    {
        *type_name = "IMG";
        return PSX_MEDIA_IMG;
    }

    *type_name = "Unknown";
    return PSX_MEDIA_UNKNOWN;
}

bool psx_core_find_bios(psx_bios_info_t *out_info)
{
    static const char *bios_candidates[] = {
        RG_BASE_PATH_BIOS "/scph5500.bin",
        RG_BASE_PATH_BIOS "/scph5501.bin",
        RG_BASE_PATH_BIOS "/scph5502.bin",
        RG_BASE_PATH_BIOS "/scph1001.bin",
        RG_BASE_PATH_BIOS "/psxonpsp660.bin",
        RG_BASE_PATH_BIOS "/scph101.bin",
        RG_BASE_PATH_BIOS "/SCPH5500.BIN",
        RG_BASE_PATH_BIOS "/SCPH5501.BIN",
        RG_BASE_PATH_BIOS "/SCPH5502.BIN",
        RG_BASE_PATH_BIOS "/SCPH1001.BIN",
        RG_BASE_PATH_BIOS "/PSXONPSP660.BIN",
    };

    memset(out_info, 0, sizeof(*out_info));

    for (size_t i = 0; i < RG_COUNT(bios_candidates); ++i)
    {
        if (rg_storage_exists(bios_candidates[i]))
        {
            out_info->present = true;
            out_info->path = bios_candidates[i];
            return true;
        }
    }

    set_error("No PS1 BIOS found. Add scph5501.bin, scph5502.bin, scph5500.bin, scph1001.bin, or psxonpsp660.bin to /sd/retro-go/bios/.");
    return false;
}

bool psx_core_probe_disc(const char *path, psx_disc_info_t *out_info)
{
    if (!out_info)
        return false;

    memset(out_info, 0, sizeof(*out_info));
    out_info->size = get_file_size(path);
    out_info->type = get_media_type(path, &out_info->type_name);
    out_info->supported = true;

    if (!path || !path[0])
    {
        set_error("No PS1 image path was provided.");
        return false;
    }

    if (!rg_storage_exists(path))
    {
        snprintf(last_error, sizeof(last_error), "PS1 image not found: %s", path);
        return false;
    }

    switch (out_info->type)
    {
        case PSX_MEDIA_CUE:
            if (!psx_validate_cue_file(path, NULL, 0))
                return false;
            break;

        case PSX_MEDIA_CHD:
        case PSX_MEDIA_PBP:
        case PSX_MEDIA_M3U:
        case PSX_MEDIA_TOC:
        case PSX_MEDIA_CCD:
        case PSX_MEDIA_MDS:
        case PSX_MEDIA_MDF:
        case PSX_MEDIA_CBIN:
        case PSX_MEDIA_ISO:
        case PSX_MEDIA_BIN:
        case PSX_MEDIA_IMG:
            if (out_info->size == 0)
            {
                snprintf(last_error, sizeof(last_error), "PS1 image is empty: %s", path);
                return false;
            }
            break;

        case PSX_MEDIA_ECM:
            out_info->supported = false;
            set_error("ECM-compressed PS1 images are not supported. Decompress to BIN/CUE first.");
            return false;

        default:
            out_info->supported = false;
            snprintf(last_error, sizeof(last_error), "Unsupported PS1 image extension: %s", path);
            return false;
    }

    if (out_info->type == PSX_MEDIA_BIN || out_info->type == PSX_MEDIA_IMG ||
        out_info->type == PSX_MEDIA_MDF)
        out_info->needs_companion_files = true;

    return true;
}

bool psx_core_prepare_boot(const rg_app_t *app, psx_boot_config_t *out_cfg, psx_bios_info_t *out_bios, psx_disc_info_t *out_disc)
{
    const char *boot_rom_path;

    memset(out_cfg, 0, sizeof(*out_cfg));

    if (!app || !app->romPath)
    {
        set_error("No PS1 image selected.");
        return false;
    }

    if (!psx_core_find_bios(out_bios))
        return false;

    if (!psx_core_probe_disc(app->romPath, out_disc))
        return false;

    boot_rom_path = app->romPath;
    if (out_disc->type == PSX_MEDIA_M3U)
    {
        if (!psx_resolve_m3u_first_disc(app->romPath, resolved_rom_path, sizeof(resolved_rom_path)))
        {
            set_error("M3U playlist did not contain a readable PS1 disc image.");
            return false;
        }

        RG_LOGI("Resolved M3U '%s' to '%s'", app->romPath, resolved_rom_path);
        boot_rom_path = resolved_rom_path;
        if (!psx_core_probe_disc(boot_rom_path, out_disc))
            return false;
    }

    if (!psx_resolve_preferred_launch_path(boot_rom_path, out_disc->type, resolved_rom_path, sizeof(resolved_rom_path)))
    {
        set_error("Selected PS1 image is not readable from SD card.");
        return false;
    }
    if (strcmp(boot_rom_path, resolved_rom_path) != 0)
    {
        RG_LOGI("Resolved PSX launch path '%s' to '%s'", boot_rom_path, resolved_rom_path);
        boot_rom_path = resolved_rom_path;
        if (!psx_core_probe_disc(boot_rom_path, out_disc))
            return false;
    }

    if (out_disc->type == PSX_MEDIA_CUE)
    {
        char first_file[RG_PATH_MAX + 32] = {0};
        if (!psx_validate_cue_file(boot_rom_path, first_file, sizeof(first_file)))
            return false;
        RG_LOGI("Validated CUE '%s' first data file '%s'", boot_rom_path, first_file[0] ? first_file : "(none)");
    }

    char *base_path = rg_emu_get_path(RG_PATH_SAVE_SRAM, boot_rom_path);
    if (!base_path || !base_path[0])
    {
        free(base_path);
        set_error("Could not create memory-card save path for selected PS1 image.");
        return false;
    }
    snprintf(memcard0_path, sizeof(memcard0_path), "%s.0.mcd", base_path);
    snprintf(memcard1_path, sizeof(memcard1_path), "%s.1.mcd", base_path);
    free(base_path);

    out_cfg->rom_path = boot_rom_path;
    out_cfg->bios_path = out_bios->path;
    out_cfg->memcard0_path = memcard0_path;
    out_cfg->memcard1_path = memcard1_path;
    out_cfg->sample_rate = app->sampleRate;
    out_cfg->frame_rate = app->tickRate;

    active_boot = *out_cfg;
    active_bios = *out_bios;
    active_disc = *out_disc;

    return true;
}

bool psx_core_init(const psx_boot_config_t *cfg)
{
    if (!cfg || !cfg->rom_path || !cfg->bios_path)
    {
        set_error("Incomplete PS1 boot configuration.");
        return false;
    }

    pcsx_port_probe_t probe = {0};
    pcsx_port_probe_vendor(&probe);

    if (!probe.vendor_tree_present)
    {
        set_error("PCSX ReARMed source tree not found in the vendor folder.");
        return false;
    }

    set_error("PCSX ReARMed source tree detected.");
    return true;
}

bool psx_core_load_game(const psx_disc_info_t *disc)
{
    pcsx_port_context_t ctx = {
        .boot = &active_boot,
        .disc = disc ?: &active_disc,
        .bios = &active_bios,
    };

    if (pcsx_port_boot(&ctx))
        return true;

    const char *message = pcsx_runtime_get_last_message();
    set_error(message && message[0] ? message : "PCSX ReARMed core not integrated yet.");
    return false;
}

void psx_core_shutdown(void)
{
    pcsx_port_shutdown();
}

const char *psx_core_get_last_error(void)
{
    return last_error[0] ? last_error : "No PS1 error recorded.";
}
