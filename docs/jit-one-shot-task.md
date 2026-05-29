# JIT Compiler — One-Shot Instruction Gap Task

## Goal

Fill `compile_one_noncontrol()` in `jit_compile.c` with the missing MIPS instruction handlers so blocks actually compile to native Xtensa code. Currently **zero blocks compile** because the first instruction of every MIPS block is unsupported.

## What Works

- JIT framework: init, block cache, execute loop, interpreter fallback — all solid
- Code buffer: allocated from executable IRAM via `heap_caps_aligned_alloc(MALLOC_CAP_EXEC)`
- Preflight: automatic `intFetch` repair if interpreter state is corrupted
- 12 instructions already compile successfully (see below)

## What's Broken

Every call to `jit_compile_block()` returns NULL because the first instruction of the block is unsupported. The failure happens at `jit_compile.c:1908`:

```c
if (!compile_one_noncontrol(&e, insn, cur_pc, compiled, true)) {
    emit_rewind(&e, mark);
    if (compiled == 0u) {
        jit_code_rollback(block_base);
        return NULL;  // <<< BLOCK FAILS, falls back to interpreter
    }
}
```

## The Fix

Add `case` entries to `compile_one_noncontrol()` at **line 980** of `jit_compile.c`. Each new entry follows one of two patterns:

### Pattern A: Simple 3-register operation (R-type, opcode 0x00)

```c
case MIPS_FUNCT_XXX:
    return mips_rtype_sa_zero(insn) ? compile_xxx(e, insn) : false;
```

Then implement the handler:
```c
static bool compile_xxx(JitEmit *e, uint32_t insn) {
    unsigned rd = MIPS_RD(insn), rs = MIPS_RS(insn), rt = MIPS_RT(insn);
    if (rd == 0) return true;  // MIPS r0 is always 0
    
    unsigned rd_ar = jit_map_gpr_to_ar(rd);
    unsigned rs_ar = jit_map_gpr_to_ar(rs);
    unsigned rt_ar = jit_map_gpr_to_ar(rt);
    
    jit_load_gpr(e, rs_ar, rs);
    jit_load_gpr(e, rt_ar, rt);
    jit_emit_xxx(e, rd_ar, rs_ar, rt_ar);  // the actual Xtensa instruction
    jit_store_gpr(e, rd_ar, rd);
    return true;
}
```

### Pattern B: Immediate operation (I-type)

```c
case MIPS_OP_XXX:
    return compile_xxx(e, insn);
```

Handler:
```c
static bool compile_xxx(JitEmit *e, uint32_t insn) {
    unsigned rt = MIPS_RT(insn), rs = MIPS_RS(insn);
    int16_t imm = (int16_t)MIPS_IMM(insn);
    if (rt == 0) return true;
    
    unsigned rt_ar = jit_map_gpr_to_ar(rt);
    unsigned rs_ar = jit_map_gpr_to_ar(rs);
    
    jit_load_gpr(e, rs_ar, rs);
    jit_emit_addi(e, rt_ar, rs_ar, (int)imm);
    jit_store_gpr(e, rt_ar, rt);
    return true;
}
```

## Already Implemented (12 instructions — use as templates)

Look at these working handlers in `jit_compile.c` for exact patterns:

| Instruction | Handler function | Type |
|-------------|-----------------|------|
| `addu` | `compile_addu()` | R-type, 3-reg |
| `subu` | `compile_subu()` | R-type, 3-reg |
| `and` | `compile_and_reg()` | R-type, 3-reg |
| `or` | `compile_or_reg()` | R-type, 3-reg |
| `xor` | `compile_xor_reg()` | R-type, 3-reg |
| `sll` | `compile_sll_imm()` | R-type, shift |
| `addiu` | `compile_addiu()` | I-type, imm |
| `andi` | `compile_andi()` | I-type, imm |
| `ori` | `compile_ori()` | I-type, imm |
| `xori` | `compile_xori()` | I-type, imm |
| `lui` | `compile_lui()` | I-type, upper imm |
| `lw` | `compile_lw()` | I-type, memory load |
| `sw` | `compile_sw()` | I-type, memory store |

## Must Implement — Priority Order

### CRITICAL (no blocks compile without these)

**R-type (opcode 0x00) — add to `case MIPS_OP_SPECIAL` switch:**

| Funct | Mnemonic | Description |
|-------|----------|-------------|
| 0x2a | `slt` | Set if less than (signed) |
| 0x2b | `sltu` | Set if less than (unsigned) |
| 0x02 | `srl` | Shift right logical |
| 0x03 | `sra` | Shift right arithmetic |
| 0x27 | `nor` | Bitwise NOR |

**I-type — add to main opcode switch:**

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| 0x0a | `slti` | Set if less than immediate (signed) |
| 0x0b | `sltiu` | Set if less than immediate (unsigned) |
| 0x20 | `lb` | Load byte (signed) |
| 0x24 | `lbu` | Load byte unsigned |
| 0x21 | `lh` | Load halfword (signed) |
| 0x25 | `lhu` | Load halfword unsigned |
| 0x28 | `sb` | Store byte |
| 0x29 | `sh` | Store halfword |

### HIGH — Needed for most game code

**R-type (opcode 0x00):**

| Funct | Mnemonic | Description |
|-------|----------|-------------|
| 0x00 | `sll` | Shift left logical (already done) |
| 0x04 | `sllv` | Shift left logical variable |
| 0x06 | `srlv` | Shift right logical variable |
| 0x07 | `srav` | Shift right arithmetic variable |
| 0x18 | `mult` | Multiply (→ HI,LO) |
| 0x19 | `multu` | Multiply unsigned |
| 0x1a | `div` | Divide (→ HI,LO) |
| 0x1b | `divu` | Divide unsigned |
| 0x10 | `mfhi` | Move from HI |
| 0x12 | `mflo` | Move from LO |
| 0x11 | `mthi` | Move to HI |
| 0x13 | `mtlo` | Move to LO |

