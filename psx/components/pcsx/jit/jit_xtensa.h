#ifndef JIT_XTENSA_H
#define JIT_XTENSA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Integration notes:
 *   - In the emulator build, include this after the PCSX headers that define
 *     psxRegisters and R3000Acpu, or define JIT_XTENSA_INCLUDE_PCSX_HEADERS=1
 *     and adjust the include names below.
 *   - The standalone mode is only for syntax checking on non-Xtensa hosts. It
 *     never executes generated Xtensa code.
 */
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#include "r3000a.h"
#include "psxmem.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JIT_CODE_BUFFER_SIZE
/* Requested maximum executable JIT cache. Runtime clamps this to available internal EXEC RAM. */
#define JIT_CODE_BUFFER_SIZE (256u * 1024u)
#endif

#ifndef JIT_MIN_CODE_BUFFER_SIZE
#define JIT_MIN_CODE_BUFFER_SIZE (32u * 1024u)
#endif

#ifndef JIT_EXEC_IRAM_HEADROOM_BYTES
/* Keep internal executable RAM available for Wi-Fi/BT/USB/JTAG stacks and late allocations. */
#define JIT_EXEC_IRAM_HEADROOM_BYTES (24u * 1024u)
#endif

#ifndef JIT_STAGING_BUFFER_SIZE
/* Byte-addressable temporary buffer used before aligned copy into executable RAM. */
#define JIT_STAGING_BUFFER_SIZE (32u * 1024u)
#endif

#ifndef JIT_CACHE_LINE_BYTES
#define JIT_CACHE_LINE_BYTES 64u
#endif

#ifndef JIT_CACHE_SYNC_ALIGN_BYTES
#define JIT_CACHE_SYNC_ALIGN_BYTES JIT_CACHE_LINE_BYTES
#endif

#ifndef JIT_EXEC_ALIGN_BYTES
#define JIT_EXEC_ALIGN_BYTES JIT_CACHE_SYNC_ALIGN_BYTES
#endif

#if (JIT_CACHE_SYNC_ALIGN_BYTES < 4u) || ((JIT_CACHE_SYNC_ALIGN_BYTES & (JIT_CACHE_SYNC_ALIGN_BYTES - 1u)) != 0u)
#error "JIT_CACHE_SYNC_ALIGN_BYTES must be a power of two and at least 4"
#endif

#if (JIT_EXEC_ALIGN_BYTES < 4u) || ((JIT_EXEC_ALIGN_BYTES & (JIT_EXEC_ALIGN_BYTES - 1u)) != 0u)
#error "JIT_EXEC_ALIGN_BYTES must be a power of two and at least 4"
#endif

#if JIT_EXEC_ALIGN_BYTES < JIT_CACHE_SYNC_ALIGN_BYTES
#error "JIT_EXEC_ALIGN_BYTES must be at least JIT_CACHE_SYNC_ALIGN_BYTES so cache sync never begins before the executable allocation"
#endif

#ifndef JIT_BLOCK_HASH_SIZE
#define JIT_BLOCK_HASH_SIZE 4096u
#endif

#if (JIT_BLOCK_HASH_SIZE == 0u) || ((JIT_BLOCK_HASH_SIZE & (JIT_BLOCK_HASH_SIZE - 1u)) != 0u)
#error "JIT_BLOCK_HASH_SIZE must be a power of two"
#endif

#ifndef JIT_BLOCK_TABLE_IN_SPIRAM
/* ESP32-S3 profiles can set this to 1 so block metadata lives in PSRAM instead
 * of internal SRAM. Non-ESP builds ignore the SPIRAM preference.
 */
#define JIT_BLOCK_TABLE_IN_SPIRAM 0
#endif

#ifndef JIT_MAX_MIPS_INSNS_PER_BLOCK
#define JIT_MAX_MIPS_INSNS_PER_BLOCK 64u
#endif

#ifndef JIT_LITERAL_SLOTS_PER_BLOCK
#define JIT_LITERAL_SLOTS_PER_BLOCK 128u
#endif

