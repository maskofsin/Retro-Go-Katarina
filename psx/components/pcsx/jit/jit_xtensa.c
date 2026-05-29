#include "jit_xtensa.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <string.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#ifndef MALLOC_CAP_32BIT
#define MALLOC_CAP_32BIT 0u
#endif
#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define JIT_HAVE_ESP_CACHE 1
#ifndef ESP_CACHE_MSYNC_FLAG_INVALIDATE
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 0
#endif
#ifndef ESP_CACHE_MSYNC_FLAG_UNALIGNED
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 0
#endif
#ifndef ESP_CACHE_MSYNC_FLAG_DIR_C2M
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 0
#endif
#ifndef ESP_CACHE_MSYNC_FLAG_TYPE_INST
#define ESP_CACHE_MSYNC_FLAG_TYPE_INST 0
#endif
#else
#define JIT_HAVE_ESP_CACHE 0
#endif
#else
#define JIT_HAVE_ESP_CACHE 0
#endif

#if !JIT_NATIVE_ENABLED && (defined(__GNUC__) || defined(__clang__))
#define JIT_ATTR_NATIVE_ONLY __attribute__((unused))
#else
#define JIT_ATTR_NATIVE_ONLY
#endif

JitRuntime g_jit;
volatile uint32_t g_jit_store_flag;
volatile uint32_t g_jit_last_store_addr;
volatile uint32_t g_jit_bail_flag;
void **g_jit_mem_wlut;


#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void jit_before_interpreter_fallback(psxRegisters *regs) {
    JIT_UNUSED(regs);
}

static void jit_interpreter_fallback_one(psxRegisters *regs) {
    jit_before_interpreter_fallback(regs);
    JIT_EXEC_INTERPRETER_ONE(regs);
}

static uint32_t jit_hash_pc(uint32_t pc) {
    uint32_t x = pc >> 2;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    size_t count = g_jit.block_count != 0u ? g_jit.block_count : (size_t)JIT_BLOCK_HASH_SIZE;
    return x & (uint32_t)(count - 1u);
}


static bool jit_ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
    return a0 < b1 && b0 < a1;
}

#if JIT_NORMALIZE_MAIN_RAM_MIRRORS
static bool jit_addr_is_main_ram_mirror(uint32_t addr) {
    return (addr & (uint32_t)JIT_PSX_PHYS_MASK) < (uint32_t)JIT_PSX_MAIN_RAM_MIRROR_BYTES;
}
#endif

static uint32_t jit_block_len_bytes(const JitBlock *b) {
    uint32_t len = b->end_pc - b->pc;
    uint32_t max_len = (uint32_t)(JIT_MAX_MIPS_INSNS_PER_BLOCK * 8u + 16u);
    if (len == 0u || len > max_len) len = 4u;
    return len;
}

#if JIT_NORMALIZE_MAIN_RAM_MIRRORS
static bool jit_mod_ranges_overlap(uint32_t a0, uint32_t alen, uint32_t b0, uint32_t blen, uint32_t mod) {
    if (alen == 0u) alen = 1u;
    if (blen == 0u) blen = 1u;
    if (alen >= mod || blen >= mod) return true;

    uint32_t a1 = a0 + alen;
    uint32_t b1 = b0 + blen;
    if (a1 <= mod && b1 <= mod) {
        return jit_ranges_overlap(a0, a1, b0, b1);
    }
    if (a1 > mod) {
        uint32_t aw = a1 - mod;
        return jit_ranges_overlap(a0, mod, b0, b1 <= mod ? b1 : mod) ||
               jit_mod_ranges_overlap(0u, aw, b0, blen, mod);
    }
    uint32_t bw = b1 - mod;
    return jit_ranges_overlap(a0, a1, b0, mod) ||
           jit_mod_ranges_overlap(a0, alen, 0u, bw, mod);
}
#endif