**Control flow — add to `compile_control()`:**

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| 0x04 | `beq` | Branch if equal |
| 0x05 | `bne` | Branch if not equal |
| 0x06 | `blez` | Branch if ≤ 0 |
| 0x07 | `bgtz` | Branch if > 0 |
| 0x02 | `j` | Jump |
| 0x03 | `jal` | Jump and link |
| 0x00/0x08 | `jr` | Jump register |
| 0x00/0x09 | `jalr` | Jump and link register |

## Xtensa Emitter API (`jit_emit.h`)

```c
// Register allocation
unsigned jit_map_gpr_to_ar(unsigned mips_reg);  // MIPS GPR → Xtensa AR
void jit_load_gpr(JitEmit *e, unsigned ar, unsigned mips_reg);  // MIPS → Xtensa reg
void jit_store_gpr(JitEmit *e, unsigned ar, unsigned mips_reg); // Xtensa → MIPS reg
unsigned jit_emit_alloc_reg(JitEmit *e);   // Get scratch Xtensa reg
void jit_emit_free_reg(JitEmit *e, unsigned ar);    // Release scratch reg

// Arithmetic (24-bit)
void jit_emit_addi(JitEmit *e, unsigned ar, unsigned as, int imm);  // ar = as + imm
void jit_emit_add(JitEmit *e, unsigned ar, unsigned as, unsigned at); // ar = as + at (narrow)
void jit_emit_sub(JitEmit *e, unsigned ar, unsigned as, unsigned at); // ar = as - at
void jit_emit_and(JitEmit *e, unsigned ar, unsigned as, unsigned at); // ar = as & at (narrow)
void jit_emit_or(JitEmit *e, unsigned ar, unsigned as, unsigned at);   // ar = as | at (narrow)
void jit_emit_xor(JitEmit *e, unsigned ar, unsigned as, unsigned at);  // ar = as ^ at
void jit_emit_slli(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as << sa
void jit_emit_srli(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as >> sa (logical)
void jit_emit_srai(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as >> sa (arith)

// Immediate loads
void jit_emit_movi(JitEmit *e, unsigned ar, int imm);     // ar = imm (-32..95)
void jit_emit_movi32(JitEmit *e, unsigned ar, uint32_t imm); // ar = imm (full 32-bit)

// Memory access
void jit_emit_l32i(JitEmit *e, unsigned ar, unsigned as, int imm8); // ar = *(as + imm8*4)
void jit_emit_s32i(JitEmit *e, unsigned ar, unsigned as, int imm8); // *(as + imm8*4) = ar
void jit_emit_l8ui(JitEmit *e, unsigned ar, unsigned as, int imm8); // ar = *(uint8_t*)(as + imm8)
void jit_emit_s8i(JitEmit *e, unsigned ar, unsigned as, int imm8);  // *(uint8_t*)(as + imm8) = ar
void jit_emit_l16ui(JitEmit *e, unsigned ar, unsigned as, int imm8); // ar = *(uint16_t*)(as + imm8*2)
void jit_emit_s16i(JitEmit *e, unsigned ar, unsigned as, int imm8);  // *(uint16_t*)(as + imm8*2) = ar

// Branches
void jit_emit_beqz(JitEmit *e, unsigned ar, JitLabel label); // if ar==0 goto label
void jit_emit_bnez(JitEmit *e, unsigned ar, JitLabel label); // if ar!=0 goto label
void jit_emit_beq(JitEmit *e, unsigned ar, unsigned as, JitLabel l); // if ar==as goto l
void jit_emit_bne(JitEmit *e, unsigned ar, unsigned as, JitLabel l); // if ar!=as goto l
void jit_emit_bge(JitEmit *e, unsigned ar, unsigned as, JitLabel l); // if ar>=as goto l
void jit_emit_blt(JitEmit *e, unsigned ar, unsigned as, JitLabel l); // if ar<as goto l
void jit_emit_j(JitEmit *e, JitLabel label);  // unconditional jump

// Labels
JitLabel jit_emit_label(JitEmit *e);           // Create unresolved label
void jit_emit_bind(JitEmit *e, JitLabel l);    // Bind label to current position
void jit_emit_patch(JitEmit *e, JitLabel l);   // Patch jumps to this label

// PSX memory address computation
void jit_get_memaddr(JitEmit *e, unsigned addr_ar, unsigned base_ar, int offset);
```

## MIPS Field Extractors

```c
#define MIPS_RD(insn)  (((insn) >> 11) & 0x1f)
#define MIPS_RS(insn)  (((insn) >> 21) & 0x1f)
#define MIPS_RT(insn)  (((insn) >> 16) & 0x1f)
#define MIPS_SA(insn)  (((insn) >>  6) & 0x1f)
#define MIPS_IMM(insn) ((insn) & 0xffff)
#define MIPS_FUNCT(insn) ((insn) & 0x3f)
```

## Files to modify

**ONLY `jit_compile.c`** — lines 975-1015 (the `compile_one_noncontrol` switch). Add new `case` entries and their handler functions. Everything else in the framework is ready.

## Validation

After adding instructions, rebuild and flash. Check the serial log for:
```
JIT60: native=X fallback=Y ok=Z fail=W
```

If `ok > 0`, blocks are compiling. If `native > 0`, native code is executing. The goal is `native > 0` — any native execution at all proves the pipeline works.