#ifndef JIT_BLOCK_CODE_GUARD_BYTES
#define JIT_BLOCK_CODE_GUARD_BYTES 512u
#endif

#ifndef JIT_LITERAL_LOW_WATERMARK_SLOTS
/* Stop extending a block before the literal pool is nearly full, so the block
 * can still emit a clean dispatcher return instead of failing late.
 */
#define JIT_LITERAL_LOW_WATERMARK_SLOTS 8u
#endif

#define JIT_BLOCK_RESERVE_BYTES \
    ((JIT_LITERAL_SLOTS_PER_BLOCK * 4u) + \
     JIT_BLOCK_CODE_GUARD_BYTES + \
     (JIT_MAX_MIPS_INSNS_PER_BLOCK * 160u))

#if JIT_STAGING_BUFFER_SIZE < JIT_BLOCK_RESERVE_BYTES
#error "JIT_STAGING_BUFFER_SIZE must be at least JIT_BLOCK_RESERVE_BYTES"
#endif

#ifndef JIT_NATIVE_ENABLED
#if defined(__XTENSA__) || defined(__xtensa__)
#define JIT_NATIVE_ENABLED 1
#else
#define JIT_NATIVE_ENABLED 0
#endif
#endif


#ifndef JIT_XTENSA_USE_WRAPPER
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#define JIT_XTENSA_USE_WRAPPER 1
#else
#define JIT_XTENSA_USE_WRAPPER 0
#endif
#endif

#ifndef JIT_FETCH32_TAKES_REGS
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#define JIT_FETCH32_TAKES_REGS 1
#else
#define JIT_FETCH32_TAKES_REGS 0
#endif
#endif

#ifndef JIT_USE_REGS_PTRS_MEMLUT
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#define JIT_USE_REGS_PTRS_MEMLUT 1
#else
#define JIT_USE_REGS_PTRS_MEMLUT 0
#endif
#endif

#ifndef JIT_USE_PSXINT_EXECUTEBLOCK_FALLBACK
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#define JIT_USE_PSXINT_EXECUTEBLOCK_FALLBACK 1
#else
#define JIT_USE_PSXINT_EXECUTEBLOCK_FALLBACK 0
#endif
#endif

#ifndef JIT_CHECK_PTRS_INTFETCH
#if defined(JIT_XTENSA_INCLUDE_PCSX_HEADERS) && !defined(JIT_XTENSA_STANDALONE_TEST)
#define JIT_CHECK_PTRS_INTFETCH 1
#else
#define JIT_CHECK_PTRS_INTFETCH 0
#endif
#endif

#ifndef JIT_REPAIR_NULL_INTFETCH_WITH_MEMREAD32
#if JIT_CHECK_PTRS_INTFETCH && JIT_FETCH32_TAKES_REGS
#define JIT_REPAIR_NULL_INTFETCH_WITH_MEMREAD32 1
#else
#define JIT_REPAIR_NULL_INTFETCH_WITH_MEMREAD32 0
#endif
#endif

#ifndef JIT_USE_WINDOWED_ABI
#if defined(CONFIG_COMPILER_CALL0_ABI) || defined(__XTENSA_CALL0_ABI__)
#define JIT_USE_WINDOWED_ABI 0
#else
#define JIT_USE_WINDOWED_ABI 1
#endif
#endif

#if JIT_NATIVE_ENABLED && !JIT_USE_WINDOWED_ABI && !defined(JIT_ALLOW_CALL0_NATIVE)
#error "Native Xtensa JIT currently requires windowed ABI register windows; disable JIT_NATIVE_ENABLED or add a call0 save/restore trampoline"
#endif

#ifndef JIT_TRACK_STORES_FOR_SMC
#define JIT_TRACK_STORES_FOR_SMC 1
#endif

#ifndef JIT_END_BLOCK_AFTER_STORE
/* First-safe SMC policy: after a translated store, return to dispatcher so the recorded write can invalidate code immediately. */
#define JIT_END_BLOCK_AFTER_STORE JIT_TRACK_STORES_FOR_SMC
#endif

