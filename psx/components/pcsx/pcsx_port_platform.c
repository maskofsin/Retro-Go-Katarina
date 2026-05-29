#include "pcsx_runtime.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_attr.h>
#include <rg_audio.h>
#include <rg_display.h>
#include <rg_input.h>
#include <rg_surface.h>
#include <rg_system.h>

#ifdef _
#undef _
#endif

#include "revision.h"
#include "vendor/pcsx_rearmed/frontend/cspace.h"
#include "vendor/pcsx_rearmed/libpcsxcore/system.h"
#include "vendor/pcsx_rearmed/libpcsxcore/plugins.h"
#include "vendor/pcsx_rearmed/libpcsxcore/misc.h"
#include "vendor/pcsx_rearmed/libpcsxcore/r3000a.h"
#include "vendor/pcsx_rearmed/libpcsxcore/psxcommon.h"
#include "vendor/pcsx_rearmed/libpcsxcore/psxcounters.h"
#include "vendor/pcsx_rearmed/libpcsxcore/psxmem_map.h"
#include "vendor/pcsx_rearmed/libpcsxcore/sio.h"
#include "vendor/pcsx_rearmed/libpcsxcore/cdrom-async.h"
#include "vendor/pcsx_rearmed/libpcsxcore/new_dynarec/new_dynarec.h"
#include "vendor/pcsx_rearmed/frontend/main.h"
#include "vendor/pcsx_rearmed/frontend/plugin.h"
#include "vendor/pcsx_rearmed/frontend/plugin_lib.h"
#include "vendor/pcsx_rearmed/include/psemu_plugin_defs.h"
#include "vendor/pcsx_rearmed/plugins/dfsound/out.h"
#include "vendor/pcsx_rearmed/plugins/dfsound/spu_config.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifndef RG_PSX_DEFAULT_FRAMESKIP
#define RG_PSX_DEFAULT_FRAMESKIP 1
#endif
#ifndef RG_PSX_CYCLE_MULTIPLIER
#define RG_PSX_CYCLE_MULTIPLIER CYCLE_MULT_DEFAULT
#endif
#ifndef RG_PSX_SCALE_HIRES
#define RG_PSX_SCALE_HIRES 1
#endif
#ifndef RG_PSX_SPU_REVERB
#define RG_PSX_SPU_REVERB 0
#endif
#ifndef RG_PSX_SPU_INTERPOLATION
#define RG_PSX_SPU_INTERPOLATION 0
#endif
#ifndef RG_PSX_FAST_MDEC
#define RG_PSX_FAST_MDEC 1
#endif
#ifndef RG_PSX_MAX_RENDER_WIDTH
#define RG_PSX_MAX_RENDER_WIDTH 320
#endif
#ifndef RG_PSX_MAX_RENDER_HEIGHT
#define RG_PSX_MAX_RENDER_HEIGHT 240
#endif
#ifndef RG_PSX_TARGET_WIDTH
#define RG_PSX_TARGET_WIDTH RG_PSX_MAX_RENDER_WIDTH
#endif
#ifndef RG_PSX_TARGET_HEIGHT
#define RG_PSX_TARGET_HEIGHT RG_PSX_MAX_RENDER_HEIGHT
#endif
#ifndef RG_PSX_FAST_MDEC
#define RG_PSX_FAST_MDEC 1
#endif
#ifndef RG_PSX_DISABLE_AUDIO
#define RG_PSX_DISABLE_AUDIO 0
#endif

char cfgfile_basename[MAXPATHLEN];
int state_slot;

unsigned long gpuDisp;
int ready_to_go;
int g_emu_want_quit;
int g_emu_resetting;

char hud_msg[64];
int hud_new_msg;

int in_type[8] = {
    PSE_PAD_TYPE_STANDARD,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE,
};
int multitap1;
int multitap2;
int in_analog_left[8][2] = {
    {127, 127}, {127, 127}, {127, 127}, {127, 127},
    {127, 127}, {127, 127}, {127, 127}, {127, 127},
};
int in_analog_right[8][2] = {
    {127, 127}, {127, 127}, {127, 127}, {127, 127},
    {127, 127}, {127, 127}, {127, 127}, {127, 127},
};
unsigned short in_keystate[8];
int in_mouse[8][2];
int in_adev[2];
int in_adev_axis[2][2];
int in_adev_is_nublike[2];
int in_enable_vibration;
struct out_driver *out_current;

