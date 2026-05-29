#include "jit_emit.h"

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define JIT_ATTR_UNUSED __attribute__((unused))
#else
#define JIT_ATTR_UNUSED
#endif

#define MIPS_OP_SPECIAL 0x00u
#define MIPS_OP_REGIMM  0x01u
#define MIPS_OP_J       0x02u
#define MIPS_OP_JAL     0x03u
#define MIPS_OP_BEQ     0x04u
#define MIPS_OP_BNE     0x05u
#define MIPS_OP_BLEZ    0x06u
#define MIPS_OP_BGTZ    0x07u
#define MIPS_OP_ADDIU   0x09u
#define MIPS_OP_SLTI    0x0au
#define MIPS_OP_SLTIU   0x0bu
#define MIPS_OP_ANDI    0x0cu
#define MIPS_OP_ORI     0x0du
#define MIPS_OP_XORI    0x0eu
#define MIPS_OP_LUI     0x0fu
#define MIPS_OP_LB      0x20u
#define MIPS_OP_LH      0x21u
#define MIPS_OP_LWL     0x22u
#define MIPS_OP_LW      0x23u
#define MIPS_OP_LBU     0x24u
#define MIPS_OP_LHU     0x25u
#define MIPS_OP_LWR     0x26u
#define MIPS_OP_SB      0x28u
#define MIPS_OP_SH      0x29u
#define MIPS_OP_SWL     0x2au
#define MIPS_OP_SW      0x2bu
#define MIPS_OP_SWR     0x2eu

#define MIPS_FUNCT_SLL  0x00u
#define MIPS_FUNCT_SRL  0x02u
#define MIPS_FUNCT_SRA  0x03u
#define MIPS_FUNCT_SLLV 0x04u
#define MIPS_FUNCT_SRLV 0x06u
#define MIPS_FUNCT_SRAV 0x07u
#define MIPS_FUNCT_JR   0x08u
#define MIPS_FUNCT_JALR 0x09u
#define MIPS_FUNCT_MFHI 0x10u
#define MIPS_FUNCT_MTHI 0x11u
#define MIPS_FUNCT_MFLO 0x12u
#define MIPS_FUNCT_MTLO 0x13u
#define MIPS_FUNCT_MULT 0x18u
#define MIPS_FUNCT_MULTU 0x19u
#define MIPS_FUNCT_DIV  0x1au
#define MIPS_FUNCT_DIVU 0x1bu
#define MIPS_FUNCT_ADDU 0x21u
#define MIPS_FUNCT_SUBU 0x23u
#define MIPS_FUNCT_AND  0x24u
#define MIPS_FUNCT_OR   0x25u
#define MIPS_FUNCT_XOR  0x26u
#define MIPS_FUNCT_NOR  0x27u
#define MIPS_FUNCT_SLT  0x2au
#define MIPS_FUNCT_SLTU 0x2bu

#define MIPS_RS(i)      (((i) >> 21) & 31u)
#define MIPS_RT(i)      (((i) >> 16) & 31u)
#define MIPS_RD(i)      (((i) >> 11) & 31u)
#define MIPS_SA(i)      (((i) >>  6) & 31u)
#define MIPS_FUNCT(i)   ((i) & 63u)
#define MIPS_IMM(i)     ((uint16_t)((i) & 0xffffu))
#define MIPS_SIMM(i)    ((int16_t)((i) & 0xffffu))
#define MIPS_TARGET(i)  ((i) & 0x03ffffffu)

#define MIPS_REGIMM_BLTZ   0x00u
#define MIPS_REGIMM_BGEZ   0x01u
#define MIPS_REGIMM_BLTZAL 0x10u
#define MIPS_REGIMM_BGEZAL 0x11u

typedef struct JitEmitMark {
    uint8_t *p;
    uint32_t lit_count;
    bool failed;
} JitEmitMark;

static JitEmitMark emit_mark(const JitEmit *e) {
    JitEmitMark mark;
    mark.p = e->p;
    mark.lit_count = e->lit_count;
    mark.failed = e->failed;
    return mark;
}

static void emit_rewind(JitEmit *e, JitEmitMark mark) {
    if (mark.lit_count < e->lit_count) {
        memset(&e->lit_base[mark.lit_count], 0,
               (size_t)(e->lit_count - mark.lit_count) * sizeof(e->lit_base[0]));
    }
    if (mark.p < e->p) {
        memset(mark.p, 0, (size_t)(e->p - mark.p));
    }
    e->p = mark.p;
    e->lit_count = mark.lit_count;
    e->failed = mark.failed;
}

static int mips_hot_ar(unsigned r) {
    switch (r) {
        case 1:  return XT_A4;
        case 2:  return XT_A5;
        case 4:  return XT_A6;
        case 5:  return XT_A7;
        case 16: return XT_A8;
        case 17: return XT_A9;
        case 18: return XT_A10;
        case 19: return XT_A11;
        case 20: return XT_A12;
        case 21: return XT_A13;
        case 22: return XT_A14;
        case 23: return XT_A15;
        default: return -1;
    }
}

static unsigned xt_to_mips_hot(unsigned ar) {
    switch (ar) {
        case XT_A4:  return 1;
        case XT_A5:  return 2;
        case XT_A6:  return 4;
        case XT_A7:  return 5;
        case XT_A8:  return 16;
        case XT_A9:  return 17;
        case XT_A10: return 18;
        case XT_A11: return 19;
        case XT_A12: return 20;
        case XT_A13: return 21;
        case XT_A14: return 22;
        case XT_A15: return 23;
        default:     return 0;
    }
}

static uint32_t hot_mask_for_mips(unsigned r) {
    int ar = mips_hot_ar(r);
    return ar >= 0 ? (1u << (unsigned)ar) : 0u;
}

static bool mips_rtype_sa_zero(uint32_t insn) {
    return MIPS_SA(insn) == 0u;
}

static bool mips_jr_encoding_ok(uint32_t insn) {
    return MIPS_FUNCT(insn) == MIPS_FUNCT_JR &&
           MIPS_RT(insn) == 0u && MIPS_RD(insn) == 0u && MIPS_SA(insn) == 0u;
}

static bool mips_jalr_encoding_ok(uint32_t insn) {
    return MIPS_FUNCT(insn) == MIPS_FUNCT_JALR &&
           MIPS_RT(insn) == 0u && MIPS_SA(insn) == 0u;
}

static bool mips_insn_reads_reg(uint32_t insn, unsigned reg) {
    if (reg == 0u) return false;
    unsigned op = insn >> 26;
    switch (op) {
        case MIPS_OP_SPECIAL:
            switch (MIPS_FUNCT(insn)) {
                case MIPS_FUNCT_SLL:
                case MIPS_FUNCT_SRL:
                case MIPS_FUNCT_SRA:
                    return MIPS_RT(insn) == reg;
                case MIPS_FUNCT_SLLV:
                case MIPS_FUNCT_SRLV:
                case MIPS_FUNCT_SRAV:
                    return MIPS_RT(insn) == reg || MIPS_RS(insn) == reg;
                case MIPS_FUNCT_JR:
                case MIPS_FUNCT_JALR:
                    return MIPS_RS(insn) == reg;
                case MIPS_FUNCT_ADDU:
                case MIPS_FUNCT_SUBU:
                case MIPS_FUNCT_AND:
                case MIPS_FUNCT_OR:
                case MIPS_FUNCT_XOR:
                case MIPS_FUNCT_NOR:
                case MIPS_FUNCT_SLT:
                case MIPS_FUNCT_SLTU:
                    return MIPS_RS(insn) == reg || MIPS_RT(insn) == reg;
                case MIPS_FUNCT_MTHI:
                case MIPS_FUNCT_MTLO:
                    return MIPS_RS(insn) == reg;
                case MIPS_FUNCT_MULT:
                case MIPS_FUNCT_MULTU:
                case MIPS_FUNCT_DIV:
                case MIPS_FUNCT_DIVU:
                    return MIPS_RS(insn) == reg || MIPS_RT(insn) == reg;
                default:
                    return false;
            }
        case MIPS_OP_REGIMM:
        case MIPS_OP_BLEZ:
        case MIPS_OP_BGTZ:
            return MIPS_RS(insn) == reg;
        case MIPS_OP_BEQ:
        case MIPS_OP_BNE:
            return MIPS_RS(insn) == reg || MIPS_RT(insn) == reg;
        case MIPS_OP_ADDIU:
        case MIPS_OP_SLTI:
        case MIPS_OP_SLTIU:
        case MIPS_OP_ANDI:
        case MIPS_OP_ORI:
        case MIPS_OP_XORI:
        case MIPS_OP_LB:
        case MIPS_OP_LBU:
        case MIPS_OP_LH:
        case MIPS_OP_LHU:
        case MIPS_OP_LW:
            return MIPS_RS(insn) == reg;
        case MIPS_OP_LWL:
        case MIPS_OP_LWR:
            return MIPS_RS(insn) == reg || MIPS_RT(insn) == reg;
        case MIPS_OP_SB:
        case MIPS_OP_SH:
        case MIPS_OP_SW:
        case MIPS_OP_SWL:
        case MIPS_OP_SWR:
            return MIPS_RS(insn) == reg || MIPS_RT(insn) == reg;
        default:
            return false;
    }
}

static bool mips_insn_writes_reg(uint32_t insn, unsigned reg) {
    if (reg == 0u) return false;
    unsigned op = insn >> 26;
    switch (op) {
        case MIPS_OP_SPECIAL:
            switch (MIPS_FUNCT(insn)) {
                case MIPS_FUNCT_SLL:
                case MIPS_FUNCT_SRL:
                case MIPS_FUNCT_SRA:
                case MIPS_FUNCT_SLLV:
                case MIPS_FUNCT_SRLV:
                case MIPS_FUNCT_SRAV:
                case MIPS_FUNCT_ADDU:
                case MIPS_FUNCT_SUBU:
                case MIPS_FUNCT_AND:
                case MIPS_FUNCT_OR:
                case MIPS_FUNCT_XOR:
                case MIPS_FUNCT_NOR:
                case MIPS_FUNCT_SLT:
                case MIPS_FUNCT_SLTU:
                case MIPS_FUNCT_MFHI:
                case MIPS_FUNCT_MFLO:
                    return MIPS_RD(insn) == reg;
                case MIPS_FUNCT_JALR:
                    return MIPS_RD(insn) == reg;
                default:
                    return false;
            }
        case MIPS_OP_ADDIU:
        case MIPS_OP_SLTI:
        case MIPS_OP_SLTIU:
        case MIPS_OP_ANDI:
        case MIPS_OP_ORI:
        case MIPS_OP_XORI:
        case MIPS_OP_LUI:
        case MIPS_OP_LB:
        case MIPS_OP_LBU:
        case MIPS_OP_LH:
        case MIPS_OP_LHU:
        case MIPS_OP_LW:
        case MIPS_OP_LWL:
        case MIPS_OP_LWR:
            return MIPS_RT(insn) == reg;
        case MIPS_OP_JAL:
            return reg == 31u;
        case MIPS_OP_REGIMM:
            return reg == 31u &&
                   (MIPS_RT(insn) == MIPS_REGIMM_BLTZAL ||
                    MIPS_RT(insn) == MIPS_REGIMM_BGEZAL);
        default:
            return false;
    }
}