#ifndef JIT_DIRECT_MAIN_RAM_ONLY
/* Native lw/sw should not bypass scratchpad, hardware-register, BIOS, or
 * expansion handlers. By default only PSX main RAM mirrors are emitted as
 * direct native memory operations; other addresses bail to the interpreter.
 */
#define JIT_DIRECT_MAIN_RAM_ONLY 1
#endif

#ifndef JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
/* When the direct path is main-RAM-only, address psxRegs.ptrs.psxM directly.
 * This is both faster and avoids using a read LUT for writes.
 */
#define JIT_USE_PTRS_PSXM_FOR_MAIN_RAM JIT_DIRECT_MAIN_RAM_ONLY
#endif

#ifndef JIT_MEM_LUT_PAGE_BASE_IS_BIASED
/*
 * 0: PCSX/PCSX-R style LUT entries are host page-base pointers; native address
 *    is lut[guest >> 16] + (guest & 0xffff).
 * 1: Integration-specific biased LUT entries; native address is
 *    lut[guest >> 16] + guest.
 */
#if defined(JIT_MEMRLUT_BIASED_BASES)
#define JIT_MEM_LUT_PAGE_BASE_IS_BIASED JIT_MEMRLUT_BIASED_BASES
#else
#define JIT_MEM_LUT_PAGE_BASE_IS_BIASED 0
#endif
#endif

#ifndef JIT_MEMRLUT_BIASED_BASES
#define JIT_MEMRLUT_BIASED_BASES JIT_MEM_LUT_PAGE_BASE_IS_BIASED
#endif

#ifndef JIT_HAS_MEMWLUT
/* PCSX-style cores normally expose separate read and write lookup tables. */
#define JIT_HAS_MEMWLUT 1
#endif

#ifndef JIT_ALLOW_STORE_THROUGH_MEMRLUT_WITHOUT_MEMWLUT
/* Native stores need a write lookup table unless the direct psxM main-RAM path
 * is active. Writing through a read LUT can bypass MMIO/read-only semantics, so
 * keep that legacy escape hatch opt-in only.
 */
#define JIT_ALLOW_STORE_THROUGH_MEMRLUT_WITHOUT_MEMWLUT 0
#endif

#ifndef JIT_NORMALIZE_MAIN_RAM_MIRRORS
/* PSX main RAM is 2 MiB mirrored through the low 8 MiB physical window. */
#define JIT_NORMALIZE_MAIN_RAM_MIRRORS 1
#endif

#ifndef JIT_PSX_PHYS_MASK
#define JIT_PSX_PHYS_MASK 0x1fffffffu
#endif

#ifndef JIT_PSX_MAIN_RAM_SIZE_BYTES
#define JIT_PSX_MAIN_RAM_SIZE_BYTES (2u * 1024u * 1024u)
#endif

#ifndef JIT_PSX_MAIN_RAM_MIRROR_BYTES
#define JIT_PSX_MAIN_RAM_MIRROR_BYTES (8u * 1024u * 1024u)
#endif

#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM || JIT_DIRECT_MAIN_RAM_ONLY
#if (JIT_PSX_MAIN_RAM_SIZE_BYTES == 0u) || ((JIT_PSX_MAIN_RAM_SIZE_BYTES & (JIT_PSX_MAIN_RAM_SIZE_BYTES - 1u)) != 0u)
#error "JIT_PSX_MAIN_RAM_SIZE_BYTES must be a nonzero power of two"
#endif
#if (JIT_PSX_MAIN_RAM_MIRROR_BYTES == 0u) || ((JIT_PSX_MAIN_RAM_MIRROR_BYTES & (JIT_PSX_MAIN_RAM_MIRROR_BYTES - 1u)) != 0u)
#error "JIT_PSX_MAIN_RAM_MIRROR_BYTES must be a nonzero power of two"
#endif
#if JIT_PSX_MAIN_RAM_MIRROR_BYTES < JIT_PSX_MAIN_RAM_SIZE_BYTES
#error "JIT_PSX_MAIN_RAM_MIRROR_BYTES must be at least JIT_PSX_MAIN_RAM_SIZE_BYTES"
#endif
#endif

