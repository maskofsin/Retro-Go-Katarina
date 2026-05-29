#include "shared.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <oswan.h>

#define WS_WIDTH       224
#define WS_HEIGHT      144
#define WS_FPS         75
#define WS_AUDIO_RATE  30000

#define WS_A      0x8000
#define WS_B      0x4000
#define WS_START  0x2000
#define WS_OPTION 0x1000
#define WS_X4     0x0800
#define WS_X3     0x0400
#define WS_X2     0x0200
#define WS_X1     0x0100
#define WS_Y4     0x0080
#define WS_Y3     0x0040
#define WS_Y2     0x0020
#define WS_Y1     0x0010

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_audio_sample_t *audioBuffer;
static int16_t wsAudioScratch[2][SND_RNGSIZE];
static uint8_t *romData;
static size_t romSize;
static bool drawFrame = true;
static bool slowFrame = false;
static int skipFrames = 0;
static int saveFrameCounter = 0;

extern int FrameSkip;
extern int RAMSize;
extern int RAMBanks;
extern unsigned char **RAMMap;

static const char *SETTING_AUDIO = "audio";
static const char *SETTING_DPAD_MODE = "dpadMode";
static const char *SETTING_WS_MODIFIER = "wsModifier";
static bool audioEnabled = true;
static int dpadMode = 0;
static int wsModifier = 0;

enum {
    WS_DPAD_AUTO = 0,
    WS_DPAD_Y,
    WS_DPAD_X,
    WS_DPAD_COUNT,
};

static const char *WS_DPAD_NAMES[] = {
    "Auto",
    "Y buttons",
    "X buttons",
};

enum {
    WS_MOD_NONE = 0,
    WS_MOD_MENU,
    WS_MOD_OPTION,
    WS_MOD_SELECT,
    WS_MOD_START,
    WS_MOD_COUNT,
};

static const char *WS_MOD_NAMES[] = {
    "Off",
    "Menu",
    "Option",
    "Select",
    "Start",
};

static void save_sram(void);

static inline uint32_t ws_modifier_to_mask(int binding)
{
    switch (binding)
    {
        case WS_MOD_MENU: return RG_KEY_MENU;
        case WS_MOD_OPTION: return RG_KEY_OPTION;
        case WS_MOD_SELECT: return RG_KEY_SELECT;
        case WS_MOD_START: return RG_KEY_START;
        default: return 0;
    }
}

static inline void apply_ws_dpad(uint16_t *state, uint32_t joystick, bool xpad)
{
    if (xpad)
    {
        if (joystick & RG_KEY_UP)    *state |= WS_X1;
        if (joystick & RG_KEY_RIGHT) *state |= WS_X2;
        if (joystick & RG_KEY_DOWN)  *state |= WS_X3;
        if (joystick & RG_KEY_LEFT)  *state |= WS_X4;
    }
    else
    {
        if (joystick & RG_KEY_UP)    *state |= WS_Y1;
        if (joystick & RG_KEY_RIGHT) *state |= WS_Y2;
        if (joystick & RG_KEY_DOWN)  *state |= WS_Y3;
        if (joystick & RG_KEY_LEFT)  *state |= WS_Y4;
    }
}

static size_t get_file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fclose(fp);
    return size;
}