void *pl_vout_buf;
int g_layer_x;
int g_layer_y;
int g_layer_w;
int g_layer_h;
enum sched_action emu_action;
enum sched_action emu_action_old;
struct ndrc_globals ndrc_g;

static rg_surface_t *psx_updates[2];
static rg_surface_t *psx_current_update;
static rg_surface_t *psx_last_update;
static int psx_fb_width = 256;
static int psx_fb_height = 240;
static int psx_raw_width = 256;
static int psx_raw_height = 240;
static const uint8_t *psx_raw_vram;
static bool psx_frame_dirty;
static bool psx_video_ready;
static uint32_t psx_vout_flip_count;
static uint32_t psx_vout_blank_count;
static struct out_driver psx_audio_driver;

typedef void (bgr_to_fb_func)(void *dst, const void *src, int dst_pixels);

static const struct
{
    bgr_to_fb_func *blit_16;
    bgr_to_fb_func *blit_24;
} cspace_funcs = {
    .blit_16 = bgr555_to_rgb565,
    .blit_24 = bgr888_to_rgb565,
};

static int retrogo_snd_init(void)
{
    return 0;
}

static void retrogo_snd_finish(void)
{
}

static int retrogo_snd_busy(void)
{
    return 0;
}

static void IRAM_ATTR retrogo_snd_feed(void *data, int bytes)
{
    size_t frames;

    if (!data || bytes <= 0)
        return;

    frames = (size_t)bytes / sizeof(rg_audio_frame_t);
    if (frames == 0)
        return;

    rg_audio_submit((const rg_audio_frame_t *)data, frames);
}

void out_register_retrogo(struct out_driver *drv)
{
    drv->name = "retrogo";
    drv->init = retrogo_snd_init;
    drv->finish = retrogo_snd_finish;
    drv->busy = retrogo_snd_busy;
    drv->feed = retrogo_snd_feed;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
    (void)pad;
    (void)low;
    (void)high;
}

void pl_gun_byte2(int port, unsigned char byte)
{
    (void)port;
    (void)byte;
}

static int retrogo_vout_open(void)
{
    if (!psx_updates[0])
        psx_updates[0] = rg_surface_create(RG_PSX_TARGET_WIDTH, RG_PSX_TARGET_HEIGHT, RG_PIXEL_565_LE, MEM_SLOW);
    if (!psx_updates[1])
        psx_updates[1] = rg_surface_create(RG_PSX_TARGET_WIDTH, RG_PSX_TARGET_HEIGHT, RG_PIXEL_565_LE, MEM_SLOW);

    if (!psx_updates[0] || !psx_updates[1])
    {
        pcsx_runtime_set_last_message("Unable to allocate PSX video buffers.");
        return -1;
    }

    psx_current_update = psx_updates[0];
    psx_last_update = psx_updates[0];
    psx_frame_dirty = false;
    psx_vout_flip_count = 0;
    psx_vout_blank_count = 0;
    psx_video_ready = true;
    RG_LOGI("PCSX video output opened (buffers=%p,%p)", psx_updates[0], psx_updates[1]);
    return 0;
}

static void retrogo_vout_close(void)
{
    for (size_t i = 0; i < RG_COUNT(psx_updates); ++i)
    {
        if (psx_updates[i])
            rg_surface_free(psx_updates[i]);
        psx_updates[i] = NULL;
    }

    psx_current_update = NULL;
    psx_last_update = NULL;
    psx_raw_vram = NULL;
    psx_video_ready = false;
}

static inline uint16_t IRAM_ATTR psx_bgr555_to_rgb565_one(uint16_t s)
{
    uint16_t d = (uint16_t)(s << 1);
    return (uint16_t)((d & 0x07c0) | (d << 10) | (d >> 11));
}

static inline uint16_t IRAM_ATTR psx_bgr888_to_rgb565_one(const uint8_t *s)
{
    return (uint16_t)(((uint16_t)(s[0] & 0xf8) << 8) |
        ((uint16_t)(s[1] & 0xfc) << 3) | ((uint16_t)s[2] >> 3));
}

