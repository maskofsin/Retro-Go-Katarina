#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("jit_xtensa.h")
#include "jit_xtensa.h"
#else
#include "jit/jit_xtensa.h"
#endif

#include "r3000a.h"

#include <stdio.h>

extern R3000Acpu psxInt;
extern R3000Acpu *psxCpu;

#ifndef JIT_PSXINT_APPLYCONFIG_REQUIRES_PSXCPU
#define JIT_PSXINT_APPLYCONFIG_REQUIRES_PSXCPU 1
#endif

static enum blockExecCaller s_jit_fallback_caller;
static unsigned s_jit_psxint_init_done;

static int jit_psx_intfetch_missing(psxRegisters *regs)
{
#if JIT_CHECK_PTRS_INTFETCH
    return regs != NULL && jit_regs_intfetch(regs) == NULL;
#else
    JIT_UNUSED(regs);
    return 0;
#endif
}

static void jit_psx_install_memread_intfetch(psxRegisters *regs)
{
#if JIT_REPAIR_NULL_INTFETCH_WITH_MEMREAD32 && JIT_CHECK_PTRS_INTFETCH && JIT_FETCH32_TAKES_REGS
    if (!jit_psx_intfetch_missing(regs)) return;
    u32 (*fetch_fn)(psxRegisters *, u32) = psxMemRead32;
    memcpy((uint8_t *)(void *)regs + JIT_PSX_PTRS_INTFETCH_OFFSET, &fetch_fn, sizeof(fetch_fn));
#else
    JIT_UNUSED(regs);
#endif
}

static void jit_psx_call_int_apply_config_forced(void)
{
    if (psxInt.ApplyConfig == NULL) return;

#if JIT_PSXINT_APPLYCONFIG_REQUIRES_PSXCPU
    R3000Acpu *saved_cpu = psxCpu;
    psxCpu = &psxInt;
    psxInt.ApplyConfig();
    psxCpu = saved_cpu;
#else
    psxInt.ApplyConfig();
#endif
}

static void jit_psx_refresh_interpreter_config(psxRegisters *regs)
{
    if (!s_jit_psxint_init_done) return;
    if (regs != NULL && !jit_psx_intfetch_missing(regs)) return;

    if (psxInt.ApplyConfig != NULL) {
        psxInt.ApplyConfig();
    }

    if (regs == NULL || jit_psx_intfetch_missing(regs)) {
        jit_psx_call_int_apply_config_forced();
    }
    if (regs != NULL && jit_psx_intfetch_missing(regs)) {
        jit_psx_install_memread_intfetch(regs);
    }
}

void jit_before_interpreter_fallback(psxRegisters *regs)
{
    if (jit_psx_intfetch_missing(regs)) {
        jit_psx_refresh_interpreter_config(regs);
    }
}

void jit_psx_interpreter_execute_fallback(psxRegisters *regs)
{
    jit_before_interpreter_fallback(regs);
    if (psxInt.ExecuteBlock != NULL) {
        psxInt.ExecuteBlock(regs, s_jit_fallback_caller);
    }
}

static int jit_psx_init(void)
{
    int ret = 0;
    if (psxInt.Init != NULL) {
        ret = psxInt.Init();
        if (ret != 0) return ret;
    }
    s_jit_psxint_init_done = 1u;
    ret = jit_init();
    if (ret != 0) {
        printf("JIT init FAILED (code=%d) - interpreter only\n", ret);
        s_jit_psxint_init_done = 0u;
        return 0;
    }
    printf("JIT init OK - buffer allocated\n");
    jit_psx_refresh_interpreter_config(NULL);
    return 0;
}

static void jit_psx_reset(void)
{
    if (psxInt.Reset != NULL) {
        psxInt.Reset();
    }
    jit_reset();
    jit_psx_refresh_interpreter_config(NULL);
}

static void jit_psx_execute(psxRegisters *regs)
{
    s_jit_fallback_caller = (enum blockExecCaller)0;
    jit_psx_refresh_interpreter_config(regs);
    jit_execute(regs);
}

static void jit_psx_execute_block(psxRegisters *regs, enum blockExecCaller caller)
{
    s_jit_fallback_caller = caller;
    jit_psx_refresh_interpreter_config(regs);
    jit_execute_block(regs);
}

static void jit_psx_clear(u32 addr, u32 size)
{
    jit_clear(addr, size);
    if (psxInt.Clear != NULL) {
        psxInt.Clear(addr, size);
    }
}

static void jit_psx_notify(enum R3000Anote note, void *data)
{
    jit_notify((int)note, data);
    if (psxInt.Notify != NULL) {
        psxInt.Notify(note, data);
    }
}

static void jit_psx_apply_config(void)
{
    jit_psx_refresh_interpreter_config(NULL);
    jit_apply_config();
}

static void jit_psx_shutdown(void)
{
    jit_shutdown();
    if (psxInt.Shutdown != NULL) {
        psxInt.Shutdown();
    }
    s_jit_psxint_init_done = 0u;
}

R3000Acpu psxJit = {
    .Init         = jit_psx_init,
    .Reset        = jit_psx_reset,
    .Execute      = jit_psx_execute,
    .ExecuteBlock = jit_psx_execute_block,
    .Clear        = jit_psx_clear,
    .Notify       = jit_psx_notify,
    .ApplyConfig  = jit_psx_apply_config,
    .Shutdown     = jit_psx_shutdown,
};
