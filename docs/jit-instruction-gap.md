# JIT Compiler — Instruction Reference for GPT

## Current state: 12 of ~50 MIPS instructions supported. Zero blocks compile.

The file `jit_compile.c` contains `compile_one_noncontrol()` at line 975 which is the instruction dispatch. Currently supports only:

### ✅ Supported
| Opcode | Mnemonic | Type |
|--------|----------|------|
| 0x00/0x21 | `addu` | R-type (rs+rt→rd) |
| 0x00/0x23 | `subu` | R-type (rs-rt→rd) |
| 0x00/0x24 | `and` | R-type (rs&rt→rd) |
| 0x00/0x25 | `or` | R-type |
| 0x00/0x26 | `xor` | R-type |
| 0x00/0x00 | `sll` | R-type (rt<<sa→rd) |
| 0x09 | `addiu` | I-type (rs+imm→rt) |
| 0x0c | `andi` | I-type (rs&imm→rt) |
| 0x0d | `ori` | I-type |
| 0x0e | `xori` | I-type |
| 0x0f | `lui` | I-type (imm<<16→rt) |
| 0x23 | `lw` | I-type (mem[rs+imm]→rt) |
| 0x2b | `sw` | I-type (rt→mem[rs+imm]) |

### Implementation pattern (use existing code as template)
```
case MIPS_OP_XXX:
    return mips_rtype_sa_zero(insn) ? compile_xxx(e, insn) : false;
```
For I-type: `return compile_xxx(e, insn);`

### ❌ Must implement (priority order)

#### CRITICAL — No blocks compile without these:

| Opcode | Mnemonic | Notes |
|--------|----------|-------|
| 0x00/0x2a | `slt` | R-type (rs<rt→rd) |
| 0x00/0x2b | `sltu` | R-type unsigned |
| 0x0a | `slti` | I-type (rs<imm→rt) |
| 0x0b | `sltiu` | I-type unsigned |

#### CRITICAL — Control flow:

| Opcode | Mnemonic | Notes |
|--------|----------|-------|
| 0x04 | `beq` | Branch if rs==rt |
| 0x05 | `bne` | Branch if rs!=rt |
| 0x06 | `blez` | Branch if rs≤0 |
| 0x07 | `bgtz` | Branch if rs>0 |
| 0x01/0x00 | `bltz` | REGIMM: Branch if rs<0 |
| 0x01/0x01 | `bgez` | REGIMM: Branch if rs≥0 |
| 0x02 | `j` | Jump |
| 0x03 | `jal` | Jump and link |
| 0x00/0x08 | `jr` | Jump register |
| 0x00/0x09 | `jalr` | Jump and link register |

#### HIGH — Memory access:

| Opcode | Mnemonic | Notes |
|--------|----------|-------|
| 0x20 | `lb` | Load byte signed |
| 0x24 | `lbu` | Load byte unsigned |
| 0x21 | `lh` | Load halfword signed |
| 0x25 | `lhu` | Load halfword unsigned |
| 0x28 | `sb` | Store byte |
| 0x29 | `sh` | Store halfword |
| 0x22 | `lwl` | Load word left |
| 0x26 | `lwr` | Load word right |
| 0x2a | `swl` | Store word left |
| 0x2e | `swr` | Store word right |

#### MEDIUM — Arithmetic:

| Opcode | Mnemonic | Notes |
|--------|----------|-------|
| 0x00/0x27 | `nor` | R-type |
| 0x00/0x02 | `srl` | Shift right logical |
| 0x00/0x03 | `sra` | Shift right arithmetic |
| 0x00/0x04 | `sllv` | Shift left logical variable |
| 0x00/0x06 | `srlv` | Shift right logical variable |
| 0x00/0x07 | `srav` | Shift right arithmetic variable |
| 0x00/0x18 | `mult` | Multiply (HI,LO) |
| 0x00/0x19 | `multu` | Multiply unsigned |
| 0x00/0x1a | `div` | Divide (HI,LO) |
| 0x00/0x1b | `divu` | Divide unsigned |
| 0x00/0x10 | `mfhi` | Move from HI |
| 0x00/0x12 | `mflo` | Move from LO |
| 0x00/0x11 | `mthi` | Move to HI |
| 0x00/0x13 | `mtlo` | Move to LO |

## Xtensa Emitter API (jit_emit.h)