static bool mips_insn_touches_reg(uint32_t insn, unsigned reg) {
    return mips_insn_reads_reg(insn, reg) || mips_insn_writes_reg(insn, reg);
}

typedef struct Scratch {
    uint32_t used_mask;
    uint32_t saved_mask;
} Scratch;

static const unsigned kScratchRegs[] = { XT_A12, XT_A13, XT_A14, XT_A15 };

static void scratch_init(Scratch *s) {
    s->used_mask = 0u;
    s->saved_mask = 0u;
}

static int scratch_take(JitEmit *e, Scratch *s, uint32_t avoid_mask) {
    for (size_t i = 0; i < sizeof(kScratchRegs) / sizeof(kScratchRegs[0]); ++i) {
        unsigned ar = kScratchRegs[i];
        uint32_t bit = 1u << ar;
        if ((s->used_mask & bit) != 0u) continue;
        if ((avoid_mask & bit) != 0u) continue;
        unsigned mips = xt_to_mips_hot(ar);
        if (mips == 0u) {
            e->failed = true;
            return -1;
        }
        if ((s->saved_mask & bit) == 0u) {
            jit_emit_s32i(e, ar, XT_A2, JIT_MIPS_GPR_OFFSET(mips));
            s->saved_mask |= bit;
        }
        s->used_mask |= bit;
        return (int)ar;
    }
    e->failed = true;
    return -1;
}

static void scratch_release(Scratch *s, int ar) {
    if (ar < 0) return;
    s->used_mask &= ~(1u << (unsigned)ar);
}

static void scratch_restore(JitEmit *e, const Scratch *s) {
    for (size_t i = 0; i < sizeof(kScratchRegs) / sizeof(kScratchRegs[0]); ++i) {
        unsigned ar = kScratchRegs[i];
        uint32_t bit = 1u << ar;
        if ((s->saved_mask & bit) == 0u) continue;
        unsigned mips = xt_to_mips_hot(ar);
        jit_emit_l32i(e, ar, XT_A2, JIT_MIPS_GPR_OFFSET(mips));
    }
}

static void emit_load_hot_regs(JitEmit *e) {
    jit_emit_l32i(e, XT_A4,  XT_A2, JIT_MIPS_GPR_OFFSET(1));
    jit_emit_l32i(e, XT_A5,  XT_A2, JIT_MIPS_GPR_OFFSET(2));
    jit_emit_l32i(e, XT_A6,  XT_A2, JIT_MIPS_GPR_OFFSET(4));
    jit_emit_l32i(e, XT_A7,  XT_A2, JIT_MIPS_GPR_OFFSET(5));
    jit_emit_l32i(e, XT_A8,  XT_A2, JIT_MIPS_GPR_OFFSET(16));
    jit_emit_l32i(e, XT_A9,  XT_A2, JIT_MIPS_GPR_OFFSET(17));
    jit_emit_l32i(e, XT_A10, XT_A2, JIT_MIPS_GPR_OFFSET(18));
    jit_emit_l32i(e, XT_A11, XT_A2, JIT_MIPS_GPR_OFFSET(19));
    jit_emit_l32i(e, XT_A12, XT_A2, JIT_MIPS_GPR_OFFSET(20));
    jit_emit_l32i(e, XT_A13, XT_A2, JIT_MIPS_GPR_OFFSET(21));
    jit_emit_l32i(e, XT_A14, XT_A2, JIT_MIPS_GPR_OFFSET(22));
    jit_emit_l32i(e, XT_A15, XT_A2, JIT_MIPS_GPR_OFFSET(23));
}

static void emit_flush_hot_regs(JitEmit *e, uint32_t skip_xt_mask) {
    if ((skip_xt_mask & (1u << XT_A4))  == 0u) jit_emit_s32i(e, XT_A4,  XT_A2, JIT_MIPS_GPR_OFFSET(1));
    if ((skip_xt_mask & (1u << XT_A5))  == 0u) jit_emit_s32i(e, XT_A5,  XT_A2, JIT_MIPS_GPR_OFFSET(2));
    if ((skip_xt_mask & (1u << XT_A6))  == 0u) jit_emit_s32i(e, XT_A6,  XT_A2, JIT_MIPS_GPR_OFFSET(4));
    if ((skip_xt_mask & (1u << XT_A7))  == 0u) jit_emit_s32i(e, XT_A7,  XT_A2, JIT_MIPS_GPR_OFFSET(5));
    if ((skip_xt_mask & (1u << XT_A8))  == 0u) jit_emit_s32i(e, XT_A8,  XT_A2, JIT_MIPS_GPR_OFFSET(16));
    if ((skip_xt_mask & (1u << XT_A9))  == 0u) jit_emit_s32i(e, XT_A9,  XT_A2, JIT_MIPS_GPR_OFFSET(17));
    if ((skip_xt_mask & (1u << XT_A10)) == 0u) jit_emit_s32i(e, XT_A10, XT_A2, JIT_MIPS_GPR_OFFSET(18));
    if ((skip_xt_mask & (1u << XT_A11)) == 0u) jit_emit_s32i(e, XT_A11, XT_A2, JIT_MIPS_GPR_OFFSET(19));
    if ((skip_xt_mask & (1u << XT_A12)) == 0u) jit_emit_s32i(e, XT_A12, XT_A2, JIT_MIPS_GPR_OFFSET(20));
    if ((skip_xt_mask & (1u << XT_A13)) == 0u) jit_emit_s32i(e, XT_A13, XT_A2, JIT_MIPS_GPR_OFFSET(21));
    if ((skip_xt_mask & (1u << XT_A14)) == 0u) jit_emit_s32i(e, XT_A14, XT_A2, JIT_MIPS_GPR_OFFSET(22));
    if ((skip_xt_mask & (1u << XT_A15)) == 0u) jit_emit_s32i(e, XT_A15, XT_A2, JIT_MIPS_GPR_OFFSET(23));
}

static void emit_update_cycles(JitEmit *e, uint32_t cycles) {
    if (cycles == 0u) return;
    uint32_t scaled = cycles << JIT_BLOCK_CYCLE_SHIFT;
    jit_emit_l32i(e, XT_A14, XT_A2, JIT_PSX_CYCLE_OFFSET);
    if (jit_s8((int32_t)scaled)) {
        jit_emit_addi(e, XT_A14, XT_A14, (int32_t)scaled);
    } else {
        jit_emit_movi32(e, XT_A15, scaled);
        jit_emit_add(e, XT_A14, XT_A14, XT_A15);
    }
    jit_emit_s32i(e, XT_A14, XT_A2, JIT_PSX_CYCLE_OFFSET);
}

static void emit_exit_imm(JitEmit *e, uint32_t pc, uint32_t cycles, uint32_t skip_xt_mask) {
    emit_flush_hot_regs(e, skip_xt_mask);
    emit_update_cycles(e, cycles);
    jit_emit_movi32(e, XT_A14, pc);
    jit_emit_s32i(e, XT_A14, XT_A2, JIT_PSX_PC_OFFSET);
    jit_emit_return(e);
}

static void emit_exit_reg(JitEmit *e, unsigned pc_reg, uint32_t cycles, uint32_t skip_xt_mask) {
    emit_flush_hot_regs(e, skip_xt_mask);
    jit_emit_s32i(e, pc_reg, XT_A2, JIT_PSX_PC_OFFSET);
    emit_update_cycles(e, cycles);
    jit_emit_return(e);
}

static bool emit_store_mips_reg(JitEmit *e, unsigned mips_reg, unsigned ar) {
    if (mips_reg == 0u) return true;
    int hot = mips_hot_ar(mips_reg);
    if (hot >= 0) {
        if ((unsigned)hot != ar) jit_emit_mov(e, (unsigned)hot, ar);
    } else {
        jit_emit_s32i(e, ar, XT_A2, JIT_MIPS_GPR_OFFSET(mips_reg));
    }
    return !e->failed;
}

static bool materialize_mips_reg(JitEmit *e, Scratch *s, unsigned mips_reg,
                                 uint32_t avoid_mask, int *out_ar) {
    if (mips_reg == 0u) {
        int tmp = scratch_take(e, s, avoid_mask);
        if (tmp < 0) return false;
        jit_emit_movi12(e, (unsigned)tmp, 0);
        *out_ar = tmp;
        return !e->failed;
    }
    int hot = mips_hot_ar(mips_reg);
    if (hot >= 0) {
        *out_ar = hot;
        return true;
    }
    int tmp = scratch_take(e, s, avoid_mask);
    if (tmp < 0) return false;
    jit_emit_l32i(e, (unsigned)tmp, XT_A2, JIT_MIPS_GPR_OFFSET(mips_reg));
    *out_ar = tmp;
    return !e->failed;
}

static bool materialize_dest_reg(JitEmit *e, Scratch *s, unsigned mips_reg,
                                 uint32_t avoid_mask, int *out_ar) {
    if (mips_reg == 0u) {
        *out_ar = -1;
        return true;
    }
    int hot = mips_hot_ar(mips_reg);
    if (hot >= 0) {
        *out_ar = hot;
        return true;
    }
    int tmp = scratch_take(e, s, avoid_mask);
    if (tmp < 0) return false;
    *out_ar = tmp;
    return true;
}

