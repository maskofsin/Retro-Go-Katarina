#ifndef JIT_EMIT_H
#define JIT_EMIT_H

#include "jit_xtensa.h"
#include <limits.h>
#include <string.h>

/*
 * Xtensa LX code emitter for the small subset needed by the first-pass PSX JIT.
 * All instructions are emitted in little-endian byte order.
 *
 * The helpers intentionally prefer 24-bit encodings. Code-density encodings are
 * present only where they are simple and useful for returns/nops.
 */

typedef enum XtReg {
    XT_A0  = 0,
    XT_A1  = 1,
    XT_A2  = 2,
    XT_A3  = 3,
    XT_A4  = 4,
    XT_A5  = 5,
    XT_A6  = 6,
    XT_A7  = 7,
    XT_A8  = 8,
    XT_A9  = 9,
    XT_A10 = 10,
    XT_A11 = 11,
    XT_A12 = 12,
    XT_A13 = 13,
    XT_A14 = 14,
    XT_A15 = 15
} XtReg;

typedef struct JitEmit {
    /* base/p/end point at the byte-addressable staging buffer. exec_base is the final executable address. */
    uint8_t *base;
    uint8_t *p;
    uint8_t *end;
    uint8_t *exec_base;
    uint32_t *lit_base;
    uint32_t lit_count;
    uint32_t lit_capacity;
    bool failed;
} JitEmit;

typedef struct JitPatch {
    uint8_t *at;
} JitPatch;

static inline bool jit_xt_reg_valid(int r) {
    return r >= 0 && r <= 15;
}

static inline bool jit_s8(int32_t v) {
    return v >= -128 && v <= 127;
}

static inline bool jit_s12(int32_t v) {
    return v >= -2048 && v <= 2047;
}

static inline bool jit_u32_offset_ok(size_t byte_offset) {
    return (byte_offset <= 1020u) && ((byte_offset & 3u) == 0u);
}

static inline void jit_emit_init(JitEmit *e, uint8_t *base, uint8_t *entry,
                                 uint8_t *end, uint8_t *exec_base,
                                 uint32_t *literal_base, uint32_t literal_capacity) {
    e->base = base;
    e->p = entry;
    e->end = end;
    e->exec_base = exec_base;
    e->lit_base = literal_base;
    e->lit_count = 0;
    e->lit_capacity = literal_capacity;
    e->failed = false;
}

static inline size_t jit_emit_offset(const JitEmit *e) {
    return (size_t)(e->p - e->base);
}

static inline uint8_t *jit_emit_exec_addr(const JitEmit *e, const void *staging_addr) {
    return e->exec_base + ((const uint8_t *)staging_addr - e->base);
}

static inline size_t jit_emit_entry_offset(const JitEmit *e) {
    return (size_t)(e->p - e->base);
}

static inline bool jit_emit_room(JitEmit *e, size_t n) {
    if (e->failed || (size_t)(e->end - e->p) < n) {
        e->failed = true;
        return false;
    }
    return true;
}

static inline void jit_emit8(JitEmit *e, uint8_t b) {
    if (!jit_emit_room(e, 1u)) return;
    *e->p++ = b;
}

static inline void jit_emit16_raw(JitEmit *e, uint16_t w) {
    if (!jit_emit_room(e, 2u)) return;
    e->p[0] = (uint8_t)(w & 0xffu);
    e->p[1] = (uint8_t)((w >> 8) & 0xffu);
    e->p += 2;
}

static inline void jit_emit24_raw(JitEmit *e, uint32_t w) {
    if (!jit_emit_room(e, 3u)) return;
    e->p[0] = (uint8_t)(w & 0xffu);
    e->p[1] = (uint8_t)((w >> 8) & 0xffu);
    e->p[2] = (uint8_t)((w >> 16) & 0xffu);
    e->p += 3;
}

