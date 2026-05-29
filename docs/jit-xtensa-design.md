# Custom MIPS→Xtensa JIT for ESP32-S3 PSX Emulator

## Architecture

The interpreter bottleneck is PSRAM latency. Every MIPS instruction fetch hits the 120MHz SPI PSRAM bus (~5-50 cycles). A JIT compiles MIPS blocks to native Xtensa code in fast IRAM (1 cycle access), eliminating repeated fetches.

## Xtensa LX7 Registers

| Type | Count | Notes |
|------|-------|-------|
| AR (general) | 16 (a0-a15) | a0=return addr, a1=stack ptr |
| AR (windowed) | 64 physical, 16 visible | Register windows for calls |
| SAR (shift) | 1 | Shift amount register |
| MISC | 4 | Misc special registers |

## MIPS→Xtensa Register Mapping

MIPS has 32 GPRs + HI/LO/PC. Xtensa has 16 visible ARs. Static assignment:

| MIPS Reg | Xtensa Reg | Strategy |
|----------|------------|----------|
| r0 (zero) | — | Hardcoded 0, never read |
| r1 (at) | a4 | Hot (assembler temp) |
| r2 (v0) | a5 | Hot (return value) |
| r3 (v1) | mem[regs+GPR+12] | Cold |
| r4 (a0) | a6 | Hot (arg0) |
| r5 (a1) | a7 | Hot (arg1) |
| r6-r7 (a2-a3) | mem[regs+GPR+24] | Cold |
| r8-r15 (t0-t7) | mem[regs+GPR+32] | Cold (temps) |
| r16-r23 (s0-s7) | a8-a15 | Hot (saved regs) |
| r24-r25 (t8-t9) | mem[regs+GPR+96] | Cold |
| r26-r27 (k0-k1) | mem[regs+GPR+104] | Cold (kernel) |
| r28 (gp) | mem[regs+GPR+112] | Cold |
| r29 (sp) | mem[regs+GPR+116] | Cold (seldom changes) |
| r30 (fp) | mem[regs+GPR+120] | Cold |
| r31 (ra) | mem[regs+GPR+124] | Cold |
| HI | mem[regs+GPR+128] | |
| LO | mem[regs+GPR+132] | |
| PC | mem[regs+pc_offset] | Updated at block end |

## PSX RAM Access

PSX RAM is at `psxRegs.ptrs.psxM` (PSRAM address). MemRLUT accelerates lookups.

```c
// In JIT code (pseudocode):
// Load MIPS reg at a6(r4) from PSX RAM
movi  a10, psx_memrlut_base    // LUT base address
srli  a11, a6, 16              // a11 = r4 >> 16 (page index)
addx4 a11, a11, a10            // LUT[page]
l32i  a11, a11, 0              // page base address
add   a11, a11, a6             // + offset
l32i  a5, a11, 0               // load value → v0
```

## Instruction Translation Examples

Xtensa LX7 has 16-bit (narrow) and 24-bit encodings. Narrow ops save space.

```
; MIPS:  addu r2, r4, r5        (r2 = r4 + r5)
; Xtensa: add.n a5, a6, a7      (16-bit narrow add)

; MIPS:  addiu r4, r4, 0x10     (r4 += 16)
; Xtensa: addi a6, a6, 0x10     (24-bit immediate)

; MIPS:  lw r2, 0(r4)           (load word from addr in r4)
; Xtensa: [see PSX RAM Access above]

; MIPS:  sw r2, 0(r4)           (store word to addr in r4)
; Xtensa: [reverse of load — compute address, write via s32i]

; MIPS:  beq r2, r0, target     (branch if r2 == 0)
; Xtensa: beqz.n a5, target     (16-bit branch if zero)

; MIPS:  bne r2, r0, target     (branch if r2 != 0)
; Xtensa: bnez.n a5, target     (16-bit branch if not zero)

; MIPS:  j target
; Xtensa: j target              (24-bit jump)

; MIPS:  jal target              (jump and link)
; Xtensa: [emit: store ra, then j target]

; MIPS:  jr r31                  (jump to return address)
; Xtensa: [emit: load ra from mem, then jx aX]

; MIPS:  ori r2, r0, 0x1234     (r2 = 0x1234)
; Xtensa: movi.n a5, 0x1234     (16-bit narrow, limited range)
; or:     movi a5, 0x1234       (24-bit, full range)

; MIPS:  lui r2, 0x1234         (r2 = 0x12340000)
; Xtensa: movi a5, 0x12340000   (24-bit)

; MIPS:  slt r2, r4, r5         (r2 = (r4 < r5) ? 1 : 0)
; Xtensa: [compare + conditional move]
```

## JIT Block Structure