static bool jit_block_overlaps_write(const JitBlock *b, uint32_t addr, uint32_t size) {
    if (size == 0u) size = 4u;
#if JIT_NORMALIZE_MAIN_RAM_MIRRORS
    if (jit_addr_is_main_ram_mirror(addr) && jit_addr_is_main_ram_mirror(b->pc)) {
        uint32_t mod = (uint32_t)JIT_PSX_MAIN_RAM_SIZE_BYTES;
        uint32_t a0 = (addr & (uint32_t)JIT_PSX_PHYS_MASK) & (mod - 1u);
        uint32_t b0 = (b->pc & (uint32_t)JIT_PSX_PHYS_MASK) & (mod - 1u);
        return jit_mod_ranges_overlap(a0, size, b0, jit_block_len_bytes(b), mod);
    }
#endif
    uint32_t a0 = addr & (uint32_t)JIT_PSX_PHYS_MASK;
    uint32_t a1 = a0 + size;
    if (a1 < a0) a1 = UINT32_MAX;
    uint32_t b0 = b->pc & (uint32_t)JIT_PSX_PHYS_MASK;
    uint32_t b1 = b0 + jit_block_len_bytes(b);
    if (b1 < b0) b1 = UINT32_MAX;
    return jit_ranges_overlap(a0, a1, b0, b1);
}

static uintptr_t jit_align_up_uintptr(uintptr_t v, size_t align) {
    if (align == 0u) return v;
    uintptr_t mask = (uintptr_t)align - 1u;
    return (v + mask) & ~mask;
}

static size_t jit_align_up_size(size_t v, size_t align) {
    if (align == 0u) return v;
    size_t mask = align - 1u;
    return (v + mask) & ~mask;
}

static size_t jit_align_down_size(size_t v, size_t align) {
    if (align == 0u) return v;
    return v & ~(align - 1u);
}

static size_t jit_power2_or_default(size_t value, size_t fallback) {
    if (value != 0u && (value & (value - 1u)) == 0u) return value;
    return fallback;
}