static void submit_audio(void)
{
    if (!audioEnabled || !audioBuffer)
        return;

    const size_t samples = WS_AUDIO_RATE / WS_FPS;
    size_t available = apuBufLen();
    size_t count = RG_MIN(available, (size_t)SND_RNGSIZE);

    if (count == 0)
    {
        memset(audioBuffer, 0, samples * sizeof(*audioBuffer));
        rg_audio_submit(audioBuffer, samples);
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        wsAudioScratch[0][i] = sndbuffer[0][rBuf];
        wsAudioScratch[1][i] = sndbuffer[1][rBuf];
        if (++rBuf >= SND_RNGSIZE)
            rBuf = 0;
    }

    if (count == 1)
    {
        for (size_t i = 0; i < samples; ++i)
        {
            audioBuffer[i].left = wsAudioScratch[0][0];
            audioBuffer[i].right = wsAudioScratch[1][0];
        }
    }
    else
    {
        const uint32_t step = (uint32_t)(((count - 1) << 16) / RG_MAX(samples - 1, 1));
        uint32_t pos = 0;

        for (size_t i = 0; i < samples; ++i, pos += step)
        {
            size_t idx = pos >> 16;
            uint32_t frac = pos & 0xFFFF;
            size_t next = RG_MIN(idx + 1, count - 1);
            int32_t l0 = wsAudioScratch[0][idx];
            int32_t l1 = wsAudioScratch[0][next];
            int32_t r0 = wsAudioScratch[1][idx];
            int32_t r1 = wsAudioScratch[1][next];

            audioBuffer[i].left = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
            audioBuffer[i].right = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
        }
    }

    rg_audio_submit(audioBuffer, samples);
}

static void copy_frame(uint16_t *dst)
{
    const uint16_t *src = (const uint16_t *)FrameBuffer;
    for (int y = 0; y < WS_HEIGHT; ++y)
    {
        memcpy(dst + y * WS_WIDTH, src + y * SCREEN_WIDTH, WS_WIDTH * sizeof(uint16_t));
    }
}

void ws_graphics_paint(void)
{
    if (!FrameBuffer)
        return;

    if (drawFrame)
    {
        copy_frame((uint16_t *)currentUpdate->data);
        slowFrame = !rg_display_sync(false);
        rg_display_submit(currentUpdate, 0);
        currentUpdate = updates[currentUpdate == updates[0]];
    }
}

int ws_input_poll(int mode)
{
    uint32_t joystick = rg_input_read_gamepad();
    uint16_t state = 0;
    uint32_t dpad = joystick & (RG_KEY_UP | RG_KEY_RIGHT | RG_KEY_DOWN | RG_KEY_LEFT);
    bool prefer_x = (dpadMode == WS_DPAD_AUTO) ? (mode != 0) : (dpadMode == WS_DPAD_X);
    uint32_t modifier_mask = ws_modifier_to_mask(wsModifier);
    bool inactive_cluster = modifier_mask && (joystick & modifier_mask) && dpad;

    if ((joystick & RG_KEY_MENU) && !(dpad && wsModifier == WS_MOD_MENU))
    {
        save_sram();
        rg_gui_game_menu();
    }
    else if ((joystick & RG_KEY_OPTION) && !(dpad && wsModifier == WS_MOD_OPTION))
    {
        save_sram();
        rg_gui_options_menu();
    }

    apply_ws_dpad(&state, joystick, inactive_cluster ? !prefer_x : prefer_x);

    if (joystick & RG_KEY_A) state |= WS_A;
    if (joystick & RG_KEY_B) state |= WS_B;
    if (joystick & RG_KEY_START) state |= WS_START;
    if (joystick & RG_KEY_SELECT) state |= WS_OPTION;

    return state;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        rg_display_submit(updates[currentUpdate == updates[0]], 0);
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(updates[currentUpdate == updates[0]], filename, width, height);
}

static bool reset_handler(bool hard)
{
    WsReset();
    return true;
}

