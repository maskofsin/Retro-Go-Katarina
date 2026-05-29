#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/memmap.h"
#include "../components/gbsp-libretro/sound.h"
#include "../components/gbsp-libretro/gba_memory.h"
#include "../components/gbsp-libretro/gba_cc_lut.h"

#define AUDIO_SAMPLE_RATE (GBA_SOUND_FREQUENCY)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

#ifndef GBA_DOUBLE_BUFFER
#define GBA_DOUBLE_BUFFER 1
#endif

#ifndef GBA_AUDIO_BEFORE_TICK
#define GBA_AUDIO_BEFORE_TICK 1
#endif

#ifndef GBA_FRAME_PROFILE
#define GBA_FRAME_PROFILE 0
#endif

#ifndef GBA_PROFILE_LOG_EVERY
#define GBA_PROFILE_LOG_EVERY 120
#endif

#ifndef GBA_DISABLE_DISPLAY_SUBMIT
#define GBA_DISABLE_DISPLAY_SUBMIT 0
#endif

#ifndef GBA_DISABLE_AUDIO_SUBMIT
#define GBA_DISABLE_AUDIO_SUBMIT 0
#endif

#ifndef GBA_DISABLE_SOUND_MIX
#define GBA_DISABLE_SOUND_MIX 0
#endif

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;

u32 skip_next_frame = 0;
int sprite_limit = 1;

gbsp_memory_t *gbsp_memory;

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_surface_t *displayUpdate;
static rg_app_t *app;

static const char *SETTING_SOUND_EMULATION = "sound";

void netpacket_poll_receive()
{
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    void *buffer = malloc(buffer_len);
    if (!buffer)
        return false;
    gba_save_state(buffer);
    bool success = rg_storage_write_file(filename, buffer, buffer_len, 0);
    free(buffer);
    return success;
}

static bool load_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    void *buffer = malloc(buffer_len);
    if (!buffer)
        return false;
    bool success = rg_storage_read_file(filename, &buffer, &buffer_len, RG_FILE_USER_BUFFER)
                    && gba_load_state(buffer);
    free(buffer);
    return success;
}

static bool reset_handler(bool hard)
{
    reset_gba();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(displayUpdate ? displayUpdate : currentUpdate, 0);
    }
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // RG_LOGI("%u, %u, %u, %u", port, device, index, id);
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (joystick & RG_KEY_B) val |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (joystick & RG_KEY_A) val |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    return val;
}

void set_fastforward_override(bool fastforward)
{
}

static rg_gui_event_t sound_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_master_enable = !sound_master_enable;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_master_enable);
    }

    strcpy(option->value, sound_master_enable ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &sound_toggle_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

#if GBA_FRAME_PROFILE
typedef struct
{
    uint32_t frames;
    int64_t exec_us, display_us, sound_us, tick_us, audio_us, total_us;
    int64_t max_exec_us, max_display_us, max_sound_us, max_tick_us, max_audio_us, max_total_us;
} gba_frame_profile_t;

static gba_frame_profile_t frame_profile;

static inline void profile_accum(int64_t *sum, int64_t *maxv, int64_t value)
{
    *sum += value;
    if (value > *maxv)
        *maxv = value;
}

static void profile_frame_log(int64_t exec_us, int64_t display_us, int64_t sound_us,
                              int64_t tick_us, int64_t audio_us, int64_t total_us)
{
    profile_accum(&frame_profile.exec_us, &frame_profile.max_exec_us, exec_us);
    profile_accum(&frame_profile.display_us, &frame_profile.max_display_us, display_us);
    profile_accum(&frame_profile.sound_us, &frame_profile.max_sound_us, sound_us);
    profile_accum(&frame_profile.tick_us, &frame_profile.max_tick_us, tick_us);
    profile_accum(&frame_profile.audio_us, &frame_profile.max_audio_us, audio_us);
    profile_accum(&frame_profile.total_us, &frame_profile.max_total_us, total_us);

    if (++frame_profile.frames >= GBA_PROFILE_LOG_EVERY)
    {
        const uint32_t n = frame_profile.frames;
        RG_LOGI("GBA frame avg/max us: exec %lld/%lld, disp %lld/%lld, sound %lld/%lld, tick %lld/%lld, audio %lld/%lld, total %lld/%lld",
            (long long)(frame_profile.exec_us / n), (long long)frame_profile.max_exec_us,
            (long long)(frame_profile.display_us / n), (long long)frame_profile.max_display_us,
            (long long)(frame_profile.sound_us / n), (long long)frame_profile.max_sound_us,
            (long long)(frame_profile.tick_us / n), (long long)frame_profile.max_tick_us,
            (long long)(frame_profile.audio_us / n), (long long)frame_profile.max_audio_us,
            (long long)(frame_profile.total_us / n), (long long)frame_profile.max_total_us);
        memset(&frame_profile, 0, sizeof(frame_profile));
    }
}
#endif

static inline void gba_swap_render_surface(void)
{
#if GBA_DOUBLE_BUFFER
    if (updates[1])
    {
        currentUpdate = (currentUpdate == updates[0]) ? updates[1] : updates[0];
        gba_screen_pixels = currentUpdate->data;
    }
#endif
}

static void gba_try_enable_double_buffer(void)
{
#if GBA_DOUBLE_BUFFER
    if (updates[1])
        return;

    /* Give the ROM cache first claim on PSRAM/internal heap. The second
     * framebuffer is useful for frame pacing, but it should not be allowed to
     * prevent small ROMs from entering resident mode. */
    updates[1] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    if (!updates[1])
        updates[1] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_ANY);

    if (updates[1])
    {
        updates[1]->height = GBA_SCREEN_HEIGHT;
        RG_LOGI("GBA display: double buffering enabled");
    }
    else
    {
        RG_LOGI("GBA display: second framebuffer allocation failed, using single buffer");
    }
#endif
}