#ifndef JIT_FALLBACK_ON_INIT_ALLOC_FAILURE
/* Keep the emulator usable on IRAM/PSRAM-constrained builds by falling back to
 * interpreter execution if the native cache cannot be allocated.
 */
#define JIT_FALLBACK_ON_INIT_ALLOC_FAILURE 1
#endif

#ifndef JIT_STRICT_MEMRLUT_NULL_CHECKS
#define JIT_STRICT_MEMRLUT_NULL_CHECKS 1
#endif

#ifndef JIT_STRICT_ALIGNMENT_CHECKS
#define JIT_STRICT_ALIGNMENT_CHECKS 1
#endif

#ifndef JIT_BLOCK_CYCLE_SHIFT
#define JIT_BLOCK_CYCLE_SHIFT 0
#endif

#if JIT_BLOCK_CYCLE_SHIFT < 0 || JIT_BLOCK_CYCLE_SHIFT >= 32
#error "JIT_BLOCK_CYCLE_SHIFT must be in the range 0..31"
#endif

#ifndef JIT_UNUSED
#define JIT_UNUSED(x) ((void)(x))
#endif

#ifdef JIT_XTENSA_STANDALONE_TEST

typedef struct psxRegisters {
    struct {
        uint32_t r[34];
    } GPR;
    uint32_t pc;
    uint32_t cycle;
    uint32_t stop;
    struct {
        uint8_t *psxM;
    } ptrs;
} psxRegisters;

typedef struct R3000Acpu {
    int  (*Init)(void);
    void (*Reset)(void);
    void (*Execute)(psxRegisters *regs);
    void (*ExecuteBlock)(psxRegisters *regs);
    void (*Clear)(uint32_t addr, uint32_t size);
    void (*Notify)(int note, void *data);
    void (*ApplyConfig)(void);
    void (*Shutdown)(void);
} R3000Acpu;

static inline uint32_t jit_standalone_fetch32(psxRegisters *regs, uint32_t pc) {
    JIT_UNUSED(regs);
    JIT_UNUSED(pc);
    return 0u;
}

static inline void jit_standalone_execI(psxRegisters *regs) {
    regs->pc += 4u;
}

static inline void **jit_standalone_memrlut(psxRegisters *regs) {
    static void *dummy[0x10000];
    JIT_UNUSED(regs);
    return dummy;
}

static inline void **jit_standalone_memwlut(psxRegisters *regs) {
    return jit_standalone_memrlut(regs);
}

#ifndef JIT_FETCH32
#define JIT_FETCH32(regs, pc) jit_standalone_fetch32((regs), (pc))
#endif
#ifndef JIT_EXEC_INTERPRETER_ONE
#define JIT_EXEC_INTERPRETER_ONE(regs) jit_standalone_execI((regs))
#endif
#ifndef JIT_GET_MEMRLUT
#define JIT_GET_MEMRLUT(regs) jit_standalone_memrlut((regs))
#endif
#ifndef JIT_GET_MEMWLUT
#define JIT_GET_MEMWLUT(regs) jit_standalone_memwlut((regs))
#endif

#endif /* JIT_XTENSA_STANDALONE_TEST */

#ifndef JIT_FETCH32
#if JIT_FETCH32_TAKES_REGS
#define JIT_FETCH32(regs, pc) psxMemRead32((regs), (pc))
#else
extern uint32_t psxMemRead32(uint32_t addr);
#define JIT_FETCH32(regs, pc) (JIT_UNUSED(regs), psxMemRead32((pc)))
#endif
#endif

#ifndef JIT_EXEC_INTERPRETER_ONE
#if JIT_USE_PSXINT_EXECUTEBLOCK_FALLBACK
void jit_psx_interpreter_execute_fallback(psxRegisters *regs);
#define JIT_EXEC_INTERPRETER_ONE(regs) jit_psx_interpreter_execute_fallback((regs))
#else
extern void execI(psxRegisters *regs);
#define JIT_EXEC_INTERPRETER_ONE(regs) execI((regs))
#endif
#endif

