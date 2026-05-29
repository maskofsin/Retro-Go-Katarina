# JIT Xtensa Integration — Issues & Fixes

## Crash Symptom

```
Guru Meditation Error: InstrFetchProhibited at PC=0x00000000
jit_execute_block:672 → JIT_EXEC_INTERPRETER_ONE(regs)
```

The JIT falls back to the interpreter but `psxRegs.ptrs.intFetch` is NULL. Despite calling `psxInt.Init()` and `psxInt.ApplyConfig()`, the function pointer gets cleared before `psxExecuteBios()` runs.

## Fixed Issues

| # | Problem | Fix Applied |
|---|---------|-------------|
| 1 | `psxMemRead32(uint32_t)` vs `u32(psxRegisters*,u32)` signature mismatch | Changed JIT_FETCH32 to pass regs |
| 2 | `psxMemRLUT`/`psxMemWLUT` globals don't exist — they're `psxRegs.ptrs.memRLUT` | `offsetof()`-based access |
| 3 | Without `DRC_DISABLE`, `psxRegs`/`rcnts` undefined | Keep `DRC_DISABLE=1` + `JIT_XTENSA=1` |
| 4 | JIT didn't call `psxInt.Init()` — dispatch tables (psxBSC[], psxSPC[]) unset | `jit_psx_init()` calls `psxInt.Init()` then `jit_init()` |
| 5 | JIT didn't call `psxInt.ApplyConfig()` — `intFetch` never initialized | `jit_psx_apply_config()` calls `psxInt.ApplyConfig()` then `jit_apply_config()` |
| 6 | JIT Reset/Shutdown didn't propagate to interpreter | Added `psxInt.Reset()` / `psxInt.Shutdown()` calls |

## Remaining Blocker

`psxRegs.ptrs.intFetch` is NULL when `psxInt.ExecuteBlock()` runs during BIOS boot.

### Boot sequence (r3000a.c)

```
psxInit():  psxCpu = &psxJit; psxCpu->Init() → psxInt.Init() + jit_init()
psxReset():
    1. psxMemReset()          — sets psxRegs.ptrs (psxM, psxR, memRLUT, memWLUT)
    2. memset(&psxRegs, 0, offsetof(psxRegisters, ptrs)) — zeros regs, NOT ptrs
    3. psxCpu->ApplyConfig()  → psxInt.ApplyConfig() → sets intFetch = fetchNoCache
    4. psxCpu->Reset()        → psxInt.Reset() (no-op on ptrs)
    5. padReset()
    6. psxHwReset()
    7. psxBiosInit()
    8. psxExecuteBios()       → psxCpu->ExecuteBlock() → intExecuteBlock()
                              → reads regs->ptrs.intFetch → NULL → CRASH
```

### Suspected causes

A. **jit_init() heap allocation** — `heap_caps_malloc(MALLOC_CAP_EXEC)` might overlap with psxRegs memory region, corrupting ptrs

B. **One of padReset()/psxHwReset()/psxBiosInit()** zeroes ptrs.intFetch between ApplyConfig and ExecuteBlock

C. **offsetof() calculation wrong** — memset might accidentally include ptrs, zeroing intFetch

D. **intApplyConfig() called but condition fails** — `psxCpu != &psxInt` check might prevent intFetch assignment

### Quick Fix (untested)

In `jit_execute_block()`, force-set intFetch right before fallback:

```c
if (regs->ptrs.intFetch == NULL)
    psxInt.ApplyConfig();
JIT_EXEC_INTERPRETER_ONE(regs);
```

## Files

| File | Role |
|------|------|
| `jit/jit_xtensa.c` | Main JIT: init, block cache, execute loop |
| `jit/jit_compile.c` | MIPS→Xtensa compiler |
| `jit/jit_emit.h` | Xtensa instruction encoding macros |
| `jit/jit_xtensa.h` | Public API + overrides (JIT_FETCH32, JIT_EXEC_INTERPRETER_ONE) |
| `jit/jit_xtensa_config_esp32s3.h` | ESP32-S3 profile |
| `jit/jit_psx.c` | R3000Acpu wrapper |
| `r3000a.c:44-55` | psxInit — selects psxJit |
| `r3000a.c:62-85` | psxReset — boot sequence |
| `psxinterpreter.c:1227` | intExecuteBlock — reads intFetch |
| `psxinterpreter.c:1286` | intApplyConfig — sets intFetch |
| `CMakeLists.txt` | Build config |

## Integration Steps

1. Copy `jit/` folder into `psx/components/pcsx/jit/`
2. Add to CMakeLists.txt:
   - Include dir: `jit/`
   - Sources: `jit/jit_xtensa.c`, `jit/jit_compile.c`, `jit/jit_psx.c`
   - Defines: `JIT_XTENSA=1`, `JIT_XTENSA_INCLUDE_PCSX_HEADERS=1` (keep `DRC_DISABLE=1`)
   - Compile option: `-include jit_xtensa_config_esp32s3.h`
3. Modify `r3000a.c:psxInit()` to select `psxJit` when `JIT_XTENSA` defined
4. Modify `pcsx_port_platform.c` to set `Config.Cpu = CPU_DYNAREC` when `JIT_XTENSA` defined