```
Block entry:
    ; Store link to regs structure in a3 (passed in)
    ; Spill any hot regs that are dirty

Instruction loop:
    [translated MIPS instructions...]

Block exit (branch/jump/syscall):
    ; Update psxRegs.pc
    ; Update psxRegs.cycle
    ; Check psxRegs.stop (if set, return to interpreter)
    ; Check interrupt flag
    ; Flush dirty hot regs to memory
    ; Return to dispatch loop (or tail-call next block)
```

## Integration with PCSX

Replace `psxInt` with `psxJit` implementing `R3000Acpu` (from `r3000a.h:65-75`):

```c
R3000Acpu psxJit = {
    .Init        = jit_init,         // Allocate code buffer (heap_caps_malloc EXEC)
    .Reset       = jit_reset,        // Invalidate all blocks
    .Execute     = jit_execute,      // Main entry: runs JIT blocks in loop
    .ExecuteBlock= jit_execute_block,// Execute up to next branch
    .Clear       = jit_clear,        // Invalidate blocks containing address
    .Notify      = jit_notify,       // Before save: flush regs to psxRegs
    .ApplyConfig = jit_apply_config, // Update timing
    .Shutdown    = jit_shutdown,     // Free code buffer
};
```

### Execute loop

```c
void jit_execute(psxRegisters *regs) {
    while (!regs->stop) {
        // Flush psxRegs → local (sync from memory if not dirty)
        sync_cold_regs_from_psxregs(regs);
        
        void *block = find_block(regs->pc);
        if (!block) {
            block = compile_block(regs);
            if (!block) {
                // Fall back to interpreter for this instruction
                execI(regs);
                continue;
            }
        }
        
        // Call native code block
        // Convention: a2 = &psxRegs, a3 = memRLUT pointer
        block();
        
        // On return: dirty hot regs written to psxRegs
    }
    
    // Sync all regs back to psxRegs
    sync_all_regs_to_psxregs(regs);
}
```

### Code buffer

Use `heap_caps_malloc(CODE_BUFFER_SIZE, MALLOC_CAP_EXEC)` for executable memory.
Start with 256KB. If full, invalidate LRU blocks (or clear all on overflow).

### Block cache

Simple hash table mapping `psxRegs.pc → native_code_ptr`.

Invalidate blocks when PSX RAM is written at that address. Hook into `psxMemWrite` or `psxhw.c` write handlers.

## Files to Create

| File | Lines | Purpose |
|------|-------|---------|
| `jit_xtensa.c` | ~800 | Init, dispatch loop, block cache, fallback |
| `jit_compile.c` | ~1000 | MIPS→Xtensa translator, one function per opcode |
| `jit_emit.h` | ~300 | Xtensa instruction encodings (macros to emit bytes) |

## Instruction Encoding Reference

Xtensa instructions are 16-bit (narrow) or 24-bit (wide). Little-endian byte order.

Format (24-bit):
```
Byte 0: opcode[7:0]
Byte 1: opcode[15:8] | (operand_bits << (places))
Byte 2: opcode[23:16]
```

Key opcodes:
- `l32i.n ar, as, imm4` → load 32-bit word (narrow)
- `s32i.n ar, as, imm4` → store 32-bit word (narrow)
- `add.n ar, as, at` → add (narrow)
- `beqz.n ar, offset` → branch if zero (narrow)
- `bnez.n ar, offset` → branch if not zero (narrow)
- `movi.n ar, imm` → move immediate (narrow, limited range)
- `ret.n` → return (narrow)
- `j offset` → jump (wide)
- `call0 offset` → call (wide)
- `addi ar, as, imm` → add immediate (wide)
- `l32i ar, as, imm8` → load word (wide)
- `s32i ar, as, imm8` → store word (wide)

See ESP-IDF: `tools/xtensa-esp-elf/xtensa-esp-elf/include/xtensa/config/core.h`
and the Xtensa ISA Reference Manual for full encoding.

## Minimal First Version

Translate ONLY the 10 most common instructions (~80% coverage):

| MIPS | Xtensa | Complexity |
|------|--------|------------|
| `lw` | Compute addr + l32i | Medium (PSRAM access) |
| `sw` | Compute addr + s32i | Medium |
| `addiu` | addi | Easy (1:1) |
| `addu` | add.n | Easy (1:1) |
| `beq` | beqz.n | Easy (reg→reg check) |
| `bne` | bnez.n | Easy |
| `ori` | movi.n | Easy |
| `lui` | movi | Easy (wide imm) |
| `jal` | call0 | Medium (save ra) |
| `jr` | jx + ret.n | Medium |

Fall through to interpreter for all other instructions and edge cases.
This version would be ~500 lines of code.

## Self-Modifying Code

PSX games sometimes write to code memory (e.g., decompressors, runtime patches).
Hook `psxMemWrite` to check if the written address has a cached JIT block.
If so, invalidate that block — next execution will recompile from the new MIPS code.