static void IRAM_ATTR psx_blit_scaled_line_16(uint8_t *dest_, const uint8_t *src_, int src_w, int dst_w)
{
    uint16_t *dest = (uint16_t *)dest_;
    const uint16_t *src = (const uint16_t *)src_;
    uint32_t step = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
    uint32_t pos = step >> 1;

    for (int x = 0; x < dst_w; ++x)
    {
        int sx = (int)(pos >> 16);
        if (sx >= src_w)
            sx = src_w - 1;
        dest[x] = psx_bgr555_to_rgb565_one(src[sx]);
        pos += step;
    }
}

static void IRAM_ATTR psx_blit_scaled_line_24(uint8_t *dest_, const uint8_t *src, int src_w, int dst_w)
{
    uint16_t *dest = (uint16_t *)dest_;
    uint32_t step = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
    uint32_t pos = step >> 1;

    for (int x = 0; x < dst_w; ++x)
    {
        int sx = (int)(pos >> 16);
        if (sx >= src_w)
            sx = src_w - 1;
        dest[x] = psx_bgr888_to_rgb565_one(src + sx * 3);
        pos += step;
    }
}

static void IRAM_ATTR psx_blit_scaled_vram_lines(uint8_t *dest, int dest_stride,
    const uint8_t *vram, int vram_ofs, uint32_t vram_mask, int src_stride,
    int src_w, int src_h, int out_w, int out_h, int bgr24)
{
    uint32_t y_step = ((uint32_t)src_h << 16) / (uint32_t)out_h;
    uint32_t y_pos = y_step >> 1;
    int last_sy = -1;
    int last_ofs = vram_ofs;

    for (int line = 0; line < out_h; ++line)
    {
        int sy = (int)(y_pos >> 16);
        int ofs;

        if (sy >= src_h)
            sy = src_h - 1;

        if (sy == last_sy)
        {
            ofs = last_ofs;
        }
        else
        {
            ofs = (vram_ofs + src_stride * sy) & (int)vram_mask;
            last_sy = sy;
            last_ofs = ofs;
        }

        if (bgr24)
            psx_blit_scaled_line_24(dest, vram + ofs, src_w, out_w);
        else
            psx_blit_scaled_line_16(dest, vram + ofs, src_w, out_w);

        y_pos += y_step;
        dest += dest_stride;
    }
}

static void retrogo_vout_set_raw_vram(void *vram)
{
    psx_raw_vram = (const uint8_t *)vram;
}

static inline int psx_vram_src_stride(int visible_height, int mode_height)
{
    return (visible_height >= mode_height * 3 / 2) ? 4096 : 2048;
}

static inline uint32_t psx_vram_mask_for_width(int width, int mode_width)
{
    return (width > mode_width) ? ~0u : 0x000fffffu;
}

static void IRAM_ATTR psx_blit_vram_lines(uint8_t *dest, int dest_stride,
    const uint8_t *vram, int vram_ofs, uint32_t vram_mask, int src_stride,
    int width, int height, bgr_to_fb_func *convert)
{
    for (int line = 0; line < height; ++line)
    {
        convert(dest, vram + vram_ofs, width);
        vram_ofs = (vram_ofs + src_stride) & (int)vram_mask;
        dest += dest_stride;
    }
}

static void IRAM_ATTR psx_fix_wrapped_vram_lines(uint8_t *dest, int dest_stride,
    const uint8_t *vram, int vram_ofs, int src_stride, int bytes_pp_s,
    int width, int height, bgr_to_fb_func *convert)
{
    int hwrapped = (vram_ofs & 2047) + width * bytes_pp_s - 2048;

    if (hwrapped <= 0)
        return;

    int tail_pixels = hwrapped / bytes_pp_s;
    if (tail_pixels <= 0 || tail_pixels >= width)
        return;

    vram_ofs = (vram_ofs - height * src_stride) & 0xff800;
    dest += (width - tail_pixels) * 2;

    for (int line = 0; line < height; ++line)
    {
        convert(dest, vram + vram_ofs, tail_pixels);
        vram_ofs = (vram_ofs + src_stride) & 0x000fffff;
        dest += dest_stride;
    }
}