static bool emit_move_mips(JitEmit *e, unsigned dst, unsigned src) {
    if (dst == 0u || dst == src) return true;
    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(dst) | hot_mask_for_mips(src);

    int dst_ar = mips_hot_ar(dst);
    int src_ar = mips_hot_ar(src);
    if (src == 0u) {
        if (dst_ar >= 0) {
            jit_emit_movi12(e, (unsigned)dst_ar, 0);
        } else {
            int tmp = scratch_take(e, &sc, avoid);
            if (tmp < 0) return false;
            jit_emit_movi12(e, (unsigned)tmp, 0);
            jit_emit_s32i(e, (unsigned)tmp, XT_A2, JIT_MIPS_GPR_OFFSET(dst));
            scratch_restore(e, &sc);
        }
        return !e->failed;
    }

    if (dst_ar >= 0) {
        if (src_ar >= 0) {
            jit_emit_mov(e, (unsigned)dst_ar, (unsigned)src_ar);
        } else {
            jit_emit_l32i(e, (unsigned)dst_ar, XT_A2, JIT_MIPS_GPR_OFFSET(src));
        }
        return !e->failed;
    }

    if (src_ar >= 0) {
        jit_emit_s32i(e, (unsigned)src_ar, XT_A2, JIT_MIPS_GPR_OFFSET(dst));
    } else {
        int tmp = scratch_take(e, &sc, avoid);
        if (tmp < 0) return false;
        jit_emit_l32i(e, (unsigned)tmp, XT_A2, JIT_MIPS_GPR_OFFSET(src));
        jit_emit_s32i(e, (unsigned)tmp, XT_A2, JIT_MIPS_GPR_OFFSET(dst));
        scratch_restore(e, &sc);
    }
    return !e->failed;
}