static inline void jit_emit32_literal(JitEmit *e, uint32_t value) {
    if (!jit_emit_room(e, 4u)) return;
    e->p[0] = (uint8_t)(value & 0xffu);
    e->p[1] = (uint8_t)((value >> 8) & 0xffu);
    e->p[2] = (uint8_t)((value >> 16) & 0xffu);
    e->p[3] = (uint8_t)((value >> 24) & 0xffu);
    e->p += 4;
}

static inline void jit_patch24_raw(uint8_t *p, uint32_t w) {
    p[0] = (uint8_t)(w & 0xffu);
    p[1] = (uint8_t)((w >> 8) & 0xffu);
    p[2] = (uint8_t)((w >> 16) & 0xffu);
}

static inline uint32_t jit_xt_rrr(unsigned op2, unsigned op1, unsigned r,
                                  unsigned s, unsigned t, unsigned op0) {
    return ((op2 & 0xfu) << 20) |
           ((op1 & 0xfu) << 16) |
           ((r   & 0xfu) << 12) |
           ((s   & 0xfu) <<  8) |
           ((t   & 0xfu) <<  4) |
           ((op0 & 0xfu) <<  0);
}

static inline uint32_t jit_xt_rri8(unsigned op1, unsigned s, unsigned t,
                                   int32_t imm8, unsigned op0) {
    return (((uint32_t)imm8 & 0xffu) << 16) |
           ((op1 & 0xfu) << 12) |
           ((s   & 0xfu) <<  8) |
           ((t   & 0xfu) <<  4) |
           ((op0 & 0xfu) <<  0);
}

static inline uint32_t jit_xt_bri12(unsigned s, unsigned m, int32_t off12) {
    return (((uint32_t)off12 & 0xfffu) << 12) |
           ((s & 0xfu) << 8) |
           ((m & 0xfu) << 4) |
           0x6u;
}

static inline uint32_t jit_xt_ri16(unsigned t, int32_t imm16, unsigned op0) {
    return (((uint32_t)imm16 & 0xffffu) << 8) |
           ((t & 0xfu) << 4) |
           (op0 & 0xfu);
}

static inline void jit_emit_nop_n(JitEmit *e) {
    jit_emit16_raw(e, 0xf03du);
}

static inline void jit_emit_ret_n(JitEmit *e) {
    jit_emit16_raw(e, 0xf00du);
}

static inline void jit_emit_retw_n(JitEmit *e) {
    jit_emit16_raw(e, 0xf01du);
}

static inline void jit_emit_entry(JitEmit *e, unsigned sp_reg, unsigned frame_bytes) {
    if (!jit_xt_reg_valid((int)sp_reg) || sp_reg > 3u || (frame_bytes & 7u) != 0u) {
        e->failed = true;
        return;
    }
    unsigned imm = frame_bytes >> 3;
    if (imm > 0xfffu) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, (((uint32_t)imm & 0xfffu) << 12) |
                      ((sp_reg & 0xfu) << 8) |
                      (0x3u << 4) |
                      0x6u);
}

static inline void jit_emit_add(JitEmit *e, unsigned dst, unsigned src1, unsigned src2) {
    jit_emit24_raw(e, jit_xt_rrr(0x8u, 0x0u, dst, src1, src2, 0x0u));
}

static inline void jit_emit_addx4(JitEmit *e, unsigned dst, unsigned base_src, unsigned scaled_src) {
    /* Xtensa ADDX4 syntax is: addx4 ar, as, at => ar = as + (at << 2). */
    jit_emit24_raw(e, jit_xt_rrr(0xau, 0x0u, dst, base_src, scaled_src, 0x0u));
}

static inline void jit_emit_sub(JitEmit *e, unsigned dst, unsigned src1, unsigned src2) {
    jit_emit24_raw(e, jit_xt_rrr(0xcu, 0x0u, dst, src1, src2, 0x0u));
}

static inline void jit_emit_and(JitEmit *e, unsigned dst, unsigned src1, unsigned src2) {
    jit_emit24_raw(e, jit_xt_rrr(0x1u, 0x0u, dst, src1, src2, 0x0u));
}

static inline void jit_emit_or(JitEmit *e, unsigned dst, unsigned src1, unsigned src2) {
    jit_emit24_raw(e, jit_xt_rrr(0x2u, 0x0u, dst, src1, src2, 0x0u));
}