void app_main(void)
{
    app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .options = &options_handler,
        },
    });
    // rg_system_set_overclock(4); // Breaks SPI LCD timing

    sound_master_enable = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, true);

    updates[0] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    updates[0]->height = GBA_SCREEN_HEIGHT;
    currentUpdate = updates[0];
    displayUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    gbsp_memory = rg_alloc(sizeof(*gbsp_memory), MEM_ANY);
    RG_LOGI("gbsp_memory=%p", gbsp_memory);

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
    init_gamepak_buffer();
    init_sound();
    // load_bios(RG_BASE_PATH_BIOS "/gba_bios.bin");

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    if (load_gamepak(NULL, app->romPath, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }
    RG_LOGI("ROM loaded, size=%u bytes", gamepak_size);

    gba_try_enable_double_buffer();

    RG_LOGI("reset_gba");
    reset_gba();

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        RG_LOGI("load_state");
        rg_emu_load_state(app->saveSlot);
    }

    RG_LOGI("emulation loop");

    rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH] = {0};

    while (true)
    {
        // RG_TIMER_INIT();
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            memset(&mixbuffer, 0, sizeof(mixbuffer));
            continue;
        }

        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();

#if GBA_FRAME_PROFILE
        int64_t t0 = rg_system_timer();
#endif
        execute_arm(execute_cycles);
        // RG_TIMER_LAP("execute_arm");
#if GBA_FRAME_PROFILE
        int64_t t1 = rg_system_timer();
#endif

        if (!skip_next_frame)
        {
            displayUpdate = currentUpdate;
#if !GBA_DISABLE_DISPLAY_SUBMIT
            rg_display_submit(displayUpdate, 0);
#endif
            gba_swap_render_surface();
        }
#if GBA_FRAME_PROFILE
        int64_t t2 = rg_system_timer();
#endif

#if GBA_DISABLE_SOUND_MIX
        size_t frames_count = 0;
        memset(&mixbuffer, 0, sizeof(mixbuffer));
#else
        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
#endif
        // RG_TIMER_LAP("sound_read_samples");
#if GBA_FRAME_PROFILE
        int64_t t3 = rg_system_timer();
#endif

#if GBA_AUDIO_BEFORE_TICK
#if !GBA_DISABLE_AUDIO_SUBMIT
        rg_audio_submit(mixbuffer, frames_count);
#endif
        // RG_TIMER_LAP("rg_audio_submit");
#if GBA_FRAME_PROFILE
        int64_t t4 = rg_system_timer();
#endif
        rg_system_tick(rg_system_timer() - startTime);
#if GBA_FRAME_PROFILE
        int64_t t5 = rg_system_timer();
#endif
#else
        rg_system_tick(rg_system_timer() - startTime);
#if GBA_FRAME_PROFILE
        int64_t t4 = rg_system_timer();
#endif
#if !GBA_DISABLE_AUDIO_SUBMIT
        rg_audio_submit(mixbuffer, frames_count);
#endif
        // RG_TIMER_LAP("rg_audio_submit");
#if GBA_FRAME_PROFILE
        int64_t t5 = rg_system_timer();
#endif
#endif

#if GBA_FRAME_PROFILE
#if GBA_AUDIO_BEFORE_TICK
        profile_frame_log(t1 - t0, t2 - t1, t3 - t2, t5 - t4, t4 - t3, t5 - startTime);
#else
        profile_frame_log(t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t5 - startTime);
#endif
#endif

        if (skip_next_frame == 0)
            skip_next_frame = app->frameskip;
        else if (skip_next_frame > 0)
            skip_next_frame--;
    }

    RG_PANIC("GBsP Ended");
}