static bool emit_movi_mips(JitEmit *e, unsigned dst, uint32_t value) {
    if (dst == 0u) return true;
    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(dst);
    int dst_ar = mips_hot_ar(dst);
    if (dst_ar < 0) {
        dst_ar = scratch_take(e, &sc, avoid);
        if (dst_ar < 0) return false;
    }
    jit_emit_movi32(e, (unsigned)dst_ar, value);
    if (!emit_store_mips_reg(e, dst, (unsigned)dst_ar)) return false;
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_addu(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;
    if (rs == 0u) return emit_move_mips(e, rd, rt);
    if (rt == 0u) return emit_move_mips(e, rd, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_add(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_subu(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;
    if (rt == 0u) return emit_move_mips(e, rd, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_sub(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_and_reg(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;
    if (rs == 0u || rt == 0u) return emit_movi_mips(e, rd, 0u);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_and(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_xor_reg(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;
    if (rs == rt) return emit_movi_mips(e, rd, 0u);
    if (rs == 0u) return emit_move_mips(e, rd, rt);
    if (rt == 0u) return emit_move_mips(e, rd, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_xor(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_or_reg(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;
    if (rs == 0u) return emit_move_mips(e, rd, rt);
    if (rt == 0u) return emit_move_mips(e, rd, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_or(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_nor_reg(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn);
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rd == 0u) return true;

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    int ar_rs, ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_or(e, (unsigned)ar_rd, (unsigned)ar_rs, (unsigned)ar_rt);
        int ar_not = scratch_take(e, &sc, avoid | (1u << (unsigned)ar_rd) |
                                  (1u << (unsigned)ar_rs) | (1u << (unsigned)ar_rt));
        if (ar_not < 0) return false;
        jit_emit_movi32(e, (unsigned)ar_not, UINT32_MAX);
        jit_emit_xor(e, (unsigned)ar_rd, (unsigned)ar_rd, (unsigned)ar_not);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_slt_common(JitEmit *e, unsigned dst, unsigned lhs_mips,
                               bool rhs_is_imm, unsigned rhs_mips,
                               uint32_t rhs_imm, bool is_unsigned_cmp) {
    if (dst == 0u) return true;
    if (!rhs_is_imm && lhs_mips == rhs_mips) return emit_movi_mips(e, dst, 0u);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(dst) | hot_mask_for_mips(lhs_mips) |
                     (rhs_is_imm ? 0u : hot_mask_for_mips(rhs_mips));
    int lhs_ar, rhs_ar;
    if (!materialize_mips_reg(e, &sc, lhs_mips, avoid, &lhs_ar)) return false;
    if (rhs_is_imm) {
        rhs_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)lhs_ar));
        if (rhs_ar < 0) return false;
        jit_emit_movi32(e, (unsigned)rhs_ar, rhs_imm);
    } else {
        if (!materialize_mips_reg(e, &sc, rhs_mips, avoid | (1u << (unsigned)lhs_ar), &rhs_ar)) return false;
    }

    uint32_t src_mask = (1u << (unsigned)lhs_ar) | (1u << (unsigned)rhs_ar);
    int dst_hot = mips_hot_ar(dst);
    int result_ar;
    if (dst_hot >= 0 && (src_mask & (1u << (unsigned)dst_hot)) == 0u) {
        result_ar = dst_hot;
    } else {
        result_ar = scratch_take(e, &sc, avoid | src_mask);
        if (result_ar < 0) return false;
    }

    jit_emit_movi12(e, (unsigned)result_ar, 1);
    JitPatch done;
    if (is_unsigned_cmp) {
        jit_emit_bltu_placeholder(e, &done, (unsigned)lhs_ar, (unsigned)rhs_ar);
    } else {
        jit_emit_blt_placeholder(e, &done, (unsigned)lhs_ar, (unsigned)rhs_ar);
    }
    jit_emit_movi12(e, (unsigned)result_ar, 0);
    if (!jit_patch_bri8(done, e->p)) {
        e->failed = true;
        return false;
    }
    emit_store_mips_reg(e, dst, (unsigned)result_ar);
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_slt_reg(JitEmit *e, uint32_t insn, bool is_unsigned_cmp) {
    return compile_slt_common(e, MIPS_RD(insn), MIPS_RS(insn), false,
                              MIPS_RT(insn), 0u, is_unsigned_cmp);
}

static bool compile_slti(JitEmit *e, uint32_t insn, bool is_unsigned_cmp) {
    uint32_t imm = (uint32_t)(int32_t)MIPS_SIMM(insn);
    return compile_slt_common(e, MIPS_RT(insn), MIPS_RS(insn), true,
                              0u, imm, is_unsigned_cmp);
}

static bool compile_addiu(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn);
    unsigned rs = MIPS_RS(insn);
    int32_t imm = (int32_t)MIPS_SIMM(insn);
    if (rt == 0u) return true;
    if (rs == 0u) return emit_movi_mips(e, rt, (uint32_t)imm);
    if (imm == 0) return emit_move_mips(e, rt, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(rs);
    int ar_rs, ar_rt;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_dest_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (ar_rt >= 0) {
        if (jit_s8(imm)) {
            jit_emit_addi(e, (unsigned)ar_rt, (unsigned)ar_rs, imm);
        } else {
            int ar_imm = scratch_take(e, &sc, avoid);
            if (ar_imm < 0) return false;
            jit_emit_movi32(e, (unsigned)ar_imm, (uint32_t)imm);
            jit_emit_add(e, (unsigned)ar_rt, (unsigned)ar_rs, (unsigned)ar_imm);
        }
        emit_store_mips_reg(e, rt, (unsigned)ar_rt);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_ori(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn);
    unsigned rs = MIPS_RS(insn);
    uint32_t imm = (uint32_t)MIPS_IMM(insn);
    if (rt == 0u) return true;
    if (rs == 0u) return emit_movi_mips(e, rt, imm);
    if (imm == 0u) return emit_move_mips(e, rt, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(rs);
    int ar_rs, ar_rt, ar_imm;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_dest_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    ar_imm = scratch_take(e, &sc, avoid);
    if (ar_imm < 0) return false;
    jit_emit_movi32(e, (unsigned)ar_imm, imm);
    if (ar_rt >= 0) {
        jit_emit_or(e, (unsigned)ar_rt, (unsigned)ar_rs, (unsigned)ar_imm);
        emit_store_mips_reg(e, rt, (unsigned)ar_rt);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_andi(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn);
    unsigned rs = MIPS_RS(insn);
    uint32_t imm = (uint32_t)MIPS_IMM(insn);
    if (rt == 0u) return true;
    if (rs == 0u || imm == 0u) return emit_movi_mips(e, rt, 0u);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(rs);
    int ar_rs, ar_rt, ar_imm;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_dest_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    ar_imm = scratch_take(e, &sc, avoid);
    if (ar_imm < 0) return false;
    jit_emit_movi32(e, (unsigned)ar_imm, imm);
    if (ar_rt >= 0) {
        jit_emit_and(e, (unsigned)ar_rt, (unsigned)ar_rs, (unsigned)ar_imm);
        emit_store_mips_reg(e, rt, (unsigned)ar_rt);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_xori(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn);
    unsigned rs = MIPS_RS(insn);
    uint32_t imm = (uint32_t)MIPS_IMM(insn);
    if (rt == 0u) return true;
    if (rs == 0u) return emit_movi_mips(e, rt, imm);
    if (imm == 0u) return emit_move_mips(e, rt, rs);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(rs);
    int ar_rs, ar_rt, ar_imm;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_dest_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    ar_imm = scratch_take(e, &sc, avoid);
    if (ar_imm < 0) return false;
    jit_emit_movi32(e, (unsigned)ar_imm, imm);
    if (ar_rt >= 0) {
        jit_emit_xor(e, (unsigned)ar_rt, (unsigned)ar_rs, (unsigned)ar_imm);
        emit_store_mips_reg(e, rt, (unsigned)ar_rt);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_lui(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn);
    uint32_t imm = ((uint32_t)MIPS_IMM(insn)) << 16;
    return emit_movi_mips(e, rt, imm);
}

static bool compile_sll_imm(JitEmit *e, uint32_t insn) {
    if (MIPS_RS(insn) != 0u) return false;
    unsigned rd = MIPS_RD(insn);
    unsigned rt = MIPS_RT(insn);
    unsigned sa = MIPS_SA(insn);
    if (rd == 0u) return true;
    if (rt == 0u) return emit_movi_mips(e, rd, 0u);
    if (sa == 0u) return emit_move_mips(e, rd, rt);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rt);
    int ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_slli(e, (unsigned)ar_rd, (unsigned)ar_rt, sa);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_srl_imm(JitEmit *e, uint32_t insn) {
    if (MIPS_RS(insn) != 0u) return false;
    unsigned rd = MIPS_RD(insn);
    unsigned rt = MIPS_RT(insn);
    unsigned sa = MIPS_SA(insn);
    if (rd == 0u) return true;
    if (rt == 0u) return emit_movi_mips(e, rd, 0u);
    if (sa == 0u) return emit_move_mips(e, rd, rt);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rt);
    int ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_srli(e, (unsigned)ar_rd, (unsigned)ar_rt, sa);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_sra_imm(JitEmit *e, uint32_t insn) {
    if (MIPS_RS(insn) != 0u) return false;
    unsigned rd = MIPS_RD(insn);
    unsigned rt = MIPS_RT(insn);
    unsigned sa = MIPS_SA(insn);
    if (rd == 0u) return true;
    if (rt == 0u) return emit_movi_mips(e, rd, 0u);
    if (sa == 0u) return emit_move_mips(e, rd, rt);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rt);
    int ar_rt, ar_rd;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid, &ar_rd)) return false;
    if (ar_rd >= 0) {
        jit_emit_srai(e, (unsigned)ar_rd, (unsigned)ar_rt, sa);
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_shift_variable(JitEmit *e, uint32_t insn, unsigned kind) {
    if (MIPS_SA(insn) != 0u) return false;
    unsigned rd = MIPS_RD(insn);
    unsigned rt = MIPS_RT(insn);
    unsigned rs = MIPS_RS(insn);
    if (rd == 0u) return true;
    if (rt == 0u) return emit_movi_mips(e, rd, 0u);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd) | hot_mask_for_mips(rt) | hot_mask_for_mips(rs);
    int ar_rt, ar_rs, ar_rd;
    if (!materialize_mips_reg(e, &sc, rt, avoid, &ar_rt)) return false;
    if (!materialize_mips_reg(e, &sc, rs, avoid | (1u << (unsigned)ar_rt), &ar_rs)) return false;
    if (!materialize_dest_reg(e, &sc, rd, avoid | (1u << (unsigned)ar_rt) | (1u << (unsigned)ar_rs), &ar_rd)) return false;
    if (ar_rd >= 0) {
        if (kind == MIPS_FUNCT_SLLV) {
            jit_emit_ssl(e, (unsigned)ar_rs);
            jit_emit_sll(e, (unsigned)ar_rd, (unsigned)ar_rt);
        } else if (kind == MIPS_FUNCT_SRLV) {
            jit_emit_ssr(e, (unsigned)ar_rs);
            jit_emit_srl(e, (unsigned)ar_rd, (unsigned)ar_rt);
        } else {
            jit_emit_ssr(e, (unsigned)ar_rs);
            jit_emit_sra(e, (unsigned)ar_rd, (unsigned)ar_rt);
        }
        emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_mfhi_mflo(JitEmit *e, uint32_t insn, size_t source_offset) {
    if (MIPS_RS(insn) != 0u || MIPS_RT(insn) != 0u || MIPS_SA(insn) != 0u) return false;
    unsigned rd = MIPS_RD(insn);
    if (rd == 0u) return true;
    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rd);
    int ar_rd = mips_hot_ar(rd);
    if (ar_rd < 0) {
        ar_rd = scratch_take(e, &sc, avoid);
        if (ar_rd < 0) return false;
    }
    jit_emit_l32i(e, (unsigned)ar_rd, XT_A2, source_offset);
    emit_store_mips_reg(e, rd, (unsigned)ar_rd);
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_mthi_mtlo(JitEmit *e, uint32_t insn, size_t dest_offset) {
    if (MIPS_RT(insn) != 0u || MIPS_RD(insn) != 0u || MIPS_SA(insn) != 0u) return false;
    unsigned rs = MIPS_RS(insn);
    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rs);
    int ar_rs;
    if (!materialize_mips_reg(e, &sc, rs, avoid, &ar_rs)) return false;
    jit_emit_s32i(e, (unsigned)ar_rs, XT_A2, dest_offset);
    scratch_restore(e, &sc);
    return !e->failed;
}


#if JIT_USE_WINDOWED_ABI
static void jit_mips_multdiv_helper(psxRegisters *regs, uint32_t insn) {
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    uint32_t lhs = regs->GPR.r[rs];
    uint32_t rhs = regs->GPR.r[rt];

    switch (MIPS_FUNCT(insn)) {
        case MIPS_FUNCT_MULT: {
            int64_t product = (int64_t)(int32_t)lhs * (int64_t)(int32_t)rhs;
            regs->GPR.r[32] = (uint32_t)((uint64_t)product >> 32);
            regs->GPR.r[33] = (uint32_t)product;
            break;
        }
        case MIPS_FUNCT_MULTU: {
            uint64_t product = (uint64_t)lhs * (uint64_t)rhs;
            regs->GPR.r[32] = (uint32_t)(product >> 32);
            regs->GPR.r[33] = (uint32_t)product;
            break;
        }
        case MIPS_FUNCT_DIV: {
            int32_t s_lhs = (int32_t)lhs;
            int32_t s_rhs = (int32_t)rhs;
            if (s_rhs == 0) {
                regs->GPR.r[32] = lhs;
                regs->GPR.r[33] = (s_lhs < 0) ? 1u : UINT32_MAX;
            } else if (s_lhs == INT32_MIN && s_rhs == -1) {
                regs->GPR.r[32] = 0u;
                regs->GPR.r[33] = (uint32_t)INT32_MIN;
            } else {
                regs->GPR.r[33] = (uint32_t)(s_lhs / s_rhs);
                regs->GPR.r[32] = (uint32_t)(s_lhs % s_rhs);
            }
            break;
        }
        case MIPS_FUNCT_DIVU:
            if (rhs == 0u) {
                regs->GPR.r[32] = lhs;
                regs->GPR.r[33] = UINT32_MAX;
            } else {
                regs->GPR.r[33] = lhs / rhs;
                regs->GPR.r[32] = lhs % rhs;
            }
            break;
        default:
            break;
    }
}
#endif

static bool compile_multdiv(JitEmit *e, uint32_t insn) {
    if (MIPS_RD(insn) != 0u || MIPS_SA(insn) != 0u) return false;
#if !JIT_USE_WINDOWED_ABI
    JIT_UNUSED(e);
    return false;
#else
    /* Mult/div are much rarer than ALU/memory ops and exact HI/LO edge cases are
     * easy to get wrong in hand-emitted Xtensa. Flush hot regs, call a tiny C
     * helper with the architectural instruction word, then reload hot regs.
     * For windowed Xtensa ABI call8, the callee sees caller a10/a11 as a2/a3.
     */
    emit_flush_hot_regs(e, 0u);
    jit_emit_movi32(e, XT_A8, (uint32_t)(uintptr_t)jit_mips_multdiv_helper);
    jit_emit_mov(e, XT_A10, XT_A2);
    jit_emit_movi32(e, XT_A11, insn);
    jit_emit_callx8(e, XT_A8);
    emit_load_hot_regs(e);
    return !e->failed;
#endif
}

static bool emit_effective_addr(JitEmit *e, Scratch *sc, unsigned addr_ar,
                                unsigned base_mips, int32_t offset, uint32_t avoid) {
    if (base_mips == 0u) {
        jit_emit_movi32(e, addr_ar, (uint32_t)offset);
        return !e->failed;
    }

    int base_ar = mips_hot_ar(base_mips);
    if (base_ar >= 0) {
        if (offset == 0) {
            jit_emit_mov(e, addr_ar, (unsigned)base_ar);
        } else if (jit_s8(offset)) {
            jit_emit_addi(e, addr_ar, (unsigned)base_ar, offset);
        } else {
            int imm_ar = scratch_take(e, sc, avoid | (1u << addr_ar));
            if (imm_ar < 0) return false;
            jit_emit_movi32(e, (unsigned)imm_ar, (uint32_t)offset);
            jit_emit_add(e, addr_ar, (unsigned)base_ar, (unsigned)imm_ar);
            scratch_release(sc, imm_ar);
        }
    } else {
        jit_emit_l32i(e, addr_ar, XT_A2, JIT_MIPS_GPR_OFFSET(base_mips));
        if (offset != 0) {
            if (jit_s8(offset)) {
                jit_emit_addi(e, addr_ar, addr_ar, offset);
            } else {
                int imm_ar = scratch_take(e, sc, avoid | (1u << addr_ar));
                if (imm_ar < 0) return false;
                jit_emit_movi32(e, (unsigned)imm_ar, (uint32_t)offset);
                jit_emit_add(e, addr_ar, addr_ar, (unsigned)imm_ar);
                scratch_release(sc, imm_ar);
            }
        }
    }
    return !e->failed;
}

static void emit_mark_bail_flag(JitEmit *e, unsigned value_ar, unsigned ptr_ar) {
    if (value_ar == ptr_ar) {
        e->failed = true;
        return;
    }
    jit_emit_l32r_value(e, ptr_ar, (uint32_t)(uintptr_t)&g_jit_bail_flag);
    jit_emit_movi12(e, value_ar, 1);
    jit_emit_s32i(e, value_ar, ptr_ar, 0u);
}

static bool emit_bail_if_zero(JitEmit *e, Scratch *sc, unsigned checked_ar, unsigned tmp_ar,
                              uint32_t current_pc, uint32_t cycles_before) {
#if JIT_STRICT_MEMRLUT_NULL_CHECKS
    if (checked_ar == tmp_ar) {
        e->failed = true;
        return false;
    }
    JitPatch ok;
    jit_emit_bnez_placeholder(e, &ok, checked_ar);
    emit_mark_bail_flag(e, tmp_ar, checked_ar);
    emit_exit_imm(e, current_pc, cycles_before, sc->saved_mask);
    if (!jit_patch_bri12(ok, e->p)) {
        e->failed = true;
        return false;
    }
#else
    JIT_UNUSED(e);
    JIT_UNUSED(sc);
    JIT_UNUSED(checked_ar);
    JIT_UNUSED(tmp_ar);
    JIT_UNUSED(current_pc);
    JIT_UNUSED(cycles_before);
#endif
    return !e->failed;
}

static bool emit_alignment_guard(JitEmit *e, Scratch *sc, unsigned addr_ar, unsigned tmp_ar,
                                 unsigned low_bits, uint32_t current_pc, uint32_t cycles_before) {
#if JIT_STRICT_ALIGNMENT_CHECKS
    if (low_bits == 0u) {
        JIT_UNUSED(e);
        JIT_UNUSED(sc);
        JIT_UNUSED(addr_ar);
        JIT_UNUSED(tmp_ar);
        JIT_UNUSED(current_pc);
        JIT_UNUSED(cycles_before);
        return true;
    }
    if (low_bits > 2u) {
        e->failed = true;
        return false;
    }
    /* Reuse an already allocated scratch register. Taking a fresh scratch here can
     * spill/clobber a hot MIPS register that is still needed by the load/store.
     */
    jit_emit_extui(e, tmp_ar, addr_ar, 0u, low_bits);
    JitPatch aligned;
    jit_emit_beqz_placeholder(e, &aligned, tmp_ar);
    emit_mark_bail_flag(e, addr_ar, tmp_ar);
    emit_exit_imm(e, current_pc, cycles_before, sc->saved_mask);
    if (!jit_patch_bri12(aligned, e->p)) {
        e->failed = true;
        return false;
    }
#else
    JIT_UNUSED(e);
    JIT_UNUSED(sc);
    JIT_UNUSED(addr_ar);
    JIT_UNUSED(tmp_ar);
    JIT_UNUSED(low_bits);
    JIT_UNUSED(current_pc);
    JIT_UNUSED(cycles_before);
#endif
    return !e->failed;
}

static bool emit_word_alignment_guard(JitEmit *e, Scratch *sc, unsigned addr_ar, unsigned tmp_ar,
                                      uint32_t current_pc, uint32_t cycles_before) {
    return emit_alignment_guard(e, sc, addr_ar, tmp_ar, 2u, current_pc, cycles_before);
}

static bool emit_main_ram_guard(JitEmit *e, Scratch *sc, unsigned addr_ar, unsigned tmp_ar,
                                uint32_t current_pc, uint32_t cycles_before) {
#if JIT_DIRECT_MAIN_RAM_ONLY
    if (addr_ar == tmp_ar) {
        e->failed = true;
        return false;
    }
#if JIT_NORMALIZE_MAIN_RAM_MIRRORS
    const uint32_t direct_range = (uint32_t)JIT_PSX_MAIN_RAM_MIRROR_BYTES;
#else
    const uint32_t direct_range = (uint32_t)JIT_PSX_MAIN_RAM_SIZE_BYTES;
#endif
    const uint32_t outside_main_ram = (uint32_t)JIT_PSX_PHYS_MASK & ~(direct_range - 1u);
    if (outside_main_ram != 0u) {
        jit_emit_movi32(e, tmp_ar, outside_main_ram);
        jit_emit_and(e, tmp_ar, addr_ar, tmp_ar);
        JitPatch ok;
        jit_emit_beqz_placeholder(e, &ok, tmp_ar);
        emit_mark_bail_flag(e, addr_ar, tmp_ar);
        emit_exit_imm(e, current_pc, cycles_before, sc->saved_mask);
        if (!jit_patch_bri12(ok, e->p)) {
            e->failed = true;
            return false;
        }
    }
#else
    JIT_UNUSED(e);
    JIT_UNUSED(sc);
    JIT_UNUSED(addr_ar);
    JIT_UNUSED(tmp_ar);
    JIT_UNUSED(current_pc);
    JIT_UNUSED(cycles_before);
#endif
    return !e->failed;
}

static JIT_ATTR_UNUSED void emit_l32i_large_offset(JitEmit *e, unsigned dst_ar, unsigned base_ar, size_t byte_offset) {
    if (jit_u32_offset_ok(byte_offset)) {
        jit_emit_l32i(e, dst_ar, base_ar, byte_offset);
        return;
    }
    jit_emit_movi32(e, dst_ar, (uint32_t)byte_offset);
    jit_emit_add(e, dst_ar, base_ar, dst_ar);
    jit_emit_l32i(e, dst_ar, dst_ar, 0u);
}

static JIT_ATTR_UNUSED bool emit_main_ram_host_addr(JitEmit *e, Scratch *sc, unsigned addr_ar, unsigned host_ar,
                                    uint32_t current_pc, uint32_t cycles_before) {
#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    if (addr_ar == host_ar) {
        e->failed = true;
        return false;
    }
#if JIT_NORMALIZE_MAIN_RAM_MIRRORS
    jit_emit_movi32(e, host_ar, (uint32_t)JIT_PSX_MAIN_RAM_SIZE_BYTES - 1u);
    jit_emit_and(e, addr_ar, addr_ar, host_ar);
#else
    jit_emit_movi32(e, host_ar, (uint32_t)JIT_PSX_MAIN_RAM_SIZE_BYTES - 1u);
    jit_emit_and(e, addr_ar, addr_ar, host_ar);
#endif
    emit_l32i_large_offset(e, host_ar, XT_A2, JIT_PSX_PTRS_PSXM_OFFSET);
    if (!emit_bail_if_zero(e, sc, host_ar, addr_ar, current_pc, cycles_before)) return false;
    jit_emit_add(e, host_ar, host_ar, addr_ar);
    return !e->failed;
#else
    JIT_UNUSED(e);
    JIT_UNUSED(sc);
    JIT_UNUSED(addr_ar);
    JIT_UNUSED(host_ar);
    JIT_UNUSED(current_pc);
    JIT_UNUSED(cycles_before);
    return false;
#endif
}

static JIT_ATTR_UNUSED bool emit_memlut_host_addr(JitEmit *e, Scratch *sc, unsigned addr_ar,
                                  unsigned page_ar, unsigned lut_base_ar,
                                  uint32_t current_pc, uint32_t cycles_before) {
    if (page_ar == lut_base_ar) {
        e->failed = true;
        return false;
    }
    if (!emit_bail_if_zero(e, sc, lut_base_ar, page_ar, current_pc, cycles_before)) return false;
    jit_emit_extui(e, page_ar, addr_ar, 16u, 16u);
    jit_emit_addx4(e, page_ar, lut_base_ar, page_ar);
    jit_emit_l32i(e, page_ar, page_ar, 0u);
    if (!emit_bail_if_zero(e, sc, page_ar, addr_ar, current_pc, cycles_before)) return false;
#if JIT_MEM_LUT_PAGE_BASE_IS_BIASED
    jit_emit_add(e, page_ar, page_ar, addr_ar);
#else
    jit_emit_extui(e, addr_ar, addr_ar, 0u, 16u);
    jit_emit_add(e, page_ar, page_ar, addr_ar);
#endif
    return !e->failed;
}

static JIT_ATTR_UNUSED bool emit_memrlut_host_addr(JitEmit *e, Scratch *sc, unsigned addr_ar,
                                   unsigned page_ar, uint32_t current_pc,
                                   uint32_t cycles_before) {
    return emit_memlut_host_addr(e, sc, addr_ar, page_ar, XT_A3, current_pc, cycles_before);
}

static JIT_ATTR_UNUSED bool emit_memwlut_host_addr(JitEmit *e, Scratch *sc, unsigned addr_ar,
                                   unsigned page_ar, unsigned lut_ar,
                                   uint32_t current_pc, uint32_t cycles_before) {
#if JIT_HAS_MEMWLUT
    jit_emit_l32r_value(e, lut_ar, (uint32_t)(uintptr_t)&g_jit_mem_wlut);
    jit_emit_l32i(e, lut_ar, lut_ar, 0u);
    return emit_memlut_host_addr(e, sc, addr_ar, page_ar, lut_ar, current_pc, cycles_before);
#elif JIT_ALLOW_STORE_THROUGH_MEMRLUT_WITHOUT_MEMWLUT
    JIT_UNUSED(lut_ar);
    return emit_memrlut_host_addr(e, sc, addr_ar, page_ar, current_pc, cycles_before);
#else
    JIT_UNUSED(sc);
    JIT_UNUSED(addr_ar);
    JIT_UNUSED(page_ar);
    JIT_UNUSED(lut_ar);
    JIT_UNUSED(current_pc);
    JIT_UNUSED(cycles_before);
    e->failed = true;
    return false;
#endif
}

static void emit_record_last_store_addr(JitEmit *e, unsigned addr_ar, unsigned tmp_ar) {
#if JIT_TRACK_STORES_FOR_SMC
    if (addr_ar == tmp_ar) {
        e->failed = true;
        return;
    }
    jit_emit_l32r_value(e, tmp_ar, (uint32_t)(uintptr_t)&g_jit_last_store_addr);
    jit_emit_s32i(e, addr_ar, tmp_ar, 0u);
#else
    JIT_UNUSED(e);
    JIT_UNUSED(addr_ar);
    JIT_UNUSED(tmp_ar);
#endif
}

static void emit_set_store_flag(JitEmit *e, unsigned tmp_ar, unsigned value_ar) {
#if JIT_TRACK_STORES_FOR_SMC
    if (tmp_ar == value_ar) {
        e->failed = true;
        return;
    }
    jit_emit_l32r_value(e, tmp_ar, (uint32_t)(uintptr_t)&g_jit_store_flag);
    jit_emit_movi12(e, value_ar, 1);
    jit_emit_s32i(e, value_ar, tmp_ar, 0u);
#else
    JIT_UNUSED(e);
    JIT_UNUSED(tmp_ar);
    JIT_UNUSED(value_ar);
#endif
}

static bool emit_load_from_host(JitEmit *e, unsigned dst_ar, unsigned host_ar,
                                unsigned byte_width, bool sign_extend) {
    switch (byte_width) {
        case 1u:
            jit_emit_l8ui(e, dst_ar, host_ar, 0u);
            if (sign_extend) {
                jit_emit_slli(e, dst_ar, dst_ar, 24u);
                jit_emit_srai(e, dst_ar, dst_ar, 24u);
            }
            break;
        case 2u:
            if (sign_extend) {
                jit_emit_l16si(e, dst_ar, host_ar, 0u);
            } else {
                jit_emit_l16ui(e, dst_ar, host_ar, 0u);
            }
            break;
        case 4u:
            jit_emit_l32i(e, dst_ar, host_ar, 0u);
            break;
        default:
            e->failed = true;
            break;
    }
    return !e->failed;
}

static bool emit_store_to_host(JitEmit *e, unsigned value_ar, unsigned host_ar,
                               unsigned byte_width) {
    switch (byte_width) {
        case 1u:
            jit_emit_s8i(e, value_ar, host_ar, 0u);
            break;
        case 2u:
            jit_emit_s16i(e, value_ar, host_ar, 0u);
            break;
        case 4u:
            jit_emit_s32i(e, value_ar, host_ar, 0u);
            break;
        default:
            e->failed = true;
            break;
    }
    return !e->failed;
}

static bool compile_load_mem(JitEmit *e, uint32_t insn, uint32_t current_pc,
                             uint32_t cycles_before, unsigned byte_width,
                             bool sign_extend) {
    unsigned rt = MIPS_RT(insn);
    unsigned base = MIPS_RS(insn);
    int32_t off = (int32_t)MIPS_SIMM(insn);
    if (rt == 0u) return false;
    if (byte_width != 1u && byte_width != 2u && byte_width != 4u) return false;

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(base);
    int addr_ar = scratch_take(e, &sc, avoid);
    if (addr_ar < 0) return false;

    if (!emit_effective_addr(e, &sc, (unsigned)addr_ar, base, off, avoid)) return false;
    int host_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)addr_ar));
    if (host_ar < 0) return false;
    unsigned low_bits = byte_width == 4u ? 2u : (byte_width == 2u ? 1u : 0u);
    if (!emit_alignment_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                              low_bits, current_pc, cycles_before)) return false;
    if (!emit_main_ram_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    if (!emit_main_ram_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                 current_pc, cycles_before)) return false;
#else
    if (!emit_memrlut_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                current_pc, cycles_before)) return false;
#endif

    int dst_ar = mips_hot_ar(rt);
    if (dst_ar < 0) dst_ar = addr_ar;
    if (!emit_load_from_host(e, (unsigned)dst_ar, (unsigned)host_ar, byte_width, sign_extend)) return false;
    if (mips_hot_ar(rt) < 0) {
        jit_emit_s32i(e, (unsigned)dst_ar, XT_A2, JIT_MIPS_GPR_OFFSET(rt));
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_store_mem(JitEmit *e, uint32_t insn, uint32_t current_pc,
                              uint32_t cycles_before, unsigned byte_width) {
    unsigned rt = MIPS_RT(insn);
    unsigned base = MIPS_RS(insn);
    int32_t off = (int32_t)MIPS_SIMM(insn);
    if (byte_width != 1u && byte_width != 2u && byte_width != 4u) return false;

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(base);
    int addr_ar = scratch_take(e, &sc, avoid);
    if (addr_ar < 0) return false;

    if (!emit_effective_addr(e, &sc, (unsigned)addr_ar, base, off, avoid)) return false;
    int host_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)addr_ar));
    if (host_ar < 0) return false;
#if !JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    int lut_ar = -1;
#endif
    unsigned low_bits = byte_width == 4u ? 2u : (byte_width == 2u ? 1u : 0u);
    if (!emit_alignment_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                              low_bits, current_pc, cycles_before)) return false;
    if (!emit_main_ram_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
    emit_record_last_store_addr(e, (unsigned)addr_ar, (unsigned)host_ar);
    if (e->failed) return false;
#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    if (!emit_main_ram_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                 current_pc, cycles_before)) return false;
#else
    /* After the effective address is materialized, the base register is no
     * longer needed. Do not keep it in the avoid set here; otherwise a store
     * whose base and value are both scratch-backed hot registers can exhaust
     * A12-A15 before the write-LUT address is computed.
     */
    uint32_t lut_avoid = hot_mask_for_mips(rt) | (1u << (unsigned)addr_ar) |
                         (1u << (unsigned)host_ar);
    lut_ar = scratch_take(e, &sc, lut_avoid);
    if (lut_ar < 0) return false;
    if (!emit_memwlut_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, (unsigned)lut_ar,
                                current_pc, cycles_before)) return false;
#endif

    int value_ar = mips_hot_ar(rt);
    if (value_ar < 0) {
        if (rt == 0u) {
            jit_emit_movi12(e, (unsigned)addr_ar, 0);
        } else {
            jit_emit_l32i(e, (unsigned)addr_ar, XT_A2, JIT_MIPS_GPR_OFFSET(rt));
        }
        value_ar = addr_ar;
    }
    if (!emit_store_to_host(e, (unsigned)value_ar, (unsigned)host_ar, byte_width)) return false;
    emit_set_store_flag(e, (unsigned)host_ar, (unsigned)addr_ar);
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_lw(JitEmit *e, uint32_t insn, uint32_t current_pc, uint32_t cycles_before) {
    unsigned rt = MIPS_RT(insn);
    unsigned base = MIPS_RS(insn);
    int32_t off = (int32_t)MIPS_SIMM(insn);
    if (rt == 0u) return false;

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(base);
    int addr_ar = scratch_take(e, &sc, avoid);
    if (addr_ar < 0) return false;

    if (!emit_effective_addr(e, &sc, (unsigned)addr_ar, base, off, avoid)) return false;
    int host_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)addr_ar));
    if (host_ar < 0) return false;
    if (!emit_word_alignment_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
    if (!emit_main_ram_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    if (!emit_main_ram_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                 current_pc, cycles_before)) return false;
#else
    if (!emit_memrlut_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                current_pc, cycles_before)) return false;
#endif

    int dst_ar = mips_hot_ar(rt);
    if (dst_ar >= 0) {
        jit_emit_l32i(e, (unsigned)dst_ar, (unsigned)host_ar, 0u);
    } else {
        jit_emit_l32i(e, (unsigned)addr_ar, (unsigned)host_ar, 0u);
        jit_emit_s32i(e, (unsigned)addr_ar, XT_A2, JIT_MIPS_GPR_OFFSET(rt));
    }
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_sw(JitEmit *e, uint32_t insn, uint32_t current_pc, uint32_t cycles_before) {
    unsigned rt = MIPS_RT(insn);
    unsigned base = MIPS_RS(insn);
    int32_t off = (int32_t)MIPS_SIMM(insn);

    Scratch sc;
    scratch_init(&sc);
    uint32_t avoid = hot_mask_for_mips(rt) | hot_mask_for_mips(base);
    int addr_ar = scratch_take(e, &sc, avoid);
    if (addr_ar < 0) return false;

    if (!emit_effective_addr(e, &sc, (unsigned)addr_ar, base, off, avoid)) return false;
    int host_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)addr_ar));
    if (host_ar < 0) return false;
#if !JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    int lut_ar = -1;
#endif
    if (!emit_word_alignment_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
    if (!emit_main_ram_guard(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, current_pc, cycles_before)) return false;
    emit_record_last_store_addr(e, (unsigned)addr_ar, (unsigned)host_ar);
    if (e->failed) return false;
#if JIT_USE_PTRS_PSXM_FOR_MAIN_RAM
    if (!emit_main_ram_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar,
                                 current_pc, cycles_before)) return false;
#else
    /* After the effective address is materialized, the base register is no
     * longer needed. Do not keep it in the avoid set here; otherwise a store
     * whose base and value are both scratch-backed hot registers can exhaust
     * A12-A15 before the write-LUT address is computed.
     */
    uint32_t lut_avoid = hot_mask_for_mips(rt) | (1u << (unsigned)addr_ar) |
                         (1u << (unsigned)host_ar);
    lut_ar = scratch_take(e, &sc, lut_avoid);
    if (lut_ar < 0) return false;
    if (!emit_memwlut_host_addr(e, &sc, (unsigned)addr_ar, (unsigned)host_ar, (unsigned)lut_ar,
                                current_pc, cycles_before)) return false;
#endif

    int value_ar = mips_hot_ar(rt);
    if (value_ar < 0) {
        if (rt == 0u) {
            jit_emit_movi12(e, (unsigned)addr_ar, 0);
        } else {
            jit_emit_l32i(e, (unsigned)addr_ar, XT_A2, JIT_MIPS_GPR_OFFSET(rt));
        }
        value_ar = addr_ar;
    }
    jit_emit_s32i(e, (unsigned)value_ar, (unsigned)host_ar, 0u);
    emit_set_store_flag(e, (unsigned)host_ar, (unsigned)addr_ar);
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool is_control(uint32_t insn) {
    unsigned op = insn >> 26;
    if (op == MIPS_OP_BEQ || op == MIPS_OP_BNE || op == MIPS_OP_BLEZ ||
        op == MIPS_OP_BGTZ || op == MIPS_OP_REGIMM || op == MIPS_OP_J || op == MIPS_OP_JAL) {
        return true;
    }
    if (op == MIPS_OP_SPECIAL &&
        (MIPS_FUNCT(insn) == MIPS_FUNCT_JR || MIPS_FUNCT(insn) == MIPS_FUNCT_JALR)) {
        return true;
    }
    return false;
}

static JIT_ATTR_UNUSED bool is_store_mem(uint32_t insn) {
    unsigned op = insn >> 26;
    return op == MIPS_OP_SB || op == MIPS_OP_SH || op == MIPS_OP_SW ||
           op == MIPS_OP_SWL || op == MIPS_OP_SWR;
}

static bool compile_one_noncontrol(JitEmit *e, uint32_t insn, uint32_t pc,
                                   uint32_t cycles_before, bool allow_mem) {
    if (insn == 0u) return true;

    unsigned op = insn >> 26;
    switch (op) {
        case MIPS_OP_SPECIAL:
            switch (MIPS_FUNCT(insn)) {
                case MIPS_FUNCT_ADDU:
                    return mips_rtype_sa_zero(insn) ? compile_addu(e, insn) : false;
                case MIPS_FUNCT_SUBU:
                    return mips_rtype_sa_zero(insn) ? compile_subu(e, insn) : false;
                case MIPS_FUNCT_AND:
                    return mips_rtype_sa_zero(insn) ? compile_and_reg(e, insn) : false;
                case MIPS_FUNCT_OR:
                    return mips_rtype_sa_zero(insn) ? compile_or_reg(e, insn) : false;
                case MIPS_FUNCT_XOR:
                    return mips_rtype_sa_zero(insn) ? compile_xor_reg(e, insn) : false;
                case MIPS_FUNCT_NOR:
                    return mips_rtype_sa_zero(insn) ? compile_nor_reg(e, insn) : false;
                case MIPS_FUNCT_SLT:
                    return mips_rtype_sa_zero(insn) ? compile_slt_reg(e, insn, false) : false;
                case MIPS_FUNCT_SLTU:
                    return mips_rtype_sa_zero(insn) ? compile_slt_reg(e, insn, true) : false;
                case MIPS_FUNCT_SLL:
                    return compile_sll_imm(e, insn);
                case MIPS_FUNCT_SRL:
                    return compile_srl_imm(e, insn);
                case MIPS_FUNCT_SRA:
                    return compile_sra_imm(e, insn);
                case MIPS_FUNCT_SLLV:
                case MIPS_FUNCT_SRLV:
                case MIPS_FUNCT_SRAV:
                    return compile_shift_variable(e, insn, MIPS_FUNCT(insn));
                case MIPS_FUNCT_MFHI:
                    return compile_mfhi_mflo(e, insn, JIT_MIPS_HI_OFFSET);
                case MIPS_FUNCT_MFLO:
                    return compile_mfhi_mflo(e, insn, JIT_MIPS_LO_OFFSET);
                case MIPS_FUNCT_MTHI:
                    return compile_mthi_mtlo(e, insn, JIT_MIPS_HI_OFFSET);
                case MIPS_FUNCT_MTLO:
                    return compile_mthi_mtlo(e, insn, JIT_MIPS_LO_OFFSET);
                case MIPS_FUNCT_MULT:
                case MIPS_FUNCT_MULTU:
                case MIPS_FUNCT_DIV:
                case MIPS_FUNCT_DIVU:
                    return compile_multdiv(e, insn);
                default:
                    return false;
            }
        case MIPS_OP_ADDIU:
            return compile_addiu(e, insn);
        case MIPS_OP_SLTI:
            return compile_slti(e, insn, false);
        case MIPS_OP_SLTIU:
            return compile_slti(e, insn, true);
        case MIPS_OP_ANDI:
            return compile_andi(e, insn);
        case MIPS_OP_ORI:
            return compile_ori(e, insn);
        case MIPS_OP_XORI:
            return compile_xori(e, insn);
        case MIPS_OP_LUI:
            return compile_lui(e, insn);
        case MIPS_OP_LB:
            return allow_mem ? compile_load_mem(e, insn, pc, cycles_before, 1u, true) : false;
        case MIPS_OP_LBU:
            return allow_mem ? compile_load_mem(e, insn, pc, cycles_before, 1u, false) : false;
        case MIPS_OP_LH:
            return allow_mem ? compile_load_mem(e, insn, pc, cycles_before, 2u, true) : false;
        case MIPS_OP_LHU:
            return allow_mem ? compile_load_mem(e, insn, pc, cycles_before, 2u, false) : false;
        case MIPS_OP_LW:
            return allow_mem ? compile_lw(e, insn, pc, cycles_before) : false;
        case MIPS_OP_SB:
            return allow_mem ? compile_store_mem(e, insn, pc, cycles_before, 1u) : false;
        case MIPS_OP_SH:
            return allow_mem ? compile_store_mem(e, insn, pc, cycles_before, 2u) : false;
        case MIPS_OP_SW:
            return allow_mem ? compile_sw(e, insn, pc, cycles_before) : false;
        default:
            return false;
    }
}

static uint32_t mips_branch_target(uint32_t pc, uint32_t insn) {
    int32_t simm = (int32_t)MIPS_SIMM(insn);
    /* Do not left-shift a negative signed value: that is undefined in C. */
    return pc + 4u + (uint32_t)(simm * 4);
}

static uint32_t mips_jump_target(uint32_t pc, uint32_t insn) {
    return ((pc + 4u) & 0xf0000000u) | (MIPS_TARGET(insn) << 2);
}

static bool compile_delay_slot(JitEmit *e, psxRegisters *regs, uint32_t delay_pc,
                               uint32_t cycles_before_delay) {
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);
    uint32_t delay = JIT_FETCH32(regs, delay_pc);
    if (is_control(delay)) return false;
    return compile_one_noncontrol(e, delay, delay_pc, cycles_before_delay, false);
}

static bool emit_branch_compare(JitEmit *e, uint32_t insn, bool branch_if_ne,
                                JitPatch *taken_patch, Scratch *cond_sc) {
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    scratch_init(cond_sc);

    if (rs == rt) {
        if (branch_if_ne) {
            taken_patch->at = NULL;
            return true;
        }
        int tmp = scratch_take(e, cond_sc, hot_mask_for_mips(rs));
        if (tmp < 0) return false;
        jit_emit_movi12(e, (unsigned)tmp, 1);
        jit_emit_bnez_placeholder(e, taken_patch, (unsigned)tmp);
        return !e->failed;
    }

    uint32_t avoid = hot_mask_for_mips(rs) | hot_mask_for_mips(rt);
    if (rs == 0u || rt == 0u) {
        unsigned nonzero = (rs == 0u) ? rt : rs;
        int ar;
        if (!materialize_mips_reg(e, cond_sc, nonzero, avoid, &ar)) return false;
        if (branch_if_ne) {
            jit_emit_bnez_placeholder(e, taken_patch, (unsigned)ar);
        } else {
            jit_emit_beqz_placeholder(e, taken_patch, (unsigned)ar);
        }
        return !e->failed;
    }

    int ar_rs, ar_rt;
    if (!materialize_mips_reg(e, cond_sc, rs, avoid, &ar_rs)) return false;
    if (!materialize_mips_reg(e, cond_sc, rt, avoid, &ar_rt)) return false;
    int diff_ar = scratch_take(e, cond_sc, avoid);
    if (diff_ar < 0) return false;
    jit_emit_xor(e, (unsigned)diff_ar, (unsigned)ar_rs, (unsigned)ar_rt);
    if (branch_if_ne) {
        jit_emit_bnez_placeholder(e, taken_patch, (unsigned)diff_ar);
    } else {
        jit_emit_beqz_placeholder(e, taken_patch, (unsigned)diff_ar);
    }
    return !e->failed;
}

static bool compile_branch(JitEmit *e, psxRegisters *regs, uint32_t insn, uint32_t pc,
                           uint32_t cycles_before, bool branch_if_ne) {
    uint32_t delay_pc = pc + 4u;
    uint32_t target_pc = mips_branch_target(pc, insn);
    uint32_t fallthrough_pc = pc + 8u;
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);

    uint32_t delay = JIT_FETCH32(regs, delay_pc);
    if (is_control(delay)) return false;
    unsigned rs = MIPS_RS(insn);
    unsigned rt = MIPS_RT(insn);
    if (rs == rt) {
        bool taken = !branch_if_ne;
        if (!compile_delay_slot(e, regs, delay_pc, cycles_before + 1u)) return false;
        emit_exit_imm(e, taken ? target_pc : fallthrough_pc, cycles_before + 2u, 0u);
        return !e->failed;
    }

    JitPatch taken;
    Scratch cond_sc;
    if (!emit_branch_compare(e, insn, branch_if_ne, &taken, &cond_sc)) return false;

    scratch_restore(e, &cond_sc);
    if (!compile_one_noncontrol(e, delay, delay_pc, cycles_before + 1u, false)) return false;
    emit_exit_imm(e, fallthrough_pc, cycles_before + 2u, 0u);

    if (taken.at == NULL) {
        return !e->failed;
    }
    uint8_t *taken_label = e->p;
    if (!jit_patch_bri12(taken, taken_label)) {
        e->failed = true;
        return false;
    }
    scratch_restore(e, &cond_sc);
    if (!compile_one_noncontrol(e, delay, delay_pc, cycles_before + 1u, false)) return false;
    emit_exit_imm(e, target_pc, cycles_before + 2u, 0u);
    return !e->failed;
}

typedef enum JitRegBranchKind {
    JIT_BRANCH_BLTZ = 0,
    JIT_BRANCH_BGEZ,
    JIT_BRANCH_BLEZ,
    JIT_BRANCH_BGTZ
} JitRegBranchKind;

static bool compile_reg_branch(JitEmit *e, psxRegisters *regs, uint32_t insn, uint32_t pc,
                               uint32_t cycles_before, JitRegBranchKind kind, bool link) {
    uint32_t delay_pc = pc + 4u;
    uint32_t target_pc = mips_branch_target(pc, insn);
    uint32_t fallthrough_pc = pc + 8u;
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);
    uint32_t delay = JIT_FETCH32(regs, delay_pc);
    if (is_control(delay)) return false;

    /* BLTZAL/BGEZAL write $ra for both taken and not-taken paths on MIPS I.
     * Keep rs=$ra conservative; architectural manuals call that case
     * restartability-unfriendly/UNPREDICTABLE for branch-and-link.
     */
    if (link) {
        if (MIPS_RS(insn) == 31u) return false;
        if (mips_insn_touches_reg(delay, 31u)) return false;
    }

    Scratch cond_sc;
    scratch_init(&cond_sc);
    int ar_rs;
    if (!materialize_mips_reg(e, &cond_sc, MIPS_RS(insn), hot_mask_for_mips(MIPS_RS(insn)), &ar_rs)) return false;

    JitPatch taken[2];
    size_t taken_count = 0u;
    JitPatch skip_fallthrough;
    bool has_skip = false;

    switch (kind) {
        case JIT_BRANCH_BLTZ:
            jit_emit_bltz_placeholder(e, &taken[taken_count++], (unsigned)ar_rs);
            break;
        case JIT_BRANCH_BGEZ:
            jit_emit_bgez_placeholder(e, &taken[taken_count++], (unsigned)ar_rs);
            break;
        case JIT_BRANCH_BLEZ:
            jit_emit_beqz_placeholder(e, &taken[taken_count++], (unsigned)ar_rs);
            jit_emit_bltz_placeholder(e, &taken[taken_count++], (unsigned)ar_rs);
            break;
        case JIT_BRANCH_BGTZ:
            jit_emit_beqz_placeholder(e, &skip_fallthrough, (unsigned)ar_rs);
            has_skip = true;
            jit_emit_bgez_placeholder(e, &taken[taken_count++], (unsigned)ar_rs);
            break;
        default:
            return false;
    }

    uint8_t *fallthrough_label = e->p;
    if (has_skip && !jit_patch_bri12(skip_fallthrough, fallthrough_label)) {
        e->failed = true;
        return false;
    }

    scratch_restore(e, &cond_sc);
    if (link && !emit_movi_mips(e, 31u, pc + 8u)) return false;
    if (!compile_one_noncontrol(e, delay, delay_pc, cycles_before + 1u, false)) return false;
    emit_exit_imm(e, fallthrough_pc, cycles_before + 2u, 0u);

    uint8_t *taken_label = e->p;
    for (size_t i = 0; i < taken_count; ++i) {
        if (!jit_patch_bri12(taken[i], taken_label)) {
            e->failed = true;
            return false;
        }
    }
    scratch_restore(e, &cond_sc);
    if (link && !emit_movi_mips(e, 31u, pc + 8u)) return false;
    if (!compile_one_noncontrol(e, delay, delay_pc, cycles_before + 1u, false)) return false;
    emit_exit_imm(e, target_pc, cycles_before + 2u, 0u);
    return !e->failed;
}

static bool compile_jump(JitEmit *e, psxRegisters *regs, uint32_t insn, uint32_t pc,
                         uint32_t cycles_before, bool link) {
    uint32_t delay_pc = pc + 4u;
    uint32_t target_pc = mips_jump_target(pc, insn);
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);
    uint32_t delay = JIT_FETCH32(regs, delay_pc);
    if (is_control(delay)) return false;
    if (link && mips_insn_touches_reg(delay, 31u)) return false;

    if (link) {
        if (!emit_movi_mips(e, 31u, pc + 8u)) return false;
    }
    if (!compile_one_noncontrol(e, delay, delay_pc, cycles_before + 1u, false)) return false;
    emit_exit_imm(e, target_pc, cycles_before + 2u, 0u);
    return !e->failed;
}

static bool compile_jr(JitEmit *e, psxRegisters *regs, uint32_t insn, uint32_t pc,
                       uint32_t cycles_before) {
    unsigned rs = MIPS_RS(insn);
    uint32_t delay_pc = pc + 4u;
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);
    uint32_t delay = JIT_FETCH32(regs, delay_pc);

    /* Keep JR conservative: most PSX return delay slots are NOP. */
    if (delay != 0u) return false;

    Scratch sc;
    scratch_init(&sc);
    int target_ar;
    uint32_t avoid = hot_mask_for_mips(rs);
    if (!materialize_mips_reg(e, &sc, rs, avoid, &target_ar)) return false;
    emit_exit_reg(e, (unsigned)target_ar, cycles_before + 2u, sc.saved_mask);
    return !e->failed;
}