static void retrogo_vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
    int prev_width = psx_fb_width;
    int prev_height = psx_fb_height;

    if (w <= 0 || h <= 0)
    {
        RG_LOGW("PCSX video requested invalid mode %dx%d, keeping %dx%d", w, h, prev_width, prev_height);
        return;
    }

    psx_fb_width = RG_MIN(w, RG_PSX_MAX_RENDER_WIDTH);
    psx_fb_height = RG_MIN(h, RG_PSX_MAX_RENDER_HEIGHT);
    psx_raw_width = raw_w > 0 ? raw_w : w;
    psx_raw_height = raw_h > 0 ? raw_h : h;

    if (w != psx_fb_width || h != psx_fb_height)
        RG_LOGI("PCSX video mode source=%dx%d raw=%dx%d bpp=%d dsx=%dx%d",
            w, h, psx_raw_width, psx_raw_height, bpp, psx_fb_width, psx_fb_height);
    else
        RG_LOGI("PCSX video mode %dx%d raw=%dx%d bpp=%d",
            psx_fb_width, psx_fb_height, psx_raw_width, psx_raw_height, bpp);

    if (psx_current_update)
    {
        psx_current_update->width = psx_fb_width;
        psx_current_update->height = psx_fb_height;
    }
}

static void IRAM_ATTR retrogo_vout_flip(const void *vram_, int vram_ofs, int bgr24,
    int x, int y, int w, int h, int dims_changed)
{
    rg_surface_t *surface = psx_current_update;
    const uint8_t *vram = vram_ ? (const uint8_t *)vram_ : psx_raw_vram;
    const int bytes_pp = 2;
    const int bytes_pp_s = bgr24 ? 3 : 2;
    int src_stride;
    uint32_t vram_mask;
    uint8_t *dest;
    uint8_t *blit_dest;
    int dest_stride;
    int draw_w;
    int draw_h;
    int src_scaled = 0;

    if (!surface || !surface->data || w <= 0 || h <= 0)
        return;

    surface->width = psx_fb_width;
    surface->height = psx_fb_height;

    dest = (uint8_t *)surface->data;
    dest_stride = surface->stride;

    if (vram == NULL || dims_changed)
        memset(dest, 0, (size_t)dest_stride * surface->height);

    if (vram == NULL)
    {
        if (psx_vout_blank_count++ < 2)
            RG_LOGI("PCSX video blank w=%d h=%d dims_changed=%d", w, h, dims_changed);
        psx_frame_dirty = true;
        return;
    }

    if (x < 0)
    {
        int skip = -x;
        if (skip >= w)
            return;
        vram_ofs += skip * bytes_pp_s;
        w -= skip;
        x = 0;
    }
    if (y < 0)
    {
        int skip = -y;
        if (skip >= h)
            return;
        vram_ofs += skip * 2048;
        h -= skip;
        y = 0;
    }

    src_stride = psx_vram_src_stride(h, psx_fb_height);
    if (src_stride == 4096)
        h /= 2;

    draw_w = RG_MIN(w, surface->width - x);
    draw_h = RG_MIN(h, surface->height - y);
    if (w > draw_w || h > draw_h)
        src_scaled = 1;

    if (x >= surface->width || y >= surface->height || draw_w <= 0 || draw_h <= 0)
        return;

    vram_mask = psx_vram_mask_for_width(w, surface->width);
    vram_ofs &= (int)vram_mask;
    blit_dest = dest + ((y * dest_stride) + (x * bytes_pp));

    if (psx_vout_flip_count++ < 10)
        RG_LOGI("PCSX video flip #%lu src=%p ofs=%d bgr24=%d xy=%d,%d wh=%dx%d draw=%dx%d stride=%d scaled=%d mask=%08lx",
            (unsigned long)psx_vout_flip_count, vram, vram_ofs, bgr24, x, y, w, h,
            draw_w, draw_h, src_stride, src_scaled, (unsigned long)vram_mask);
    else if (psx_vout_flip_count % 60 == 0)
        RG_LOGI("PCSX video flip #%lu wh=%dx%d draw=%dx%d scaled=%d",
            (unsigned long)psx_vout_flip_count, w, h, draw_w, draw_h, src_scaled);

    if (src_scaled)
    {
        psx_blit_scaled_vram_lines(blit_dest, dest_stride, vram, vram_ofs,
            vram_mask, src_stride, w, h, draw_w, draw_h, bgr24);
    }
    else
    {
        bgr_to_fb_func *convert = bgr24 ? cspace_funcs.blit_24 : cspace_funcs.blit_16;
        psx_blit_vram_lines(blit_dest, dest_stride, vram, vram_ofs, vram_mask,
            src_stride, draw_w, draw_h, convert);

        if (vram_mask == 0x000fffffu)
            psx_fix_wrapped_vram_lines(blit_dest, dest_stride, vram, vram_ofs,
                src_stride, bytes_pp_s, draw_w, draw_h, convert);
    }

    psx_frame_dirty = true;
    pl_rearmed_cbs.flip_cnt++;
}

