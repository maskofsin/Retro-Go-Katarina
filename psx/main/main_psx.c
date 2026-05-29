#include <rg_system.h>
#include <rg_input.h>

#include <psx_core.h>
#include <pcsx_port.h>
#include <pcsx_runtime.h>

#define PSX_AUDIO_RATE 32000
#define PSX_FRAME_RATE 60

static void event_handler(int event, void *arg)
{
    (void)arg;

    if (event == RG_EVENT_REDRAW)
        retrogo_psx_redraw();
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .event = &event_handler,
    };

    rg_app_t *app = rg_system_init(&(const rg_config_t){
        .sampleRate = PSX_AUDIO_RATE,
        .frameRate = PSX_FRAME_RATE,
        .storageRequired = true,
        .romRequired = true,
        .mallocAlwaysInternal = 0,
        .handlers = handlers,
    });

    psx_boot_config_t boot = {0};
    psx_bios_info_t bios = {0};
    psx_disc_info_t disc = {0};

    if (!psx_core_prepare_boot(app, &boot, &bios, &disc) ||
        !psx_core_init(&boot) ||
        !psx_core_load_game(&disc))
    {
        rg_gui_alert("PSX Bring-up", psx_core_get_last_error());
        rg_system_exit();
    }

    app->frameskip = -1;
    bool menu_pressed = false;
    bool menu_cancelled = false;

    while (1)
    {
        int64_t start = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        int64_t elapsed;

        pcsx_runtime_set_host_pad(joystick);

        if (menu_pressed && !(joystick & RG_KEY_MENU))
        {
            menu_pressed = false;
            if (!menu_cancelled)
            {
                rg_task_delay(50);
                rg_gui_game_menu();
                menu_cancelled = false;
                continue;
            }
            menu_cancelled = false;
        }
        else if (joystick & RG_KEY_OPTION)
        {
            rg_gui_options_menu();
            continue;
        }

        menu_pressed = !!(joystick & RG_KEY_MENU);

        if (menu_pressed && (joystick & ~(uint32_t)RG_KEY_MENU))
            menu_cancelled = true;

        if (!pcsx_port_run_frame())
        {
            rg_gui_alert("PSX Runtime", psx_core_get_last_error());
            rg_system_exit();
        }

        elapsed = rg_system_timer() - start;
        pcsx_port_set_perf_hint(elapsed > app->frameTime);
        rg_system_tick(elapsed);

        elapsed = rg_system_timer() - start;
        if (elapsed < app->frameTime)
            rg_usleep(app->frameTime - elapsed);
    }
}