static bool emit_movi_mips_avoid_xt(JitEmit *e, unsigned dst, uint32_t value,
                                     uint32_t avoid_xt_mask) {
    if (dst == 0u) return true;
    int dst_ar = mips_hot_ar(dst);
    if (dst_ar >= 0) {
        if ((avoid_xt_mask & (1u << (unsigned)dst_ar)) != 0u) return false;
        jit_emit_movi32(e, (unsigned)dst_ar, value);
        return !e->failed;
    }

    Scratch sc;
    scratch_init(&sc);
    int tmp = scratch_take(e, &sc, avoid_xt_mask | hot_mask_for_mips(dst));
    if (tmp < 0) return false;
    jit_emit_movi32(e, (unsigned)tmp, value);
    jit_emit_s32i(e, (unsigned)tmp, XT_A2, JIT_MIPS_GPR_OFFSET(dst));
    scratch_restore(e, &sc);
    return !e->failed;
}

static bool compile_jalr(JitEmit *e, psxRegisters *regs, uint32_t insn, uint32_t pc,
                         uint32_t cycles_before) {
    unsigned rs = MIPS_RS(insn);
    unsigned rd = MIPS_RD(insn);
    uint32_t delay_pc = pc + 4u;
    JIT_UNUSED(regs);
    JIT_UNUSED(delay_pc);
    uint32_t delay = JIT_FETCH32(regs, delay_pc);

    /* Keep register-indirect calls conservative until delay-slot memory ops are
     * native-safe. The common ABI sequence is jalr/jr with a NOP delay slot.
     */
    if (delay != 0u) return false;

    Scratch sc;
    scratch_init(&sc);
    int target_ar;
    uint32_t avoid = hot_mask_for_mips(rs) | hot_mask_for_mips(rd);
    if (!materialize_mips_reg(e, &sc, rs, avoid, &target_ar)) return false;

    int rd_hot = mips_hot_ar(rd);
    if (rd != 0u && rd_hot >= 0 && (unsigned)rd_hot == (unsigned)target_ar) {
        int copy_ar = scratch_take(e, &sc, avoid | (1u << (unsigned)target_ar));
        if (copy_ar < 0) return false;
        jit_emit_mov(e, (unsigned)copy_ar, (unsigned)target_ar);
        target_ar = copy_ar;
    }

    if (!emit_movi_mips_avoid_xt(e, rd, pc + 8u, 1u << (unsigned)target_ar)) return false;
    emit_exit_reg(e, (unsigned)target_ar, cycles_before + 2u, sc.saved_mask);
    return !e->failed;
}