static void *retrogo_mmap(unsigned int size)
{
    return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void retrogo_munmap(void *ptr, unsigned int size)
{
    psxUnmap(ptr, size, MAP_TAG_VRAM);
}

static void retrogo_gpu_state_change(int what, int cycles)
{
    (void)what;
    (void)cycles;
}

static void pcsx_split_path(const char *path, char *dir_out, size_t dir_size, char *name_out, size_t name_size)
{
    const char *slash;

    if (!path || !path[0])
    {
        if (dir_out && dir_size) dir_out[0] = 0;
        if (name_out && name_size) name_out[0] = 0;
        return;
    }

    slash = strrchr(path, '/');
    if (!slash)
    {
        if (dir_out && dir_size) snprintf(dir_out, dir_size, ".");
        if (name_out && name_size) snprintf(name_out, name_size, "%s", path);
        return;
    }

    if (dir_out && dir_size)
    {
        size_t len = (size_t)(slash - path);
        if (len > dir_size - 1)
            len = dir_size - 1;
        memcpy(dir_out, path, len);
        dir_out[len] = 0;
    }
    if (name_out && name_size)
        snprintf(name_out, name_size, "%s", slash + 1);
}

static void pcsx_apply_runtime_config(void)
{
    const pcsx_runtime_config_t *cfg = pcsx_runtime_get_config();
    char bios_dir[MAXPATHLEN];
    char bios_name[64];

    memset(bios_dir, 0, sizeof(bios_dir));
    memset(bios_name, 0, sizeof(bios_name));
    pcsx_split_path(cfg->bios_path, bios_dir, sizeof(bios_dir), bios_name, sizeof(bios_name));

    snprintf(Config.Gpu, sizeof(Config.Gpu), "builtin_gpu");
    snprintf(Config.Spu, sizeof(Config.Spu), "builtin_spu");
    snprintf(Config.BiosDir, sizeof(Config.BiosDir), "%s", bios_dir[0] ? bios_dir : RG_BASE_PATH_BIOS);
    snprintf(Config.Mcd1, sizeof(Config.Mcd1), "%s", cfg->memcard0_path ?: "none");
    snprintf(Config.Mcd2, sizeof(Config.Mcd2), "%s", cfg->memcard1_path ?: "none");
    snprintf(Config.Bios[0], sizeof(Config.Bios[0]), "%s", bios_name[0] ? bios_name : "HLE");
    snprintf(Config.Bios[1], sizeof(Config.Bios[1]), "%s", Config.Bios[0]);
    snprintf(Config.Bios[2], sizeof(Config.Bios[2]), "%s", Config.Bios[0]);

    Config.HLE = strcmp(Config.Bios[0], "HLE") == 0;
    Config.SlowBoot = 0;
#ifdef JIT_XTENSA
    Config.Cpu = CPU_DYNAREC;
    RG_LOGI("PCSX using JIT");
#else
    Config.Cpu = CPU_INTERPRETER;
    RG_LOGI("PCSX using interpreter");
#endif
    /* In this PCSX tree these options are counter-intuitive: Xa=1 and
     * Cdda=1 disable their audio paths, which is useful on ESP32-S3.
     * Mdec=1 selects the fast low-quality B/W MDEC path; set via
     * RG_PSX_FAST_MDEC at compile time for speed on ESP32-S3. */
    Config.Xa = 1;
    Config.Cdda = 1;
    Config.Mdec = RG_PSX_FAST_MDEC;
    Config.PsxAuto = 1;
    Config.PsxType = PSX_TYPE_NTSC;
    Config.PsxRegion = PSX_REGION_US;
    Config.cycle_multiplier = RG_PSX_CYCLE_MULTIPLIER;
    Config.DisableStalls = 1;
    Config.PreciseExceptions = 0;
    Config.icache_emulation = 1;
    Config.GpuListWalking = -1;
    Config.FractionalFramerate = -1;
    Config.AlternativeFlip = -1;
}

static void pcsx_logv(const char *prefix, const char *fmt, va_list ap)
{
    char buffer[256];
    size_t len;

    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
        buffer[--len] = 0;

    RG_LOGI("%s%s", prefix, buffer);
    pcsx_runtime_set_last_message(buffer);
}

int SysInit(void)
{
    RG_LOGI("PCSX SysInit");
    return 0;
}

void SysReset(void)
{
    RG_LOGI("PCSX SysReset");
    EmuReset();
}

void SysPrintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pcsx_logv("PCSX: ", fmt, ap);
    va_end(ap);
}

void SysMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pcsx_logv("PCSX message: ", fmt, ap);
    va_end(ap);
}

void *SysLoadLibrary(const char *lib)
{
    const char *name = lib ? strrchr(lib, '/') : NULL;
    name = name ? name + 1 : lib;

    RG_LOGI("PCSX plugin request: %s", lib ?: "(null)");
    if (name && strcmp(name, "builtin_gpu") == 0)
        return (void *)(uintptr_t)(PLUGIN_DL_BASE + PLUGIN_GPU);
    if (name && strcmp(name, "builtin_spu") == 0)
        return (void *)(uintptr_t)(PLUGIN_DL_BASE + PLUGIN_SPU);

    RG_LOGW("PCSX requested dynamic library '%s' but only built-in plugins are available.", lib ?: "(null)");
    return NULL;
}

void *SysLoadSym(void *lib, const char *sym)
{
    unsigned int plugid = (unsigned int)(uintptr_t)lib;

    if (PLUGIN_DL_BASE <= plugid && plugid <= PLUGIN_DL_BASE + PLUGIN_SPU)
    {
        void *func = plugin_link((enum builtint_plugins_e)(plugid - PLUGIN_DL_BASE), sym);
        if (!func)
            RG_LOGW("PCSX built-in plugin symbol missing: %s", sym ?: "(null)");
        return func;
    }

    RG_LOGW("PCSX requested dynamic symbol '%s' but dynamic loading is disabled.", sym ?: "(null)");
    return NULL;
}

const char *SysLibError(void)
{
    return NULL;
}

void SysCloseLibrary(void *lib)
{
    (void)lib;
}

void SysRunGui(void)
{
    g_emu_want_quit = 1;
}

void SysClose(void)
{
    RG_LOGI("PCSX SysClose");
}

int emu_core_preinit(void)
{
    memset(&Config, 0, sizeof(Config));
    strcpy(cfgfile_basename, "pcsx.cfg");
    SetIsoFile(NULL);
    spu_config.iVolume = 768;
    spu_config.iXAPitch = 0;
    spu_config.iUseReverb = RG_PSX_SPU_REVERB;
    spu_config.iUseInterpolation = RG_PSX_SPU_INTERPOLATION;
    spu_config.iTempo = 1;
    spu_config.iUseThread = 0;
    pcsx_apply_runtime_config();
    pl_rearmed_cbs.pl_vout_open = retrogo_vout_open;
    pl_rearmed_cbs.pl_vout_set_mode = retrogo_vout_set_mode;
    pl_rearmed_cbs.pl_vout_flip = retrogo_vout_flip;
    pl_rearmed_cbs.pl_vout_close = retrogo_vout_close;
    pl_rearmed_cbs.pl_vout_set_raw_vram = retrogo_vout_set_raw_vram;
    pl_rearmed_cbs.mmap = retrogo_mmap;
    pl_rearmed_cbs.munmap = retrogo_munmap;
    pl_rearmed_cbs.gpu_state_change = retrogo_gpu_state_change;
    pl_rearmed_cbs.gpu_hcnt = &hSyncCount;
    pl_rearmed_cbs.gpu_frame_count = &frame_counter;
    pl_rearmed_cbs.only_16bpp = 1;
    pl_rearmed_cbs.dithering = 0;
    pl_rearmed_cbs.scale_hires = RG_PSX_SCALE_HIRES;
    pl_rearmed_cbs.frameskip = RG_PSX_DEFAULT_FRAMESKIP;
    pl_rearmed_cbs.gpu_unai.fast_lighting = 1;
    pl_rearmed_cbs.gpu_unai.ilace_force = 1;
    return 0;
    return 0;
}

