#include "pcsx_port.h"
#include "pcsx_runtime.h"

#include <string.h>

#ifdef RG_PSX_VENDOR_PRESENT
#ifdef _
#undef _
#endif
#include "vendor/pcsx_rearmed/libpcsxcore/plugins.h"
#include "vendor/pcsx_rearmed/libpcsxcore/misc.h"
#include "vendor/pcsx_rearmed/libpcsxcore/r3000a.h"
#include "vendor/pcsx_rearmed/frontend/main.h"
#include "vendor/pcsx_rearmed/frontend/plugin.h"
#include "vendor/pcsx_rearmed/frontend/plugin_lib.h"

void pcsx_runtime_sync_input(void);
#endif

bool pcsx_port_probe_vendor(pcsx_port_probe_t *out_probe)
{
    if (!out_probe)
        return false;

    memset(out_probe, 0, sizeof(*out_probe));
#ifdef RG_PSX_VENDOR_PRESENT
    out_probe->vendor_tree_present = true;
    out_probe->frontend_present = true;
    out_probe->core_present = true;
    out_probe->plugin_set_present = true;
#endif
    return true;
}

bool pcsx_port_boot(const pcsx_port_context_t *ctx)
{
    pcsx_port_probe_t probe = {0};

    if (!ctx || !ctx->boot || !ctx->disc || !ctx->bios)
        return false;

    pcsx_port_probe_vendor(&probe);
    if (!probe.vendor_tree_present)
    {
        pcsx_runtime_set_last_message("PCSX ReARMed source tree is missing.");
        return false;
    }

    pcsx_runtime_set_config(&(pcsx_runtime_config_t){
        .bios_path = ctx->boot->bios_path,
        .disc_path = ctx->boot->rom_path,
        .memcard0_path = ctx->boot->memcard0_path,
        .memcard1_path = ctx->boot->memcard1_path,
        .sample_rate = ctx->boot->sample_rate,
        .frame_rate = ctx->boot->frame_rate,
    });

#ifdef RG_PSX_VENDOR_PRESENT
    bool plugins_loaded = false;
    bool plugins_opened = false;

    ready_to_go = 0;
    emu_core_preinit();
    set_cd_image(ctx->boot->rom_path);

    if (emu_core_init() != 0)
    {
        pcsx_runtime_set_last_message("PCSX core initialization failed.");
        goto fail;
    }

    if (LoadPlugins() == -1)
    {
        pcsx_runtime_set_last_message("PCSX plugin loading failed.");
        goto fail;
    }
    plugins_loaded = true;

    plugin_call_rearmed_cbs();

    if (OpenPlugins(1) == -1)
    {
        pcsx_runtime_set_last_message("PCSX plugin open failed.");
        goto fail;
    }
    plugins_opened = true;

    if (CheckCdrom() == -1)
    {
        const char *message = pcsx_runtime_get_last_message();
        pcsx_runtime_set_last_message(message && message[0] ? message : "PCSX disc check failed.");
        goto fail;
    }

    SysReset();
    SysPrintf("ESP32 fast boot state after reset: pc=%08x hle=%d slow=%u\n",
        psxRegs.pc, Config.HLE, Config.SlowBoot);

    if (LoadCdrom() == -1)
    {
        const char *message = pcsx_runtime_get_last_message();
        pcsx_runtime_set_last_message(message && message[0] ? message : "PCSX disc boot failed.");
        goto fail;
    }

    SysPrintf("ESP32 fast boot complete: pc=%08x\n", psxRegs.pc);
    ready_to_go = 1;
    pcsx_runtime_set_last_message("PCSX interpreter booted.");
    return true;

fail:
    if (plugins_opened)
        ClosePlugins();
    if (plugins_loaded)
        ReleasePlugins();
    SysClose();
    ready_to_go = 0;
    return false;
#else
    pcsx_runtime_set_last_message(
        "PCSX vendor tree detected and Retro-Go platform hooks are staged. "
        "Next step is wiring boot flow, video, audio, and per-frame execution."
    );
    return false;
#endif
}

bool pcsx_port_run_frame(void)
{
#ifdef RG_PSX_VENDOR_PRESENT
    if (!ready_to_go || !psxCpu)
    {
        pcsx_runtime_set_last_message("PCSX CPU is not ready.");
        return false;
    }

    pcsx_runtime_sync_input();
    psxRegs.stop = 0;
    psxCpu->Execute(&psxRegs);

    /* Some boot paths do not flip video every host tick. Treat that as a
     * successful emulation step instead of dropping back to the launcher. */
    retrogo_psx_submit_frame();
    return true;
#else
    return false;
#endif
}

void pcsx_port_set_perf_hint(bool behind_frame)
{
    (void)behind_frame;
#ifdef RG_PSX_VENDOR_PRESENT
    pl_rearmed_cbs.fskip_advice = 0;
    pl_rearmed_cbs.fskip_dirty = 0;
#endif
}

void pcsx_port_shutdown(void)
{
#ifdef RG_PSX_VENDOR_PRESENT
    ready_to_go = 0;
    ClosePlugins();
    ReleasePlugins();
    SysClose();
#endif
}