static inline void jit_emit_xor(JitEmit *e, unsigned dst, unsigned src1, unsigned src2) {
    jit_emit24_raw(e, jit_xt_rrr(0x3u, 0x0u, dst, src1, src2, 0x0u));
}

static inline void jit_emit_mov(JitEmit *e, unsigned dst, unsigned src) {
    /* ADDI with immediate zero is a safe non-relaxed machine encoding for MOV. */
    jit_emit24_raw(e, jit_xt_rri8(0xcu, src, dst, 0, 0x2u));
}

static inline void jit_emit_addi(JitEmit *e, unsigned dst, unsigned src, int32_t imm) {
    if (!jit_s8(imm)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0xcu, src, dst, imm, 0x2u));
}

static inline void jit_emit_l32i(JitEmit *e, unsigned dst, unsigned base, size_t byte_offset) {
    if (!jit_u32_offset_ok(byte_offset)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x2u, base, dst, (int32_t)(byte_offset >> 2), 0x2u));
}

static inline void jit_emit_s32i(JitEmit *e, unsigned src, unsigned base, size_t byte_offset) {
    if (!jit_u32_offset_ok(byte_offset)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x6u, base, src, (int32_t)(byte_offset >> 2), 0x2u));
}

static inline void jit_emit_l8ui(JitEmit *e, unsigned dst, unsigned base, size_t byte_offset) {
    if (byte_offset > 255u) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x0u, base, dst, (int32_t)byte_offset, 0x2u));
}

static inline void jit_emit_s8i(JitEmit *e, unsigned src, unsigned base, size_t byte_offset) {
    if (byte_offset > 255u) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x4u, base, src, (int32_t)byte_offset, 0x2u));
}

static inline bool jit_u16_offset_ok(size_t byte_offset) {
    return (byte_offset <= 510u) && ((byte_offset & 1u) == 0u);
}

static inline void jit_emit_l16ui(JitEmit *e, unsigned dst, unsigned base, size_t byte_offset) {
    if (!jit_u16_offset_ok(byte_offset)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x1u, base, dst, (int32_t)(byte_offset >> 1), 0x2u));
}

static inline void jit_emit_l16si(JitEmit *e, unsigned dst, unsigned base, size_t byte_offset) {
    if (!jit_u16_offset_ok(byte_offset)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x9u, base, dst, (int32_t)(byte_offset >> 1), 0x2u));
}

static inline void jit_emit_s16i(JitEmit *e, unsigned src, unsigned base, size_t byte_offset) {
    if (!jit_u16_offset_ok(byte_offset)) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rri8(0x5u, base, src, (int32_t)(byte_offset >> 1), 0x2u));
}

static inline void jit_emit_extui(JitEmit *e, unsigned dst, unsigned src, unsigned shift, unsigned width) {
    if (shift > 31u || width == 0u || width > 16u || shift + width > 32u) {
        e->failed = true;
        return;
    }
    unsigned op2 = (width - 1u) & 0xfu;
    unsigned op1 = 0x4u | ((shift >> 4) & 0x1u);
    unsigned sfield = shift & 0xfu;
    jit_emit24_raw(e, jit_xt_rrr(op2, op1, dst, sfield, src, 0x0u));
}

static inline void jit_emit_ssai(JitEmit *e, unsigned shift) {
    if (shift > 31u) {
        e->failed = true;
        return;
    }
    jit_emit24_raw(e, jit_xt_rrr(0x4u, 0x0u, 0x4u, shift & 0xfu,
                                  (shift >> 4) & 0x1u, 0x0u));
}

static inline void jit_emit_ssl(JitEmit *e, unsigned src) {
    jit_emit24_raw(e, jit_xt_rrr(0x4u, 0x0u, 0x1u, src, 0x0u, 0x0u));
}

static inline void jit_emit_ssr(JitEmit *e, unsigned src) {
    jit_emit24_raw(e, jit_xt_rrr(0x4u, 0x0u, 0x0u, src, 0x0u, 0x0u));
}

