#include <rg_system.h>
#include <snes9x.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

#define AUDIO_SAMPLE_RATE   (32040)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)

#define FRAME_DOUBLE_BUFFERING
#define AUDIO_DOUBLE_BUFFERING
#define USE_AUDIO_TASK

typedef struct
{
	char name[16];
	struct {
		uint16_t snes9x_mask;
		uint16_t local_mask;
		uint16_t mod_mask;
	} keys[16];
} keymap_t;

enum {
    KEYMAP_TYPE_A = 0,
    KEYMAP_TYPE_B,
    KEYMAP_TYPE_C,
    KEYMAP_REGULAR
};

static const keymap_t KEYMAPS[] = {
	[KEYMAP_TYPE_A] = {"Type A", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_START, 0},
		{SNES_Y_MASK, RG_KEY_SELECT, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	[KEYMAP_TYPE_B] = {"Type B", {
		{SNES_A_MASK, RG_KEY_START, 0},
		{SNES_B_MASK, RG_KEY_A, 0},
		{SNES_X_MASK, RG_KEY_SELECT, 0},
		{SNES_Y_MASK, RG_KEY_B, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	[KEYMAP_TYPE_C] = {"Type C", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, 0, 0},
		{SNES_Y_MASK, 0, 0},
		{SNES_TL_MASK, 0, 0},
		{SNES_TR_MASK, 0, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
    [KEYMAP_REGULAR] = {"Regular", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_X, 0},
		{SNES_Y_MASK, RG_KEY_Y, 0},
		{SNES_TL_MASK, RG_KEY_L, 0},
		{SNES_TR_MASK, RG_KEY_R, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
};

static const size_t KEYMAPS_COUNT = (sizeof(KEYMAPS) / sizeof(keymap_t));

static const char *SNES_BUTTONS[] = {
	"None", "None", "None", "None", "R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"
};

#define AUDIO_LOW_PASS_RANGE ((60 * 65536) / 100)

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_audio_sample_t *audioBuffers[2];
static rg_audio_sample_t *currentAudioBuffer;

#ifdef USE_AUDIO_TASK
static rg_task_t *audio_task_handle;
#endif

extern bool S9xFastTransparency;
extern bool S9xFastFrameSkip;
extern bool S9xFastEffects;
extern bool S9xFastSprites;
bool S9xStandardRomMode = true;

static bool sound_enabled = true;
static bool fast_audio = true;
static bool lowpass_filter = false;
static bool fast_video = true;
static bool fast_sprites = true;
static bool force_50hz = false;
static bool fast_skip = true;
static bool auto_skip_busy = true;
static bool idle_skip = true;
static bool standard_rom = true;
static int max_frame_skip = 1;

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_KEYMAP = "keymap";
static const char *SETTING_SOUND_EMULATION = "apu";
static const char *SETTING_FAST_AUDIO = "fastAudio";
static const char *SETTING_SOUND_FILTER = "filter";
static const char *SETTING_FAST_VIDEO = "fastVideo";
static const char *SETTING_FAST_SPRITES = "fastSprites";
static const char *SETTING_FORCE_50HZ = "force50Hz";
static const char *SETTING_FAST_SKIP = "fastSkip";
static const char *SETTING_AUTO_SKIP_BUSY = "autoSkipBusy";
static const char *SETTING_IDLE_SKIP = "idleSkip";
static const char *SETTING_STANDARD_ROM = "standardRom";
static const char *SETTING_MAX_FRAME_SKIP = "maxFrameSkip";
// --- MAIN

static void log_heap_state(const char *tag)
{
    RG_LOGI("SNES heap %-16s int=%u/%u largest=%u, psram=%u/%u largest=%u",
        tag,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void apply_audio_setting(void)
{
    Settings.APUEnabled = sound_enabled;
    Settings.Mute = !sound_enabled;
    Settings.DisableSoundEcho = fast_audio;
    Settings.InterpolatedSound = !fast_audio;
}

static void apply_standard_rom_setting(void)
{
    S9xStandardRomMode = standard_rom;

    if (!standard_rom)
        return;

    Settings.ForceSuperFX = false;
    Settings.ForceNoSuperFX = true;
    Settings.ForceDSP1 = false;
    Settings.ForceNoDSP1 = true;
    Settings.ForceSA1 = false;
    Settings.ForceNoSA1 = true;
    Settings.ForceC4 = false;
    Settings.ForceNoC4 = true;
    Settings.ForceSDD1 = false;
    Settings.ForceNoSDD1 = true;

    Settings.SuperFX = false;
    Settings.DSP1Master = false;
    Settings.SA1 = false;
    Settings.C4 = false;
    Settings.SDD1 = false;
    Settings.SPC7110 = false;
    Settings.SPC7110RTC = false;
    Settings.OBC1 = false;
    Settings.DSP = 0;
    Settings.SRTC = false;
    Settings.SETA = 0;
}

static void apply_region_setting(void)
{
    Settings.ForcePAL = force_50hz;
    Settings.ForceNTSC = false;
}

static void update_keymap(int id)
{
    keymap_id = id % KEYMAPS_COUNT;
    keymap = KEYMAPS[keymap_id];
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    return S9xSaveState(filename);
}

static bool load_state_handler(const char *filename)
{
    bool ok = S9xLoadState(filename);
    apply_audio_setting();
    apply_standard_rom_setting();
    Settings.Shutdown = idle_skip;
    return ok;
}

static bool reset_handler(bool hard)
{
    S9xReset();
    apply_audio_setting();
    apply_standard_rom_setting();
    Settings.Shutdown = idle_skip;
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_enabled = !sound_enabled;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_enabled);
        apply_audio_setting();
    }

    strcpy(option->value, sound_enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t fast_audio_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        fast_audio = !fast_audio;
        rg_settings_set_number(NS_APP, SETTING_FAST_AUDIO, fast_audio);
        apply_audio_setting();
    }

    strcpy(option->value, fast_audio ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        lowpass_filter = !lowpass_filter;
        rg_settings_set_number(NS_APP, SETTING_SOUND_FILTER, lowpass_filter);
    }

    strcpy(option->value, lowpass_filter ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t fast_video_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        fast_video = !fast_video;
        S9xFastTransparency = fast_video;
        S9xFastEffects = fast_video;
        rg_settings_set_number(NS_APP, SETTING_FAST_VIDEO, fast_video);
    }

    strcpy(option->value, fast_video ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t fast_sprites_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        fast_sprites = !fast_sprites;
        S9xFastSprites = fast_sprites;
        rg_settings_set_number(NS_APP, SETTING_FAST_SPRITES, fast_sprites);
    }

    strcpy(option->value, fast_sprites ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t force_50hz_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        force_50hz = !force_50hz;
        rg_settings_set_number(NS_APP, SETTING_FORCE_50HZ, force_50hz);
    }

    strcpy(option->value, force_50hz ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t frame_skip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        max_frame_skip += (event == RG_DIALOG_NEXT) ? 1 : -1;
        max_frame_skip = RG_MIN(3, RG_MAX(0, max_frame_skip));
        rg_settings_set_number(NS_APP, SETTING_MAX_FRAME_SKIP, max_frame_skip);
    }

    sprintf(option->value, "%d", max_frame_skip);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t fast_skip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        fast_skip = !fast_skip;
        S9xFastFrameSkip = fast_skip;
        rg_settings_set_number(NS_APP, SETTING_FAST_SKIP, fast_skip);
    }

    strcpy(option->value, fast_skip ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t auto_skip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        auto_skip_busy = !auto_skip_busy;
        rg_settings_set_number(NS_APP, SETTING_AUTO_SKIP_BUSY, auto_skip_busy);
    }

    strcpy(option->value, auto_skip_busy ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t idle_skip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        idle_skip = !idle_skip;
        Settings.Shutdown = idle_skip;
        rg_settings_set_number(NS_APP, SETTING_IDLE_SKIP, idle_skip);
    }

    strcpy(option->value, idle_skip ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t standard_rom_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        standard_rom = !standard_rom;
        rg_settings_set_number(NS_APP, SETTING_STANDARD_ROM, standard_rom);
        apply_standard_rom_setting();
    }

    strcpy(option->value, standard_rom ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_PREV && --keymap_id < 0)
            keymap_id = KEYMAPS_COUNT - 1;
        if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1)
            keymap_id = 0;
        update_keymap(keymap_id);
        rg_settings_set_number(NS_APP, SETTING_KEYMAP, keymap_id);
        return RG_DIALOG_REDRAW;
    }

    if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_CANCEL;
    }

    if (option->arg == -1)
    {
        strcat(strcat(strcpy(option->value, "< "), keymap.name), " >");
    }
    else if (option->arg >= 0)
    {
        int local_button = keymap.keys[option->arg].local_mask;
        int mod_button = keymap.keys[option->arg].mod_mask;
        int snes9x_button = log2(keymap.keys[option->arg].snes9x_mask); // convert bitmask to bit number

        if (snes9x_button < 4 || (local_button & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT)))
        {
            option->flags = RG_DIALOG_FLAG_HIDDEN;
            return RG_DIALOG_VOID;
        }

        if (keymap.keys[option->arg].mod_mask)
            sprintf(option->value, "%s + %s", rg_input_get_key_name(mod_button), rg_input_get_key_name(local_button));
        else
            sprintf(option->value, "%s", rg_input_get_key_name(local_button));

        option->label = SNES_BUTTONS[snes9x_button];
        option->flags = RG_DIALOG_FLAG_NORMAL;
    }

    return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {-1, _("Profile"), "-", RG_DIALOG_FLAG_NORMAL, &change_keymap_cb},
            {-2, "", NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {-3, "snes9x  ", "handheld", RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {1, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {2, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {3, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {4, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {5, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {6, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {7, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {8, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {9, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {10, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {11, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {12, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {13, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {14, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {15, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, keymap.name);
    return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = currentUpdate->data;
    uint32_t subscreen_caps = fast_video ? MEM_SLOW : MEM_FAST;
    GFX.SubScreen = rg_alloc(GFX.Pitch * SNES_HEIGHT_EXTENDED, subscreen_caps);
    GFX.ZBuffer = rg_alloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MEM_FAST);
    GFX.SubZBuffer = rg_alloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, subscreen_caps);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joypad = 0;

    for (int i = 0; i < RG_COUNT(keymap.keys); ++i)
    {
        uint32_t bitmask = keymap.keys[i].local_mask | keymap.keys[i].mod_mask;
        if (bitmask && bitmask == (joystick & bitmask))
        {
            joypad |= keymap.keys[i].snes9x_mask;
        }
    }

    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

static inline void mix_samples(int32_t count)
{
    currentAudioBuffer = audioBuffers[currentAudioBuffer == audioBuffers[0]];
    if (lowpass_filter)
        S9xMixSamplesLowPass((int16_t *)currentAudioBuffer, count, AUDIO_LOW_PASS_RANGE);
    else
        S9xMixSamples((int16_t *)currentAudioBuffer, count);
}

#ifdef USE_AUDIO_TASK
static void audio_task(void *arg)
{
    rg_task_msg_t msg;
    while (rg_task_receive(&msg, -1))
    {
        if (msg.type == RG_TASK_MSG_STOP)
            break;
        if (msg.type != 0)
            mix_samples(msg.dataInt << 1);
        rg_audio_submit(currentAudioBuffer, msg.dataInt);
    }
}
#endif

static rg_gui_event_t performance_options_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {0, _("Fast sprites"), "-", RG_DIALOG_FLAG_NORMAL, &fast_sprites_cb},
            {0, _("Force 50Hz"),   "-", RG_DIALOG_FLAG_NORMAL, &force_50hz_cb},
            {0, _("Fast skip"),    "-", RG_DIALOG_FLAG_NORMAL, &fast_skip_cb},
            {0, _("Auto skip"),    "-", RG_DIALOG_FLAG_NORMAL, &auto_skip_cb},
            {0, _("Idle skip"),    "-", RG_DIALOG_FLAG_NORMAL, &idle_skip_cb},
            {0, _("Frame skip"),   "-", RG_DIALOG_FLAG_NORMAL, &frame_skip_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, _("Open"));
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &apu_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Fast audio"),   "-", RG_DIALOG_FLAG_NORMAL, &fast_audio_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio filter"), "-", RG_DIALOG_FLAG_NORMAL, &lowpass_filter_cb};
    *dest++ = (rg_gui_option_t){0, _("Fast video"),   "-", RG_DIALOG_FLAG_NORMAL, &fast_video_cb};
    *dest++ = (rg_gui_option_t){0, _("Standard ROM"),  "-", RG_DIALOG_FLAG_NORMAL, &standard_rom_cb};
    *dest++ = (rg_gui_option_t){0, _("Performance"),   "-", RG_DIALOG_FLAG_NORMAL, &performance_options_cb};
    *dest++ = (rg_gui_option_t){0, _("Controls"),     "-", RG_DIALOG_FLAG_NORMAL, &menu_keymap_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60, // Will be adjusted later if a PAL ROM is loaded
        .storageRequired = true,
        .romRequired = true,
        .handlers.loadState = &load_state_handler,
        .handlers.saveState = &save_state_handler,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
        .handlers.options = &options_handler,
        .mallocAlwaysInternal = 0x10000,
    };
    app = rg_system_init(&config);
    log_heap_state("system");

    // Load settings
    sound_enabled = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, 1);
    fast_audio = rg_settings_get_number(NS_APP, SETTING_FAST_AUDIO, 1);
    lowpass_filter = rg_settings_get_number(NS_APP, SETTING_SOUND_FILTER, 0);
    fast_video = rg_settings_get_number(NS_APP, SETTING_FAST_VIDEO, 1);
    fast_sprites = rg_settings_get_number(NS_APP, SETTING_FAST_SPRITES, 1);
    force_50hz = rg_settings_get_number(NS_APP, SETTING_FORCE_50HZ, 0);
    fast_skip = rg_settings_get_number(NS_APP, SETTING_FAST_SKIP, 1);
    auto_skip_busy = rg_settings_get_number(NS_APP, SETTING_AUTO_SKIP_BUSY, 1);
    idle_skip = rg_settings_get_number(NS_APP, SETTING_IDLE_SKIP, 1);
    standard_rom = rg_settings_get_number(NS_APP, SETTING_STANDARD_ROM, 1);
    max_frame_skip = RG_MIN(3, RG_MAX(0, rg_settings_get_number(NS_APP, SETTING_MAX_FRAME_SKIP, 1)));
    S9xFastTransparency = fast_video;
    S9xFastFrameSkip = fast_skip;
    S9xFastEffects = fast_video;
    S9xFastSprites = fast_sprites;
    int default_keymap = (rg_input_key_is_present(RG_KEY_X|RG_KEY_Y|RG_KEY_L|RG_KEY_R)) ? KEYMAP_REGULAR : KEYMAP_TYPE_A;
    update_keymap(rg_settings_get_number(NS_APP, SETTING_KEYMAP, default_keymap));
    RG_LOGI("SNES options: fastVideo=%d fastSprites=%d force50Hz=%d fastSkip=%d autoSkip=%d idleSkip=%d frameSkipMax=%d",
        fast_video, fast_sprites, force_50hz, fast_skip, auto_skip_busy, idle_skip, max_frame_skip);

    // Allocate surfaces and audio buffers
    updates[0] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, MEM_FAST);
    updates[0]->height = SNES_HEIGHT;
#ifdef FRAME_DOUBLE_BUFFERING
    updates[1] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, MEM_FAST);
    updates[1]->height = SNES_HEIGHT;
#else
    updates[1] = updates[0];
#endif
    currentUpdate = updates[0];

#ifdef AUDIO_DOUBLE_BUFFERING
    audioBuffers[0] = (rg_audio_sample_t *)rg_alloc(AUDIO_BUFFER_LENGTH * 4, MEM_FAST);
    audioBuffers[1] = (rg_audio_sample_t *)rg_alloc(AUDIO_BUFFER_LENGTH * 4, MEM_FAST);
#else
    audioBuffers[0] = (rg_audio_sample_t *)rg_alloc(AUDIO_BUFFER_LENGTH * 4, MEM_FAST);
    audioBuffers[1] = audioBuffers[0];
#endif
    currentAudioBuffer = audioBuffers[0];

    if (!updates[0] || !updates[1] || !audioBuffers[0] || !audioBuffers[1])
        RG_PANIC("Failed to allocate buffers!");
    log_heap_state("frontend");

#ifdef USE_AUDIO_TASK
    // Set up multicore audio
    audio_task_handle = rg_task_create("snes_audio", &audio_task, NULL, 2048, 2, RG_TASK_PRIORITY_6, 1);
    RG_ASSERT(audio_task_handle, "Failed to create audio task!");
#endif

    Settings.CyclesPercentage = 105;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
    apply_audio_setting();
    apply_standard_rom_setting();
    apply_region_setting();

    if (!S9xInitDisplay())
        RG_PANIC("Display init failed!");
    log_heap_state("display");

    if (!S9xInitMemory())
        RG_PANIC("Memory init failed!");
    log_heap_state("memory");

    if (!S9xInitAPU())
        RG_PANIC("APU init failed!");
    log_heap_state("apu");

    if (!S9xInitSound(0, 0))
        RG_PANIC("Sound init failed!");
    apply_audio_setting();
    log_heap_state("sound");

    if (!S9xInitGFX())
        RG_PANIC("Graphics init failed!");
    log_heap_state("gfx");

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);

    const char *filename = app->romPath;

    if (rg_extension_match(filename, "zip"))
    {
        if (!rg_storage_unzip_file(filename, NULL, (void **)&Memory.ROM, &Memory.ROM_AllocSize, RG_FILE_USER_BUFFER))
            RG_PANIC("ROM file unzipping failed!");
        filename = NULL;
    }

    apply_standard_rom_setting();
    apply_region_setting();
    if (!LoadROM(filename))
        RG_PANIC(S9xLoadError[0] ? S9xLoadError : "ROM loading failed!");
    apply_audio_setting();
    apply_standard_rom_setting();
    Settings.Shutdown = idle_skip;
    log_heap_state("rom");

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(Memory.ROMFramesPerSecond);
    app->frameskip = 0;

    const int samplesPerFrame = (int)roundf((float)app->sampleRate / app->tickRate);
    bool menuCancelled = false;
    bool menuPressed = false;
    int skipFrames = 0;
    int64_t lastFrameTime = rg_system_timer();

    while (1)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        bool drawFrame = (skipFrames == 0);
        bool slowFrame = false;

        if (drawFrame && auto_skip_busy && rg_display_is_busy())
        {
            drawFrame = false;
            skipFrames = 1;
        }

        if (menuPressed && !(joystick & RG_KEY_MENU))
        {
            if (!menuCancelled)
            {
                rg_task_delay(50);
                rg_gui_game_menu();
                menuPressed = false;
                menuCancelled = false;
                continue;
            }
            menuCancelled = false;
        }
        else if (joystick & RG_KEY_OPTION)
        {
            rg_gui_options_menu();
            memset(audioBuffers[0], 0, AUDIO_BUFFER_LENGTH * 4);
            memset(audioBuffers[1], 0, AUDIO_BUFFER_LENGTH * 4);
            continue;
        }

        menuPressed = joystick & RG_KEY_MENU;

        if (menuPressed && (joystick & ~RG_KEY_MENU))
        {
            menuCancelled = true;
        }

        IPPU.RenderThisFrame = drawFrame;
        GFX.Screen = currentUpdate->data;

        S9xMainLoop();

        if (drawFrame)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(currentUpdate, 0);
            currentUpdate = updates[currentUpdate == updates[0]];
        }

        //音量控制
        rg_volume_handle(joystick, startTime);
        
    #ifdef USE_AUDIO_TASK
        rg_system_tick(rg_system_timer() - startTime);
        rg_task_msg_t msg = {.type = (int)sound_enabled, .dataInt = samplesPerFrame};
        if (sound_enabled || app->frameTime - (rg_system_timer() - startTime) > 5000)
            rg_task_send(audio_task_handle, &msg, 0);
    #else
        if (sound_enabled)
            mix_samples(samplesPerFrame << 1);
        rg_system_tick(rg_system_timer() - startTime);
        if (sound_enabled || app->frameTime - (rg_system_timer() - startTime) > 5000) // 之前是2000，更大值以减少音频丢弃
            rg_audio_submit(currentAudioBuffer, samplesPerFrame);
    #endif

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            int sharedFrameskip = RG_MIN(max_frame_skip, RG_MAX(0, app->frameskip));

            if (max_frame_skip <= 0)
                skipFrames = 0;
            else if (sharedFrameskip > 0 && (elapsed > app->frameTime + 1000 || (drawFrame && slowFrame)))
                skipFrames = sharedFrameskip;
            else if (force_50hz && elapsed > app->frameTime + 5000)
                skipFrames = 1;
            else if (force_50hz)
                skipFrames = 0;
            else if (elapsed > app->frameTime * 2)
                skipFrames = max_frame_skip;
            else if (elapsed > app->frameTime + 2000)
                skipFrames = RG_MIN(2, max_frame_skip);
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }

        int64_t currentTime = rg_system_timer();
        int frameTime = app->frameTime;
        int sleep = frameTime - (currentTime - lastFrameTime);

        if (sleep > 0)
        {
            rg_usleep(sleep);
        }
        else if (sleep < -(frameTime / 2) && max_frame_skip > 0)
        {
            skipFrames = RG_MIN(max_frame_skip, RG_MAX(skipFrames, 1));
        }

        int64_t prevTime = rg_system_timer();
        lastFrameTime += frameTime;
        if ((lastFrameTime + frameTime) < prevTime)
            lastFrameTime = prevTime;
    }
}