#ifndef JIT_GET_MEMRLUT
#if JIT_USE_REGS_PTRS_MEMLUT
#ifndef JIT_PSX_PTRS_MEMRLUT_OFFSET
#define JIT_PSX_PTRS_MEMRLUT_OFFSET offsetof(psxRegisters, ptrs.memRLUT)
#endif
static inline void **jit_regs_memrlut(psxRegisters *regs) {
    void *p = NULL;
    if (regs == NULL) return NULL;
    memcpy(&p, (const uint8_t *)(const void *)regs + JIT_PSX_PTRS_MEMRLUT_OFFSET, sizeof(p));
    return (void **)p;
}
#define JIT_GET_MEMRLUT(regs) jit_regs_memrlut((regs))
#else
extern void **psxMemRLUT;
#define JIT_GET_MEMRLUT(regs) (JIT_UNUSED(regs), (void **)psxMemRLUT)
#endif
#endif

#ifndef JIT_GET_MEMWLUT
#if JIT_USE_REGS_PTRS_MEMLUT && JIT_HAS_MEMWLUT
#ifndef JIT_PSX_PTRS_MEMWLUT_OFFSET
#define JIT_PSX_PTRS_MEMWLUT_OFFSET offsetof(psxRegisters, ptrs.memWLUT)
#endif
static inline void **jit_regs_memwlut(psxRegisters *regs) {
    void *p = NULL;
    if (regs == NULL) return NULL;
    memcpy(&p, (const uint8_t *)(const void *)regs + JIT_PSX_PTRS_MEMWLUT_OFFSET, sizeof(p));
    return (void **)p;
}
#define JIT_GET_MEMWLUT(regs) jit_regs_memwlut((regs))
#elif JIT_HAS_MEMWLUT
extern void **psxMemWLUT;
#define JIT_GET_MEMWLUT(regs) (JIT_UNUSED(regs), (void **)psxMemWLUT)
#else
#define JIT_GET_MEMWLUT(regs) JIT_GET_MEMRLUT((regs))
#endif
#endif

#if JIT_CHECK_PTRS_INTFETCH
#ifndef JIT_PSX_PTRS_INTFETCH_OFFSET
#define JIT_PSX_PTRS_INTFETCH_OFFSET offsetof(psxRegisters, ptrs.intFetch)
#endif
static inline void *jit_regs_intfetch(psxRegisters *regs) {
    void *p = NULL;
    if (regs == NULL) return NULL;
    memcpy(&p, (const uint8_t *)(const void *)regs + JIT_PSX_PTRS_INTFETCH_OFFSET, sizeof(p));
    return p;
}
#endif

#ifndef JIT_PSX_GPR_OFFSET
#define JIT_PSX_GPR_OFFSET offsetof(psxRegisters, GPR.r)
#endif

#ifndef JIT_PSX_PC_OFFSET
#define JIT_PSX_PC_OFFSET offsetof(psxRegisters, pc)
#endif

#ifndef JIT_PSX_CYCLE_OFFSET
#define JIT_PSX_CYCLE_OFFSET offsetof(psxRegisters, cycle)
#endif

#ifndef JIT_PSX_STOP_OFFSET
#define JIT_PSX_STOP_OFFSET offsetof(psxRegisters, stop)
#endif

#ifndef JIT_PSX_PTRS_PSXM_OFFSET
#define JIT_PSX_PTRS_PSXM_OFFSET offsetof(psxRegisters, ptrs.psxM)
#endif

#define JIT_MIPS_GPR_OFFSET(reg_index) (JIT_PSX_GPR_OFFSET + ((size_t)(reg_index) * 4u))
#define JIT_MIPS_HI_OFFSET             (JIT_PSX_GPR_OFFSET + 128u)
#define JIT_MIPS_LO_OFFSET             (JIT_PSX_GPR_OFFSET + 132u)

typedef void (*jit_native_block_fn)(psxRegisters *regs, void **memrlut);