```c
// Allocate a scratch Xtensa register
unsigned jit_emit_alloc_reg(JitEmit *e);  
void   jit_emit_free_reg(JitEmit *e, unsigned ar);
void   jit_emit_free_all(JitEmit *e);

// Emit Xtensa instructions (24-bit)
void jit_emit_addi(JitEmit *e, unsigned ar, unsigned as, int imm);     // ar = as + imm
void jit_emit_add(JitEmit *e, unsigned ar, unsigned as, unsigned at);  // ar = as + at  (narrow)
void jit_emit_sub(JitEmit *e, unsigned ar, unsigned as, unsigned at);  // ar = as - at
void jit_emit_and(JitEmit *e, unsigned ar, unsigned as, unsigned at);  // ar = as & at  (narrow)
void jit_emit_or(JitEmit *e, unsigned ar, unsigned as, unsigned at);   // ar = as | at  (narrow)
void jit_emit_xor(JitEmit *e, unsigned ar, unsigned as, unsigned at);  // ar = as ^ at
void jit_emit_slli(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as << sa
void jit_emit_srli(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as >> sa (logical)
void jit_emit_srai(JitEmit *e, unsigned ar, unsigned as, unsigned sa); // ar = as >> sa (arith)
void jit_emit_movi(JitEmit *e, unsigned ar, int imm);                 // ar = imm (narrow, -32..95)
void jit_emit_movi32(JitEmit *e, unsigned ar, uint32_t imm);           // ar = imm (full 32-bit)
void jit_emit_l32i(JitEmit *e, unsigned ar, unsigned as, int imm8);    // ar = *(as + imm8*4)
void jit_emit_s32i(JitEmit *e, unsigned ar, unsigned as, int imm8);    // *(as + imm8*4) = ar
void jit_emit_l8ui(JitEmit *e, unsigned ar, unsigned as, int imm8);    // ar = *(uint8_t*)(as + imm8)
void jit_emit_s8i(JitEmit *e, unsigned ar, unsigned as, int imm8);     // *(uint8_t*)(as + imm8) = ar
void jit_emit_l16ui(JitEmit *e, unsigned ar, unsigned as, int imm8);   // ar = *(uint16_t*)(as + imm8*2)
void jit_emit_s16i(JitEmit *e, unsigned ar, unsigned as, int imm8);    // *(uint16_t*)(as + imm8*2) = ar
void jit_emit_beqz(JitEmit *e, unsigned ar, JitLabel label);           // if ar==0 goto label (narrow)
void jit_emit_bnez(JitEmit *e, unsigned ar, JitLabel label);           // if ar!=0 goto label (narrow)
void jit_emit_beq(JitEmit *e, unsigned ar, unsigned as, JitLabel l);   // if ar==as goto label
void jit_emit_bne(JitEmit *e, unsigned ar, unsigned as, JitLabel l);   // if ar!=as goto label
void jit_emit_bge(JitEmit *e, unsigned ar, unsigned as, JitLabel l);   // if ar>=as goto label
void jit_emit_blt(JitEmit *e, unsigned ar, unsigned as, JitLabel l);   // if ar<as goto label
void jit_emit_j(JitEmit *e, JitLabel label);                           // jump to label

// Labels
JitLabel jit_emit_label(JitEmit *e);           // Create unresolved label
void jit_emit_bind(JitEmit *e, JitLabel l);    // Bind label to current position
void jit_emit_patch(JitEmit *e, JitLabel l);   // Patch jumps to this label

// MIPS register helpers
unsigned jit_mips_rd(uint32_t insn);  // Extract rd field
unsigned jit_mips_rs(uint32_t insn);  // Extract rs field
unsigned jit_mips_rt(uint32_t insn);  // Extract rt field
int16_t  jit_mips_imm(uint32_t insn); // Extract signed immediate

// MIPS<->Xtensa register mapping
unsigned jit_map_gpr_to_ar(unsigned mips_reg);  // MIPS GPR → Xtensa AR
void jit_load_gpr(JitEmit *e, unsigned ar, unsigned mips_reg);  // Load MIPS reg into Xtensa
void jit_store_gpr(JitEmit *e, unsigned ar, unsigned mips_reg); // Store Xtensa to MIPS reg
void jit_get_memaddr(JitEmit *e, unsigned addr_ar, unsigned base_ar, int offset); // Compute PSX RAM address

// Existing examples to follow
static bool compile_addu(JitEmit *e, uint32_t insn);  // See jit_compile.c:XXX
static bool compile_addiu(JitEmit *e, uint32_t insn); // Simple: 3-register op
static bool compile_lw(JitEmit *e, uint32_t insn, ...); // Memory: needs memaddr helper
static bool compile_ori(JitEmit *e, uint32_t insn);   // Immediate: movi or lui+ori combo
```

## How to add a new instruction

1. Add a `case MIPS_OP_XXX` in `compile_one_noncontrol()` (line 980)
2. Create a `static bool compile_xxx(JitEmit *e, uint32_t insn)` function
3. Pattern: extract MIPS fields → map to Xtensa regs → emit Xtensa instrs → store results
4. Return `true` on success, `false` if instruction can't be handled

For control flow (branches/jumps), add to `compile_control()` which is called separately.

## Files in zip
- `jit_compile.c` — main compiler (add cases to `compile_one_noncontrol` line 975)
- `jit_emit.h` — Xtensa emitter API (all `jit_emit_*` functions)
- `jit_xtensa.h` — MIPS reg helpers, config, JIT_FETCH32 definition
- `jit_xtensa_config_esp32s3.h` — ESP32-S3 specific settings