static rg_gui_event_t audio_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        audioEnabled = !audioEnabled;
        rg_settings_set_number(NS_APP, SETTING_AUDIO, audioEnabled);
    }

    strcpy(option->value, audioEnabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t dpad_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV)
        dpadMode = (dpadMode + WS_DPAD_COUNT - 1) % WS_DPAD_COUNT;
    if (event == RG_DIALOG_NEXT)
        dpadMode = (dpadMode + 1) % WS_DPAD_COUNT;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_settings_set_number(NS_APP, SETTING_DPAD_MODE, dpadMode);

    strcpy(option->value, WS_DPAD_NAMES[dpadMode]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t ws_modifier_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        int delta = (event == RG_DIALOG_PREV) ? -1 : 1;
        wsModifier = (wsModifier + WS_MOD_COUNT + delta) % WS_MOD_COUNT;
        rg_settings_set_number(NS_APP, SETTING_WS_MODIFIER, wsModifier);
    }

    strcpy(option->value, WS_MOD_NAMES[wsModifier]);
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &audio_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("D-pad maps to"), "-", RG_DIALOG_FLAG_NORMAL, &dpad_mode_cb};
    *dest++ = (rg_gui_option_t){0, _("2nd cross mod"), "-", RG_DIALOG_FLAG_NORMAL, &ws_modifier_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void load_sram(void)
{
    if (RAMBanks < 1 || RAMSize <= 0 || !RAMMap || !RAMMap[0])
        return;

    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    void *data = RAMMap[0];
    size_t size = RAMSize;
    rg_storage_read_file(path, &data, &size, RG_FILE_USER_BUFFER);
}

static void save_sram(void)
{
    if (RAMBanks < 1 || RAMSize <= 0 || !RAMMap || !RAMMap[0])
        return;

    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    rg_storage_mkdir(rg_dirname(path));
    rg_storage_write_file(path, RAMMap[0], RAMSize, RG_FILE_ATOMIC_WRITE);
}

void ws_main(void)
{
    const rg_handlers_t handlers = {
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    app = rg_system_reinit(WS_AUDIO_RATE, &handlers, NULL);
    audioEnabled = rg_settings_get_number(NS_APP, SETTING_AUDIO, 1);
    dpadMode = ((int)rg_settings_get_number(NS_APP, SETTING_DPAD_MODE, WS_DPAD_AUTO)) % WS_DPAD_COUNT;
    wsModifier = ((int)rg_settings_get_number(NS_APP, SETTING_WS_MODIFIER, WS_MOD_MENU)) % WS_MOD_COUNT;

    updates[0] = rg_surface_create(WS_WIDTH, WS_HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    updates[1] = rg_surface_create(WS_WIDTH, WS_HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    currentUpdate = updates[0];
    audioBuffer = malloc((WS_AUDIO_RATE / WS_FPS + 1) * sizeof(rg_audio_sample_t));

    WsInit();

    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, (void **)&romData, &romSize, RG_FILE_ALIGN_64KB))
            RG_PANIC("ROM file unzipping failed!");
        if (WsCreateFromMemory(romData, romSize) != 0)
            RG_PANIC("ROM loading failed!");
    }
    else
    {
        size_t rawSize = get_file_size(app->romPath);
        RG_LOGI("WS raw ROM size=%u", (unsigned)rawSize);
        if (rawSize > 0 && rawSize <= 4 * 1024 * 1024)
        {
            RG_LOGI("WS using in-memory loader");
            if (!rg_storage_read_file(app->romPath, (void **)&romData, &romSize, RG_FILE_ALIGN_64KB))
                RG_PANIC("ROM file loading failed!");
            if (WsCreateFromMemory(romData, romSize) != 0)
                RG_PANIC("ROM loading failed!");
        }
        else if (WsCreatePaged(app->romPath) != 0)
        {
            RG_PANIC("ROM file loading failed!");
        }
    }

    load_sram();

    rg_system_set_tick_rate(WS_FPS);
    app->frameskip = 0;
    FrameSkip = 0;

    while (true)
    {
        int64_t startTime = rg_system_timer();
        int internalFrameskip = RG_MIN(RG_MAX(app->frameskip, 0), 4);
        FrameSkip = internalFrameskip;
        drawFrame = (internalFrameskip > 0) ? true : (skipFrames == 0);
        slowFrame = false;

        WsRun();

        rg_system_tick(rg_system_timer() - startTime);
        submit_audio();

        if (++saveFrameCounter >= WS_FPS * 10)
        {
            save_sram();
            saveFrameCounter = 0;
        }

        if (internalFrameskip == 0 && skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (elapsed > app->frameTime + 1000)
                skipFrames = 1;
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (internalFrameskip == 0 && skipFrames > 0)
        {
            skipFrames--;
        }
        else if (internalFrameskip > 0)
        {
            skipFrames = 0;
        }
    }
}