static inline void jit_emit_sll(JitEmit *e, unsigned dst, unsigned src) {
    jit_emit24_raw(e, jit_xt_rrr(0xau, 0x1u, dst, src, 0x0u, 0x0u));
}

static inline void jit_emit_srl(JitEmit *e, unsigned dst, unsigned src) {
    jit_emit24_raw(e, jit_xt_rrr(0x9u, 0x1u, dst, 0x0u, src, 0x0u));
}

static inline void jit_emit_sra(JitEmit *e, unsigned dst, unsigned src) {
    jit_emit24_raw(e, jit_xt_rrr(0xbu, 0x1u, dst, 0x0u, src, 0x0u));
}

static inline void jit_emit_slli(JitEmit *e, unsigned dst, unsigned src, unsigned shift) {
    if (shift > 31u) {
        e->failed = true;
        return;
    }
    if (shift == 0u) {
        jit_emit_mov(e, dst, src);
        return;
    }
    jit_emit24_raw(e, jit_xt_rrr((shift >> 4) & 0x1u, 0x1u,
                                  dst, src, shift & 0xfu, 0x0u));
}

static inline void jit_emit_srli(JitEmit *e, unsigned dst, unsigned src, unsigned shift) {
    if (shift > 31u) {
        e->failed = true;
        return;
    }
    if (shift == 0u) {
        jit_emit_mov(e, dst, src);
        return;
    }
    if (shift <= 15u) {
        jit_emit24_raw(e, jit_xt_rrr(0x4u, 0x1u, dst, shift & 0xfu, src, 0x0u));
    } else {
        jit_emit_ssai(e, shift);
        jit_emit_srl(e, dst, src);
    }
}

static inline void jit_emit_srai(JitEmit *e, unsigned dst, unsigned src, unsigned shift) {
    if (shift > 31u) {
        e->failed = true;
        return;
    }
    if (shift == 0u) {
        jit_emit_mov(e, dst, src);
        return;
    }
    jit_emit24_raw(e, jit_xt_rrr(0x2u | ((shift >> 4) & 0x1u), 0x1u,
                                  dst, shift & 0xfu, src, 0x0u));
}

static inline void jit_emit_movi12(JitEmit *e, unsigned dst, int32_t imm) {
    if (!jit_s12(imm)) {
        e->failed = true;
        return;
    }
    uint32_t u = (uint32_t)imm & 0xfffu;
    uint32_t insn = ((u & 0xffu) << 16) |
                    (0xau << 12) |
                    (((u >> 8) & 0xfu) << 8) |
                    ((dst & 0xfu) << 4) |
                    0x2u;
    jit_emit24_raw(e, insn);
}

static inline void jit_emit_l32r_abs(JitEmit *e, unsigned dst, const void *literal_addr) {
    const uint8_t *insn_ptr = jit_emit_exec_addr(e, e->p);
    const uint8_t *lit_ptr = jit_emit_exec_addr(e, literal_addr);
    uintptr_t insn_addr_u = (uintptr_t)insn_ptr;
    uintptr_t lit_addr_u = (uintptr_t)lit_ptr;
    uintptr_t aligned_pc_u = (insn_addr_u + 3u) & ~(uintptr_t)3u;

    /* Literal pools are placed before the code entry. Compute the signed
     * displacement without first subtracting unsigned uintptr_t values, which
     * would wrap for the normal backward L32R reference.
     */
    if (lit_addr_u > (uintptr_t)INTPTR_MAX || aligned_pc_u > (uintptr_t)INTPTR_MAX) {
        e->failed = true;
        return;
    }
    intptr_t diff = (intptr_t)lit_addr_u - (intptr_t)aligned_pc_u;
    if ((diff & 3) != 0 || diff >= 0 || diff < -262144) {
        e->failed = true;
        return;
    }
    int32_t imm16 = (int32_t)(diff >> 2);
    jit_emit24_raw(e, jit_xt_ri16(dst, imm16, 0x1u));
}