static bool compile_control(JitEmit *e, psxRegisters *regs, uint32_t insn,
                            uint32_t pc, uint32_t cycles_before) {
    unsigned op = insn >> 26;
    switch (op) {
        case MIPS_OP_BEQ:
            return compile_branch(e, regs, insn, pc, cycles_before, false);
        case MIPS_OP_BNE:
            return compile_branch(e, regs, insn, pc, cycles_before, true);
        case MIPS_OP_BLEZ:
            return MIPS_RT(insn) == 0u ?
                   compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BLEZ, false) : false;
        case MIPS_OP_BGTZ:
            return MIPS_RT(insn) == 0u ?
                   compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BGTZ, false) : false;
        case MIPS_OP_REGIMM:
            if (MIPS_RT(insn) == MIPS_REGIMM_BLTZ) {
                return compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BLTZ, false);
            }
            if (MIPS_RT(insn) == MIPS_REGIMM_BGEZ) {
                return compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BGEZ, false);
            }
            if (MIPS_RT(insn) == MIPS_REGIMM_BLTZAL) {
                return compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BLTZ, true);
            }
            if (MIPS_RT(insn) == MIPS_REGIMM_BGEZAL) {
                return compile_reg_branch(e, regs, insn, pc, cycles_before, JIT_BRANCH_BGEZ, true);
            }
            return false;
        case MIPS_OP_J:
            return compile_jump(e, regs, insn, pc, cycles_before, false);
        case MIPS_OP_JAL:
            return compile_jump(e, regs, insn, pc, cycles_before, true);
        case MIPS_OP_SPECIAL:
            if (MIPS_FUNCT(insn) == MIPS_FUNCT_JR) {
                return mips_jr_encoding_ok(insn) ? compile_jr(e, regs, insn, pc, cycles_before) : false;
            }
            if (MIPS_FUNCT(insn) == MIPS_FUNCT_JALR) {
                return mips_jalr_encoding_ok(insn) ? compile_jalr(e, regs, insn, pc, cycles_before) : false;
            }
            return false;
        default:
            return false;
    }
}