typedef enum JitCompileStatus {
    JIT_COMPILE_OK = 0,
    JIT_COMPILE_UNSUPPORTED,
    JIT_COMPILE_FULL,
    JIT_COMPILE_BAD_ENCODING
} JitCompileStatus;

typedef struct JitBlock {
    uint32_t pc;
    uint32_t end_pc;
    uint32_t code_bytes;
    uint32_t mips_insns;
    uint32_t generation;
    uint8_t *block_base;
    uint8_t *entry;
    bool valid;
} JitBlock;

typedef struct JitRuntime {
    uint8_t *code_base;
    uint8_t *code_cur;
    uint8_t *code_end;
    size_t code_size;

    uint8_t *staging_base;
    size_t staging_size;
    uint8_t *pending_exec_base;
    size_t pending_reserve_bytes;

    uint32_t generation;
    uint32_t compile_failures;
    uint32_t compile_successes;
    uint32_t interpreter_fallbacks;
    uint32_t native_blocks_executed;
    uint32_t code_cache_resets;
    uint32_t invalidated_blocks;
    uint32_t bailouts;
    uint32_t cache_hits;
    uint32_t cache_misses;
    size_t code_high_water;
    JitBlock *blocks;
    size_t block_count;
    size_t block_table_bytes;
} JitRuntime;

typedef struct JitStatsSnapshot {
    size_t code_size;
    size_t code_used;
    size_t staging_size;
    uint32_t native_enabled;
    uint32_t native_available;
    uint32_t generation;
    uint32_t compile_failures;
    uint32_t compile_successes;
    uint32_t interpreter_fallbacks;
    uint32_t native_blocks_executed;
    uint32_t code_cache_resets;
    uint32_t invalidated_blocks;
    uint32_t bailouts;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t live_blocks;
    size_t code_high_water;
    size_t block_slots;
    size_t block_table_bytes;
} JitStatsSnapshot;

extern JitRuntime g_jit;
extern volatile uint32_t g_jit_store_flag;
extern volatile uint32_t g_jit_last_store_addr;
extern volatile uint32_t g_jit_bail_flag;
extern void **g_jit_mem_wlut;

int jit_init(void);
void jit_reset(void);
void jit_shutdown(void);
void jit_execute(psxRegisters *regs);
void jit_execute_block(psxRegisters *regs);
void jit_clear(uint32_t addr, uint32_t size);
void jit_notify(int note, void *data);
void jit_apply_config(void);
void jit_before_interpreter_fallback(psxRegisters *regs);

void *jit_compile_block(psxRegisters *regs, uint32_t pc);
void *jit_code_reserve(size_t bytes, size_t align);
void *jit_code_commit(void *start, size_t bytes);
void jit_code_rollback(void *start);
uint8_t *jit_code_pending_exec_base(const void *start);
void jit_get_stats(JitStatsSnapshot *out);
JitBlock *jit_lookup_block(uint32_t pc);
JitBlock *jit_install_block(uint32_t pc, uint32_t end_pc, uint8_t *base, uint8_t *entry,
                            uint32_t code_bytes, uint32_t mips_insns);
void jit_invalidate_all(void);
void jit_invalidate_addr(uint32_t addr, uint32_t size);
void jit_note_store32(uint32_t addr);

static inline void jit_notify_psx_write(uint32_t addr, uint32_t size) {
    jit_invalidate_addr(addr, size);
}

#define JIT_NOTIFY_WRITE8(addr)  jit_notify_psx_write((uint32_t)(addr), 1u)
#define JIT_NOTIFY_WRITE16(addr) jit_notify_psx_write((uint32_t)(addr), 2u)
#define JIT_NOTIFY_WRITE32(addr) jit_notify_psx_write((uint32_t)(addr), 4u)
#define JIT_NOTIFY_RANGE(addr, size) jit_notify_psx_write((uint32_t)(addr), (uint32_t)(size))

extern R3000Acpu psxJit;

#ifdef __cplusplus
}
#endif

#endif /* JIT_XTENSA_H */