static JIT_ATTR_NATIVE_ONLY void *jit_alloc_exec(size_t size) {
    size_t exec_align = jit_power2_or_default((size_t)JIT_EXEC_ALIGN_BYTES, 4u);
    size_t sync_align = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    size_t align = exec_align > sync_align ? exec_align : sync_align;
    size_t rounded = jit_align_up_size(size, align);
#if defined(ESP_PLATFORM)
    void *p = heap_caps_aligned_alloc(align, rounded,
        MALLOC_CAP_EXEC | MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
    if (p == NULL)
        p = heap_caps_aligned_alloc(align, rounded,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    if (p == NULL)
        p = heap_caps_aligned_alloc(align, rounded, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (p == NULL)
        p = heap_caps_aligned_alloc(align, rounded, MALLOC_CAP_EXEC);
    return p;
#else
    void *p = aligned_alloc(align, rounded);
    if (p != NULL) return p;
    return malloc(rounded);
#endif
}

static void jit_free_exec(void *p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(p);
#endif
}

static JIT_ATTR_NATIVE_ONLY void *jit_alloc_staging(size_t size) {
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (p != NULL) return p;
#endif
    return malloc(size);
}

static void jit_free_staging(void *p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(p);
#endif
}

static JIT_ATTR_NATIVE_ONLY JitBlock *jit_alloc_block_table(size_t count, size_t *bytes_out) {
    size_t bytes = count * sizeof(JitBlock);
    if (bytes_out != NULL) *bytes_out = bytes;
    if (count == 0u || bytes / sizeof(JitBlock) != count) return NULL;
#if defined(ESP_PLATFORM)
#if JIT_BLOCK_TABLE_IN_SPIRAM
    JitBlock *blocks = (JitBlock *)heap_caps_calloc(count, sizeof(JitBlock),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (blocks != NULL) return blocks;
#endif
    return (JitBlock *)heap_caps_calloc(count, sizeof(JitBlock), MALLOC_CAP_8BIT);
#else
    return (JitBlock *)calloc(count, sizeof(JitBlock));
#endif
}

static JIT_ATTR_NATIVE_ONLY void jit_free_block_table(JitBlock *blocks) {
#if defined(ESP_PLATFORM)
    heap_caps_free(blocks);
#else
    free(blocks);
#endif
}

#if defined(ESP_PLATFORM)
static size_t jit_largest_exec_block(void) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    if (largest == 0u) {
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    }
    if (largest == 0u) {
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
    }
    return largest;
}
#endif

static JIT_ATTR_NATIVE_ONLY int jit_init_alloc_failure(void) {
#if JIT_FALLBACK_ON_INIT_ALLOC_FAILURE
    memset(&g_jit, 0, sizeof(g_jit));
    g_jit.generation = 1u;
    g_jit_store_flag = 0u;
    g_jit_last_store_addr = 0u;
    g_jit_bail_flag = 0u;
    g_jit_mem_wlut = NULL;
    return 0;
#else
    memset(&g_jit, 0, sizeof(g_jit));
    g_jit.generation = 1u;
    g_jit_store_flag = 0u;
    g_jit_last_store_addr = 0u;
    g_jit_bail_flag = 0u;
    g_jit_mem_wlut = NULL;
    return -1;
#endif
}

static JIT_ATTR_NATIVE_ONLY size_t jit_choose_code_size(void) {
    size_t exec_align = jit_power2_or_default((size_t)JIT_EXEC_ALIGN_BYTES, 4u);
    size_t sync_align = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    size_t alloc_align = exec_align > sync_align ? exec_align : sync_align;
    size_t want = jit_align_down_size((size_t)JIT_CODE_BUFFER_SIZE, alloc_align);
    size_t min_size = jit_align_up_size((size_t)JIT_MIN_CODE_BUFFER_SIZE, alloc_align);
#if defined(ESP_PLATFORM)
    size_t largest = jit_largest_exec_block();
    if (largest > (size_t)JIT_EXEC_IRAM_HEADROOM_BYTES) {
        size_t usable = largest - (size_t)JIT_EXEC_IRAM_HEADROOM_BYTES;
        if (usable < want) want = usable;
    } else if (largest != 0u && largest < want) {
        want = largest;
    }
#endif
    want = jit_align_down_size(want, alloc_align);
    if (want < min_size) return 0u;
    return want;
}

static void jit_flush_code(void *start, size_t bytes) {
    if (start == NULL || bytes == 0u) return;

#if JIT_HAVE_ESP_CACHE
    /*
     * ESP-IDF 5.x exposes esp_cache_msync(), but not every supported minor
     * exposes a public cache-line-size query. Use a configurable alignment
     * instead of calling a private/unstable helper. For ESP32-S3 this profile
     * uses 64 bytes; overriding JIT_CACHE_LINE_BYTES is safe if your SDK or
     * Kconfig reports a different instruction-cache line size.
     */
    size_t line = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    if (line != 0u && (line & (line - 1u)) == 0u) {
        uintptr_t begin = (uintptr_t)start & ~((uintptr_t)line - 1u);
        uintptr_t end = jit_align_up_uintptr((uintptr_t)start + bytes, line);
        (void)esp_cache_msync((void *)begin, (size_t)(end - begin),
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                              ESP_CACHE_MSYNC_FLAG_TYPE_INST |
                              ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    } else {
        (void)esp_cache_msync(start, bytes,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                              ESP_CACHE_MSYNC_FLAG_TYPE_INST |
                              ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                              ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
#endif

#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)start, (char *)start + bytes);
#else
    JIT_UNUSED(start);
    JIT_UNUSED(bytes);
#endif
}

static bool jit_copy_to_exec(uint8_t *dst, const uint8_t *src, size_t bytes) {
    if (dst == NULL || src == NULL) return false;
    if ((((uintptr_t)dst) & 3u) != 0u) return false;

#if defined(ESP_PLATFORM)
    volatile uint32_t *dw = (volatile uint32_t *)(void *)dst;
    size_t full_words = bytes / 4u;
    for (size_t i = 0; i < full_words; ++i) {
        uint32_t w;
        memcpy(&w, src + (i * 4u), sizeof(w));
        dw[i] = w;
    }
    size_t rem = bytes & 3u;
    if (rem != 0u) {
        uint32_t w = 0u;
        memcpy(&w, src + (full_words * 4u), rem);
        dw[full_words] = w;
    }
#else
    memcpy(dst, src, bytes);
#endif
    return true;
}

static void jit_zero_exec_range(uint8_t *dst, size_t bytes) {
    if (dst == NULL || bytes == 0u) return;
#if defined(ESP_PLATFORM)
    if ((((uintptr_t)dst) & 3u) != 0u) return;
    volatile uint32_t *dw = (volatile uint32_t *)(void *)dst;
    size_t words = bytes / 4u;
    for (size_t i = 0; i < words; ++i) {
        dw[i] = 0u;
    }
#else
    memset(dst, 0, bytes);
#endif
}

int jit_init(void) {
    if (g_jit.code_base != NULL || g_jit.staging_base != NULL || g_jit.blocks != NULL) {
        jit_shutdown();
    }
    memset(&g_jit, 0, sizeof(g_jit));
    g_jit.generation = 1u;
    g_jit_store_flag = 0u;
    g_jit_last_store_addr = 0u;
    g_jit_bail_flag = 0u;
    g_jit_mem_wlut = NULL;

#if !JIT_NATIVE_ENABLED
    printf("JIT: NATIVE DISABLED\n");
    return 0;
#else
    printf("JIT: NATIVE ENABLED, largest exec=%zu\n", jit_largest_exec_block());

    g_jit.block_count = (size_t)JIT_BLOCK_HASH_SIZE;
    g_jit.blocks = jit_alloc_block_table(g_jit.block_count, &g_jit.block_table_bytes);
    if (g_jit.blocks == NULL) {
        return jit_init_alloc_failure();
    }

    size_t exec_align = jit_power2_or_default((size_t)JIT_EXEC_ALIGN_BYTES, 4u);
    size_t sync_align = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    size_t alloc_align = exec_align > sync_align ? exec_align : sync_align;
    size_t code_size = jit_choose_code_size();
    size_t min_size = jit_align_up_size((size_t)JIT_MIN_CODE_BUFFER_SIZE, alloc_align);
    while (code_size >= min_size && code_size != 0u) {
        g_jit.code_base = (uint8_t *)jit_alloc_exec(code_size);
        if (g_jit.code_base != NULL) break;
        code_size = jit_align_down_size(code_size / 2u, alloc_align);
    }
    if (g_jit.code_base == NULL) {
        jit_free_block_table(g_jit.blocks);
        return jit_init_alloc_failure();
    }

    g_jit.staging_size = jit_align_up_size((size_t)JIT_STAGING_BUFFER_SIZE, 4u);
    g_jit.staging_base = (uint8_t *)jit_alloc_staging(g_jit.staging_size);
    if (g_jit.staging_base == NULL) {
        jit_free_exec(g_jit.code_base);
        jit_free_block_table(g_jit.blocks);
        return jit_init_alloc_failure();
    }

    g_jit.code_size = code_size;
    g_jit.code_cur = g_jit.code_base;
    g_jit.code_end = g_jit.code_base + g_jit.code_size;
    return 0;
#endif
}

void jit_invalidate_all(void) {
    if (g_jit.blocks != NULL) {
        for (size_t i = 0; i < g_jit.block_count; ++i) {
            g_jit.blocks[i].valid = false;
        }
    }
    if (++g_jit.generation == 0u) {
        g_jit.generation = 1u;
    }
}

void jit_reset(void) {
    jit_invalidate_all();
    if (g_jit.code_base != NULL) {
        g_jit.code_cur = g_jit.code_base;
    }
    g_jit.pending_exec_base = NULL;
    g_jit.pending_reserve_bytes = 0u;
    g_jit.compile_failures = 0u;
    g_jit.compile_successes = 0u;
    g_jit.interpreter_fallbacks = 0u;
    g_jit.native_blocks_executed = 0u;
    g_jit.code_cache_resets = 0u;
    g_jit.invalidated_blocks = 0u;
    g_jit.bailouts = 0u;
    g_jit.cache_hits = 0u;
    g_jit.cache_misses = 0u;
    g_jit.code_high_water = 0u;
    g_jit_store_flag = 0u;
    g_jit_last_store_addr = 0u;
    g_jit_bail_flag = 0u;
    g_jit_mem_wlut = NULL;
}

void jit_shutdown(void) {
    if (g_jit.staging_base != NULL) {
        jit_free_staging(g_jit.staging_base);
    }
    if (g_jit.code_base != NULL) {
        jit_free_exec(g_jit.code_base);
    }
    if (g_jit.blocks != NULL) {
        jit_free_block_table(g_jit.blocks);
    }
    memset(&g_jit, 0, sizeof(g_jit));
    g_jit_store_flag = 0u;
    g_jit_last_store_addr = 0u;
    g_jit_bail_flag = 0u;
    g_jit_mem_wlut = NULL;
}

void *jit_code_reserve(size_t bytes, size_t align) {
    if (g_jit.code_base == NULL || g_jit.staging_base == NULL) return NULL;
    if (bytes == 0u || bytes > g_jit.staging_size) return NULL;
    if (align < 4u) align = 4u;
    if (align < (size_t)JIT_EXEC_ALIGN_BYTES) align = (size_t)JIT_EXEC_ALIGN_BYTES;
    if ((align & (align - 1u)) != 0u) return NULL;

    size_t committed_room = jit_align_up_size(bytes, 4u);
    size_t sync_align = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    uintptr_t cur = jit_align_up_uintptr((uintptr_t)g_jit.code_cur, align);
    uintptr_t end = cur + committed_room;
    uintptr_t flush_end = jit_align_up_uintptr(end, sync_align);
    if (end < cur || flush_end < end || flush_end > (uintptr_t)g_jit.code_end) {
        ++g_jit.code_cache_resets;
        jit_invalidate_all();
        g_jit.code_cur = g_jit.code_base;
        cur = jit_align_up_uintptr((uintptr_t)g_jit.code_cur, align);
        end = cur + committed_room;
        flush_end = jit_align_up_uintptr(end, sync_align);
        if (end < cur || flush_end < end || flush_end > (uintptr_t)g_jit.code_end) {
            return NULL;
        }
    }

    g_jit.pending_exec_base = (uint8_t *)cur;
    g_jit.pending_reserve_bytes = bytes;
    return g_jit.staging_base;
}

uint8_t *jit_code_pending_exec_base(const void *start) {
    if (start != (const void *)g_jit.staging_base) return NULL;
    return g_jit.pending_exec_base;
}

void *jit_code_commit(void *start, size_t bytes) {
    if (start == NULL || start != (void *)g_jit.staging_base) return NULL;
    if (g_jit.pending_exec_base == NULL) return NULL;
    if (bytes == 0u || bytes > g_jit.pending_reserve_bytes) return NULL;

    size_t committed = jit_align_up_size(bytes, 4u);
    size_t sync_align = jit_power2_or_default((size_t)JIT_CACHE_SYNC_ALIGN_BYTES, 64u);
    uint8_t *exec_base = g_jit.pending_exec_base;
    uintptr_t end = (uintptr_t)exec_base + committed;
    uintptr_t flush_end = jit_align_up_uintptr(end, sync_align);
    if (end < (uintptr_t)exec_base || flush_end < end || flush_end > (uintptr_t)g_jit.code_end) {
        g_jit.pending_exec_base = NULL;
        g_jit.pending_reserve_bytes = 0u;
        return NULL;
    }
    if (committed > bytes) {
        memset((uint8_t *)start + bytes, 0, committed - bytes);
    }
    if (!jit_copy_to_exec(exec_base, (const uint8_t *)start, committed)) {
        g_jit.pending_exec_base = NULL;
        g_jit.pending_reserve_bytes = 0u;
        return NULL;
    }
    if (flush_end > end) {
        jit_zero_exec_range((uint8_t *)end, (size_t)(flush_end - end));
    }

    jit_flush_code(exec_base, (size_t)(flush_end - (uintptr_t)exec_base));
    g_jit.code_cur = (uint8_t *)flush_end;
    if (g_jit.code_base != NULL && g_jit.code_cur >= g_jit.code_base) {
        size_t used = (size_t)(g_jit.code_cur - g_jit.code_base);
        if (used > g_jit.code_high_water) g_jit.code_high_water = used;
    }
    g_jit.pending_exec_base = NULL;
    g_jit.pending_reserve_bytes = 0u;
    return exec_base;
}

void jit_code_rollback(void *start) {
    if (start != NULL && start == (void *)g_jit.staging_base) {
        g_jit.pending_exec_base = NULL;
        g_jit.pending_reserve_bytes = 0u;
    }
}

static JitBlock *jit_find_block(uint32_t pc, bool update_stats) {
    if (g_jit.blocks == NULL || g_jit.block_count == 0u) {
        if (update_stats) ++g_jit.cache_misses;
        return NULL;
    }
    uint32_t h = jit_hash_pc(pc);
    uint32_t mask = (uint32_t)(g_jit.block_count - 1u);
    for (uint32_t n = 0; n < (uint32_t)g_jit.block_count; ++n) {
        JitBlock *b = &g_jit.blocks[(h + n) & mask];
        if (b->valid && b->pc == pc && b->generation == g_jit.generation) {
            if (update_stats) ++g_jit.cache_hits;
            return b;
        }
    }
    if (update_stats) ++g_jit.cache_misses;
    return NULL;
}

JitBlock *jit_lookup_block(uint32_t pc) {
    return jit_find_block(pc, true);
}

JitBlock *jit_install_block(uint32_t pc, uint32_t end_pc, uint8_t *base, uint8_t *entry,
                            uint32_t code_bytes, uint32_t mips_insns) {
    if (g_jit.blocks == NULL || g_jit.block_count == 0u) return NULL;
    uint32_t h = jit_hash_pc(pc);
    uint32_t mask = (uint32_t)(g_jit.block_count - 1u);
    JitBlock *slot = NULL;
    for (uint32_t n = 0; n < (uint32_t)g_jit.block_count; ++n) {
        JitBlock *b = &g_jit.blocks[(h + n) & mask];
        if (b->valid && b->pc == pc) {
            slot = b;
            break;
        }
        if (!b->valid && slot == NULL) {
            slot = b;
        }
    }
    if (slot == NULL) {
        jit_invalidate_all();
        slot = &g_jit.blocks[h & mask];
    }
    slot->pc = pc;
    slot->end_pc = end_pc;
    slot->block_base = base;
    slot->entry = entry;
    slot->code_bytes = code_bytes;
    slot->mips_insns = mips_insns;
    slot->generation = g_jit.generation;
    slot->valid = true;
    ++g_jit.compile_successes;
    return slot;
}

void jit_invalidate_addr(uint32_t addr, uint32_t size) {
    if (size == 0u) size = 4u;
    if (g_jit.blocks == NULL) return;
    for (size_t i = 0; i < g_jit.block_count; ++i) {
        JitBlock *b = &g_jit.blocks[i];
        if (!b->valid) continue;
        if (jit_block_overlaps_write(b, addr, size)) {
            b->valid = false;
            ++g_jit.invalidated_blocks;
        }
    }
}

void jit_clear(uint32_t addr, uint32_t size) {
    jit_invalidate_addr(addr, size);
}

void jit_note_store32(uint32_t addr) {
    g_jit_last_store_addr = addr;
    g_jit_store_flag = 1u;
}

static void jit_handle_store_flag(void) {
    if (g_jit_store_flag != 0u) {
        uint32_t addr = g_jit_last_store_addr;
        g_jit_store_flag = 0u;
        jit_invalidate_addr(addr, 4u);
    }
}

#if JIT_NATIVE_ENABLED
static bool jit_handle_bail_flag(psxRegisters *regs) {
    if (g_jit_bail_flag == 0u) return false;
    g_jit_bail_flag = 0u;
    ++g_jit.bailouts;
    ++g_jit.interpreter_fallbacks;
    jit_invalidate_addr(regs->pc, 4u);
    jit_interpreter_fallback_one(regs);
    return true;
}
#endif

void jit_get_stats(JitStatsSnapshot *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->code_size = g_jit.code_size;
    if (g_jit.code_base != NULL && g_jit.code_cur >= g_jit.code_base) {
        out->code_used = (size_t)(g_jit.code_cur - g_jit.code_base);
    }
    out->staging_size = g_jit.staging_size;
    out->native_enabled = (uint32_t)JIT_NATIVE_ENABLED;
    out->native_available = (uint32_t)(JIT_NATIVE_ENABLED && g_jit.code_base != NULL &&
                                      g_jit.staging_base != NULL && g_jit.blocks != NULL);
    out->generation = g_jit.generation;
    out->compile_failures = g_jit.compile_failures;
    out->compile_successes = g_jit.compile_successes;
    out->interpreter_fallbacks = g_jit.interpreter_fallbacks;
    out->native_blocks_executed = g_jit.native_blocks_executed;
    out->code_cache_resets = g_jit.code_cache_resets;
    out->invalidated_blocks = g_jit.invalidated_blocks;
    out->bailouts = g_jit.bailouts;
    out->code_high_water = g_jit.code_high_water;
    out->cache_hits = g_jit.cache_hits;
    out->cache_misses = g_jit.cache_misses;
    out->block_slots = g_jit.block_count;
    out->block_table_bytes = g_jit.block_table_bytes;
    if (g_jit.blocks != NULL) {
        for (size_t i = 0; i < g_jit.block_count; ++i) {
            if (g_jit.blocks[i].valid && g_jit.blocks[i].generation == g_jit.generation) {
                ++out->live_blocks;
            }
        }
    }
}

#if JIT_NATIVE_ENABLED
static void jit_run_native_block(JitBlock *block, psxRegisters *regs) {
    jit_native_block_fn fn = (jit_native_block_fn)(void *)block->entry;
    g_jit_mem_wlut = JIT_GET_MEMWLUT(regs);
    ++g_jit.native_blocks_executed;
    fn(regs, JIT_GET_MEMRLUT(regs));
}

static JitBlock *jit_get_or_compile(psxRegisters *regs, uint32_t pc) {
    JitBlock *block = jit_lookup_block(pc);
    if (block != NULL) return block;

    void *entry = jit_compile_block(regs, pc);
    if (entry == NULL) {
        ++g_jit.compile_failures;
        return NULL;
    }
    return jit_find_block(pc, false);
}
#endif

void jit_execute(psxRegisters *regs) {
    if (regs == NULL) return;

#if !JIT_NATIVE_ENABLED
    while (!regs->stop) {
        ++g_jit.interpreter_fallbacks;
        jit_interpreter_fallback_one(regs);
        jit_handle_store_flag();
    }
#else
    while (!regs->stop) {
        uint32_t pc = regs->pc;
#ifdef ESP_PLATFORM
        if ((g_jit.interpreter_fallbacks & 0x7ff) == 0)
            vTaskDelay(0);
#endif
        JitBlock *block = jit_get_or_compile(regs, pc);
        if (block == NULL) {
            ++g_jit.interpreter_fallbacks;
            jit_interpreter_fallback_one(regs);
            jit_handle_store_flag();
            continue;
        }
        jit_run_native_block(block, regs);
        if (jit_handle_bail_flag(regs)) {
            jit_handle_store_flag();
            continue;
        }
        jit_handle_store_flag();
    }
#endif
}

void jit_execute_block(psxRegisters *regs) {
    if (regs == NULL) return;

#if !JIT_NATIVE_ENABLED
    ++g_jit.interpreter_fallbacks;
    jit_interpreter_fallback_one(regs);
    jit_handle_store_flag();
#else
    JitBlock *block = jit_get_or_compile(regs, regs->pc);
    if (block == NULL) {
        ++g_jit.interpreter_fallbacks;
        jit_interpreter_fallback_one(regs);
    } else {
        jit_run_native_block(block, regs);
        (void)jit_handle_bail_flag(regs);
    }
    jit_handle_store_flag();
#endif
}

void jit_notify(int note, void *data) {
    JIT_UNUSED(note);
    JIT_UNUSED(data);
    /* Blocks spill hot MIPS registers before every native return. */
}

void jit_apply_config(void) {
    /* Timing knobs can be wired here once the emulator timing model is known. */
}

#if !JIT_XTENSA_USE_WRAPPER
R3000Acpu psxJit = {
    .Init = jit_init,
    .Reset = jit_reset,
    .Execute = jit_execute,
    .ExecuteBlock = jit_execute_block,
    .Clear = jit_clear,
    .Notify = jit_notify,
    .ApplyConfig = jit_apply_config,
    .Shutdown = jit_shutdown,
};
#endif