void *jit_compile_block(psxRegisters *regs, uint32_t pc) {
    const size_t literal_bytes = JIT_LITERAL_SLOTS_PER_BLOCK * sizeof(uint32_t);
    const size_t reserve_bytes = JIT_BLOCK_RESERVE_BYTES;
    uint8_t *block_base = (uint8_t *)jit_code_reserve(reserve_bytes, 4u);
    if (block_base == NULL) {
        static unsigned rfail;
        if (++rfail <= 3) printf("JIT: reserve fail #%u\n", rfail);
        return NULL;
    }
    uint8_t *exec_base = jit_code_pending_exec_base(block_base);
    if (exec_base == NULL) {
        jit_code_rollback(block_base);
        return NULL;
    }

    memset(block_base, 0, literal_bytes);
    uint8_t *entry = block_base + literal_bytes;
    JitEmit e;
    jit_emit_init(&e, block_base, entry, block_base + reserve_bytes, exec_base,
                  (uint32_t *)(void *)block_base, JIT_LITERAL_SLOTS_PER_BLOCK);

#if JIT_USE_WINDOWED_ABI
    jit_emit_entry(&e, XT_A1, 32u);
#endif
    emit_load_hot_regs(&e);

    uint32_t cur_pc = pc;
    uint32_t compiled = 0u;
    bool finished = false;

    for (; compiled < JIT_MAX_MIPS_INSNS_PER_BLOCK; ++compiled, cur_pc += 4u) {
        if (compiled > 0u &&
            e.lit_count <= e.lit_capacity &&
            (e.lit_capacity - e.lit_count) <= (uint32_t)JIT_LITERAL_LOW_WATERMARK_SLOTS) {
            emit_exit_imm(&e, cur_pc, compiled, 0u);
            finished = true;
            break;
        }

        uint32_t insn = JIT_FETCH32(regs, cur_pc);
        if (is_control(insn)) {
            JitEmitMark mark = emit_mark(&e);
            if (!compile_control(&e, regs, insn, cur_pc, compiled)) {
                emit_rewind(&e, mark);
                if (compiled == 0u) {
                    { static unsigned c; if (++c <= 5) printf("JIT ctrl_fail op=%02x pc=%08x\n", (unsigned)(insn>>26), (unsigned)cur_pc); }
                    jit_code_rollback(block_base);
                    return NULL;
                }
                emit_exit_imm(&e, cur_pc, compiled, 0u);
            } else {
                compiled += 2u;
                cur_pc += 8u;
            }
            finished = true;
            break;
        }

        JitEmitMark mark = emit_mark(&e);
        if (!compile_one_noncontrol(&e, insn, cur_pc, compiled, true)) {
            emit_rewind(&e, mark);
            if (compiled == 0u) {
                { static unsigned c; if (++c <= 5) printf("JIT op_fail op=%02x func=%02x pc=%08x\n", (unsigned)(insn>>26), (unsigned)(insn&0x3f), (unsigned)cur_pc); }
                jit_code_rollback(block_base);
                return NULL;
            }
            emit_exit_imm(&e, cur_pc, compiled, 0u);
            finished = true;
            break;
        }

#if JIT_END_BLOCK_AFTER_STORE
        if (is_store_mem(insn)) {
            cur_pc += 4u;
            ++compiled;
            emit_exit_imm(&e, cur_pc, compiled, 0u);
            finished = true;
            break;
        }
#endif

        if (e.failed) break;
    }

    if (!finished && !e.failed) {
        emit_exit_imm(&e, cur_pc, compiled, 0u);
    }

    if (e.failed) {
        jit_code_rollback(block_base);
        return NULL;
    }

    size_t used = (size_t)(e.p - block_base);
    uint8_t *committed_base = (uint8_t *)jit_code_commit(block_base, used);
    if (committed_base == NULL) {
        jit_code_rollback(block_base);
        return NULL;
    }
    uint8_t *committed_entry = committed_base + literal_bytes;
    jit_install_block(pc, cur_pc, committed_base, committed_entry, (uint32_t)used, compiled);
    return committed_entry;
}