int emu_core_init(void)
{
    RG_LOGI("Starting PCSX-ReARMed %s", REV);

    if (EmuInit() == -1)
    {
        SysPrintf("PSX emulator couldn't be initialized.\n");
        return -1;
    }

    LoadMcds(Config.Mcd1, Config.Mcd2);
    return 0;
}

void emu_core_ask_exit(void)
{
    g_emu_want_quit = 1;
}

void emu_set_default_config(void)
{
}

void emu_on_new_cd(int show_hud_msg_flag)
{
    (void)show_hud_msg_flag;
}

void emu_make_path(char *buf, size_t size, const char *dir, const char *fname)
{
    snprintf(buf, size, "%s%s", dir ?: "", fname ?: "");
}

void emu_make_data_path(char *buff, const char *end, int size)
{
    snprintf(buff, size, "%s", end ?: "");
}

int get_state_filename(char *buf, int size, int slot)
{
    snprintf(buf, size, "%s.state%d", cfgfile_basename, slot);
    return 0;
}

int emu_check_state(int slot)
{
    (void)slot;
    return 0;
}

int emu_save_state(int slot)
{
    (void)slot;
    return -1;
}

int emu_load_state(int slot)
{
    (void)slot;
    return -1;
}

void set_cd_image(const char *fname)
{
    RG_LOGI("PCSX cd image set to: %s exists=%d", fname ?: "(null)", fname ? rg_storage_exists(fname) : 0);
    SetIsoFile(fname);
}

static uint16_t IRAM_ATTR pcsx_map_buttons(uint32_t pad)
{
    uint16_t mask = 0;
    bool menu_held = (pad & RG_KEY_MENU) != 0;

    if (pad & RG_KEY_UP)    mask |= (1u << DKEY_UP);
    if (pad & RG_KEY_RIGHT) mask |= (1u << DKEY_RIGHT);
    if (pad & RG_KEY_DOWN)  mask |= (1u << DKEY_DOWN);
    if (pad & RG_KEY_LEFT)  mask |= (1u << DKEY_LEFT);

    if (pad & RG_KEY_A)
        mask |= (1u << DKEY_CROSS);
    if (pad & RG_KEY_B)
        mask |= (1u << DKEY_CIRCLE);

    if (pad & RG_KEY_SELECT)
        mask |= (1u << (menu_held ? DKEY_SQUARE : DKEY_SELECT));
    if (pad & RG_KEY_START)
        mask |= (1u << (menu_held ? DKEY_TRIANGLE : DKEY_START));

    return mask;
}

void pcsx_runtime_sync_input(void)
{
    static uint32_t last_pad = UINT32_MAX;
    static uint16_t last_mapped = UINT16_MAX;
    uint32_t pad = pcsx_runtime_get_host_pad();
    uint32_t raw_pad = 0;
    uint16_t mapped = pcsx_map_buttons(pad);

    if (rg_input_read_gamepad_raw(&raw_pad))
    {
        pad = raw_pad;
        mapped = pcsx_map_buttons(pad);
    }

    pcsx_runtime_set_host_pad(pad);
    pcsx_runtime_set_pad_mask(0, mapped);
    for (size_t i = 0; i < RG_COUNT(in_keystate); ++i)
        in_keystate[i] = mapped;

    if (pad != last_pad || mapped != last_mapped)
    {
        RG_LOGI("PCSX input raw=%08lx mapped=%04x U=%d R=%d D=%d L=%d A=%d B=%d SEL=%d START=%d MENU=%d",
                (unsigned long)pad, mapped,
                !!(pad & RG_KEY_UP), !!(pad & RG_KEY_RIGHT),
                !!(pad & RG_KEY_DOWN), !!(pad & RG_KEY_LEFT),
                !!(pad & RG_KEY_A), !!(pad & RG_KEY_B),
                !!(pad & RG_KEY_SELECT), !!(pad & RG_KEY_START),
                !!(pad & RG_KEY_MENU));
        last_pad = pad;
        last_mapped = mapped;
    }
}