static inline void jit_emit_l32r_value(JitEmit *e, unsigned dst, uint32_t value) {
    if (e->lit_count >= e->lit_capacity) {
        e->failed = true;
        return;
    }
    uint32_t *slot = &e->lit_base[e->lit_count++];
    *slot = value;
    jit_emit_l32r_abs(e, dst, slot);
}

static inline void jit_emit_movi32(JitEmit *e, unsigned dst, uint32_t value) {
    int32_t signed_value = (int32_t)value;
    if (jit_s12(signed_value)) {
        jit_emit_movi12(e, dst, signed_value);
    } else {
        jit_emit_l32r_value(e, dst, value);
    }
}

static inline void jit_emit_beqz_placeholder(JitEmit *e, JitPatch *patch, unsigned reg) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_bri12(reg, 0x1u, 0));
}

static inline void jit_emit_bnez_placeholder(JitEmit *e, JitPatch *patch, unsigned reg) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_bri12(reg, 0x5u, 0));
}

static inline void jit_emit_bltz_placeholder(JitEmit *e, JitPatch *patch, unsigned reg) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_bri12(reg, 0x9u, 0));
}

static inline void jit_emit_bgez_placeholder(JitEmit *e, JitPatch *patch, unsigned reg) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_bri12(reg, 0xdu, 0));
}

static inline bool jit_patch_bri12(JitPatch patch, const uint8_t *target) {
    if (patch.at == NULL) return false;
    intptr_t off = (intptr_t)(target - (patch.at + 4));
    if (off < -2048 || off > 2047) return false;
    uint32_t old = ((uint32_t)patch.at[0]) |
                   ((uint32_t)patch.at[1] << 8) |
                   ((uint32_t)patch.at[2] << 16);
    uint32_t reg = (old >> 8) & 0xfu;
    uint32_t m = (old >> 4) & 0xfu;
    jit_patch24_raw(patch.at, jit_xt_bri12(reg, m, (int32_t)off));
    return true;
}

static inline void jit_emit_blt_placeholder(JitEmit *e, JitPatch *patch, unsigned lhs, unsigned rhs) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_rri8(0x2u, lhs, rhs, 0, 0x7u));
}

static inline void jit_emit_bge_placeholder(JitEmit *e, JitPatch *patch, unsigned lhs, unsigned rhs) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_rri8(0xau, lhs, rhs, 0, 0x7u));
}

static inline void jit_emit_bltu_placeholder(JitEmit *e, JitPatch *patch, unsigned lhs, unsigned rhs) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_rri8(0x3u, lhs, rhs, 0, 0x7u));
}

static inline void jit_emit_bgeu_placeholder(JitEmit *e, JitPatch *patch, unsigned lhs, unsigned rhs) {
    patch->at = e->p;
    jit_emit24_raw(e, jit_xt_rri8(0xbu, lhs, rhs, 0, 0x7u));
}

static inline bool jit_patch_bri8(JitPatch patch, const uint8_t *target) {
    if (patch.at == NULL) return false;
    intptr_t off = (intptr_t)(target - (patch.at + 4));
    if (off < -128 || off > 127) return false;
    patch.at[2] = (uint8_t)((uint32_t)off & 0xffu);
    return true;
}

static inline void jit_emit_jx(JitEmit *e, unsigned reg) {
    jit_emit24_raw(e, ((reg & 0xfu) << 8) | (0x2u << 6) | (0x2u << 4));
}

static inline void jit_emit_callx0(JitEmit *e, unsigned reg) {
    jit_emit24_raw(e, ((reg & 0xfu) << 8) | (0x3u << 6) | (0x0u << 4));
}

static inline void jit_emit_callx8(JitEmit *e, unsigned reg) {
    jit_emit24_raw(e, ((reg & 0xfu) << 8) | (0x3u << 6) | (0x2u << 4));
}

static inline void jit_emit_return(JitEmit *e) {
#if JIT_USE_WINDOWED_ABI
    jit_emit_retw_n(e);
#else
    jit_emit_ret_n(e);
#endif
}

#endif /* JIT_EMIT_H */