void pl_start_watchdog(void)
{
}

void *pl_prepare_screenshot(int *w, int *h, int *bpp)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (bpp) *bpp = 16;
    return NULL;
}

void pl_init(void)
{
    memset(cfgfile_basename, 0, sizeof(cfgfile_basename));
    strcpy(cfgfile_basename, "retro-go-psx");
    pcsx_runtime_sync_input();
}

void pl_switch_dispmode(void)
{
}

void pl_force_clear(void)
{
}

void pl_timing_prepare(int is_pal)
{
    (void)is_pal;
}

void pl_frame_limit(void)
{
    pcsx_runtime_sync_input();
    psxRegs.stop++;
}

void pl_update_layer_size(int w, int h, int fw, int fh)
{
    (void)fw;
    (void)fh;
    g_layer_w = w;
    g_layer_h = h;
}

void (*pl_plat_clear)(void);
void (*pl_plat_blit)(int doffs, const void *src, int w, int h, int sstride, int bgr24);
void (*pl_plat_hud_print)(int x, int y, const char *str, int bpp);

struct rearmed_cbs pl_rearmed_cbs = {
    .only_16bpp = 1,
    .screen_w = RG_SCREEN_WIDTH,
    .screen_h = RG_SCREEN_HEIGHT,
};

static unsigned long gpu_display;

int OpenPlugins(int load_memcards)
{
    RG_LOGI("PCSX opening CD image: %s", GetIsoFile());
    int ret = cdra_open();
    if (UsingIso() && ret < 0)
    {
        SysMessage("Error opening CD-ROM plugin!");
        return -1;
    }

    ret = SPU_open();
    if (ret < 0)
    {
        SysMessage("Error opening SPU plugin!");
        if (UsingIso())
            cdra_close();
        return -1;
    }

    ret = GPU_open(&gpu_display, "PCSX", NULL);
    if (ret < 0)
    {
        SysMessage("Error opening GPU plugin!");
        SPU_close();
        if (UsingIso())
            cdra_close();
        return -1;
    }

    SPU_registerCallback(SPUirq);
    SPU_registerScheduleCb(SPUschedule);

    if (load_memcards)
        LoadMcds(Config.Mcd1, Config.Mcd2);

    return 0;
}

void ClosePlugins(void)
{
    if (GPU_close() < 0)
        SysMessage("Error closing GPU plugin!");
    cdra_close();
    if (SPU_close() < 0)
        SysMessage("Error closing SPU plugin!");
}

void SetupSound(void)
{
#if RG_PSX_DISABLE_AUDIO
    out_current = NULL;
    SPU_async = NULL;
#else
    out_register_retrogo(&psx_audio_driver);
    if (psx_audio_driver.init && psx_audio_driver.init() == 0)
        out_current = &psx_audio_driver;
    else
        out_current = NULL;
#endif
}

void RemoveSound(void)
{
    out_current = NULL;
}

bool IRAM_ATTR retrogo_psx_submit_frame(void)
{
    static uint32_t submit_count;
    static uint32_t skip_count;

    if (!psx_video_ready || !psx_current_update)
        return false;
    if (!psx_frame_dirty)
    {
        if (skip_count++ < 5)
            RG_LOGI("PCSX submit skipped #%lu (not dirty)", (unsigned long)skip_count);
        return true;
    }

    rg_display_submit(psx_current_update, 0);
    psx_last_update = psx_current_update;
    psx_current_update = (psx_current_update == psx_updates[0]) ? psx_updates[1] : psx_updates[0];
    psx_frame_dirty = false;

    if (submit_count++ < 5)
        RG_LOGI("PCSX submit #%lu buf=%p", (unsigned long)submit_count, psx_last_update);
    return true;
}

bool retrogo_psx_redraw(void)
{
    if (!psx_video_ready || !psx_last_update)
        return false;

    rg_display_submit(psx_last_update, 0);
    return true;
}
