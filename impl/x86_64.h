#pragma once
#include <stdint.h>

//
// x86-64 bytecode generation
//

// Registers (assuming System V call convention)
#define REG_RAX 0  // RAX: return value
#define REG_RCX 1  // RCX: 4th argument
#define REG_RDX 2  // RDX: 3rd argument
#define REG_RBX 3  // RBX: callee saved (don't use)
#define REG_RSP 4  // RSP: stack pointer (don't use)
#define REG_RBP 5  // RBP: frame pointer (don't use)
#define REG_RSI 6  // RSI: 2nd argument
#define REG_RDI 7  // RDI: 1st argument
#define REG_R8  8  // R8:  5th argument
#define REG_R9  9  // R9:  6th argument
#define REG_R10 10 // R10: temporary register
#define REG_R11 11 // R11: temporary register
#define REG_R12 12 // R12: callee saved (don't use)
#define REG_R13 13 // R13: callee saved (don't use)
#define REG_R14 14 // R14: callee saved (don't use)
#define REG_R15 15 // R15: callee saved (don't use)

// Constants used in the code, makes it easier to remember what each register is used for

#define REG_FRAME REG_RBP
#define REG_STACK REG_RSP
#define REG_ARGS  REG_RDI
#define REG_RET   REG_RAX
#define REG_TMP1  REG_RSI
#define REG_TMP2  REG_RDX
#define REG_TMP3  REG_RCX

#define OBJ_SIZE (int)sizeof(Object*)

// Opcode prefixes
#define REX_W 0x48
#define REX_R 0x44

#define OP_MOD(byte) (byte << 6)
#define OP_REG(byte) (byte << 3)
#define OP_RM(byte)  (byte)

// The EMIT macro needs a `uint8_t** mem` variable where the pointer to the executable memory is
#define EMIT(byte) **mem = (uint8_t)(byte); (*mem)++;

// Immediate values
#define EMIT_IMM8(imm) EMIT(imm);
#define EMIT_IMM16(imm) EMIT(imm); EMIT((uint16_t)imm >> 8);
#define EMIT_IMM32(imm) EMIT(imm); EMIT((uint32_t)imm >> 8); EMIT((uint32_t)imm >> 16); EMIT((uint32_t)imm >> 24);
#define EMIT_IMM64(imm) EMIT(imm); EMIT((uint64_t)imm >> 8); EMIT((uint64_t)imm >> 16); EMIT((uint64_t)imm >> 24); \
    EMIT((uint64_t)imm >> 32); EMIT((uint64_t)imm >> 40); EMIT((uint64_t)imm >> 48); EMIT((uint64_t)imm >> 56);

// PUSH: a
#define EMIT_PUSH(a) EMIT(0x50 + a)

// POP: a
#define EMIT_POP(a) EMIT(0x58 + a)

// MOV: a = b (copy)
#define EMIT_MOV64_REG_REG(a, b) EMIT(REX_W); EMIT(0x89); EMIT(0xc0 | OP_RM(a) | OP_REG(b))

// MOV: *a = b (store)
#define EMIT_MOV64_PTR_REG(a, b) EMIT(REX_W); EMIT(0x89); EMIT(OP_REG(b) | OP_RM(a))

// MOV: a[off] = b (store)
#define EMIT_MOV64_OFF8_REG(a, b, off) EMIT(REX_W); EMIT(0x89); EMIT(0x40 | OP_REG(b) | OP_RM(a)); EMIT(off);

// MOV: a = *b (load)
#define EMIT_MOV64_REG_PTR(a, b) EMIT(REX_W); EMIT(0x8b); EMIT(OP_REG(a) | OP_RM(b))

// MOV: a = b[off] (load)
#define EMIT_MOV64_REG_OFF8(a, b, off) EMIT(REX_W); EMIT(0x8b); EMIT(0x40 | OP_REG(a) | OP_RM(b)); EMIT(off);

// MOV: a = imm32
#define EMIT_MOV64_REG_IMM32(a, imm) EMIT(0xb8 + a); EMIT_IMM32(imm);

// MOV: a = imm64
#define EMIT_MOV64_REG_IMM64(a, imm) EMIT(REX_W); EMIT(0xb8 + a); EMIT_IMM64(imm);

// MOV: *a = imm32 (sign-extended to imm64)
#define EMIT_MOV64_PTR_IMM32(a, imm) EMIT(REX_W); EMIT(0xc7); EMIT(OP_RM(a)); EMIT_IMM32(imm);

// ADD: a += b
#define EMIT_ADD64_REG_REG(a, b) EMIT(REX_W); EMIT(0x01); EMIT(0xc0 | OP_REG(b) | OP_RM(a));

// ADD: *a += b
#define EMIT_ADD64_PTR_REG(a, b) EMIT(REX_W); EMIT(0x01); EMIT(OP_REG(b) | OP_RM(a));

// ADD: a[off] += b
#define EMIT_ADD64_OFF8_REG(a, b, off) EMIT(REX_W); EMIT(0x01); EMIT(0x40 |  OP_REG(b) | OP_RM(a)); EMIT(off);

// ADD: a += b[off]
#define EMIT_ADD64_REG_OFF8(a, b, off) EMIT(REX_W); EMIT(0x03); EMIT(0x40 |  OP_REG(a) | OP_RM(b)); EMIT(off);

// ADD: a += imm8
#define EMIT_ADD64_IMM8(a, i) EMIT(REX_W); EMIT(0x83); EMIT(0xc0 | OP_RM(a)); EMIT_IMM8(i);

// ADD: a += imm32
#define EMIT_ADD64_IMM32(a, i) EMIT(REX_W); EMIT(0x81); EMIT(0xc0 | OP_RM(a)); EMIT_IMM32(i);

// SUB: a -= b
#define EMIT_SUB64_REG_REG(a, b) EMIT(REX_W); EMIT(0x29); EMIT(0xc0 | OP_REG(b) | OP_RM(a));

// SUB: *a -= b
#define EMIT_SUB64_PTR_REG(a, b) EMIT(REX_W); EMIT(0x29); EMIT(OP_REG(b) | OP_RM(a));

// SUB: a[off] -= b
#define EMIT_SUB64_OFF8_REG(a, b, off) EMIT(REX_W); EMIT(0x29); EMIT(0x40 | OP_REG(b) | OP_RM(a)); EMIT(off);

// SUB: a -= b[off]
#define EMIT_SUB64_REG_OFF8(a, b, off) EMIT(REX_W); EMIT(0x2b); EMIT(0x40 | OP_REG(a) | OP_RM(b)); EMIT(off);

// SUB: a -= imm8
#define EMIT_SUB64_IMM8(a, i) EMIT(REX_W); EMIT(0x83); EMIT(0xc0 | OP_REG(0x5) | OP_RM(a)); EMIT_IMM8(i);

// SUB: a -= imm32
#define EMIT_SUB64_IMM32(a, i) EMIT(REX_W); EMIT(0x81); EMIT(0xc0 | OP_REG(0x5) | OP_RM(a)); EMIT_IMM32(i);

// NEG: a = -a
#define EMIT_NEG64(a) EMIT(REX_W); EMIT(0xf7); EMIT(0xc0 | OP_REG(0x3) | OP_RM(a));

// SAR: a >>= imm8
#define EMIT_SAR64_IMM8(a, i) EMIT(REX_W); EMIT(0xc1); EMIT(0xc0 | OP_REG(0x7)| OP_RM(a)); EMIT_IMM8(i);

// SAL: a <<= imm8
#define EMIT_SAL64_IMM8(a, i) EMIT(REX_W); EMIT(0xc1); EMIT(0xc0 | OP_REG(0x4) | OP_RM(a)); EMIT_IMM8(i);

// CMP: a - b
#define EMIT_CMP64_REG_REG(a, b) EMIT(REX_W); EMIT(0x3b); EMIT(0xc0 | OP_REG(a) | OP_RM(b));

// CMP: a - *b
#define EMIT_CMP64_REG_PTR(a, b) EMIT(REX_W); EMIT(0x3b); EMIT(OP_REG(a) | OP_RM(b));

// CMP: a - b[off]
#define EMIT_CMP64_REG_OFF8(a, b, off) EMIT(REX_W); EMIT(0x3b); EMIT(0x40 | OP_REG(a) | OP_RM(b)); EMIT_IMM8(off);

// CMP: a - imm8
#define EMIT_CMP64_REG_IMM8(a, i) EMIT(REX_W); EMIT(0x83); EMIT(0xc0 | OP_REG(0x7) | OP_RM(a)); EMIT_IMM8(i);

// CMP: a - imm32
#define EMIT_CMP64_IMM32(a, i) EMIT(REX_W); EMIT(0x81); EMIT(0xc0 | OP_REG(0x7) | OP_RM(a)); EMIT_IMM32(i);

// JMP: unconditional jump, imm8 offset (Stores a placeholder that's filled in later)
#define EMIT_JMP_OFF8() EMIT(0xeb); EMIT_IMM8(0x0);

// JMP: unconditional jump, imm32 offset (Stores a placeholder that's filled in later)
#define EMIT_JMP_OFF32() EMIT(0xe9); EMIT_IMM32(0x0);

// JMP: unconditional jump, no placeholder
#define EMIT_JMP_OFF32_NO_PLACEHOLDER() EMIT(0xe9);

// JE: a - b == 0, imm8 offset (Stores a placeholder that's filled in later)
#define EMIT_JE_OFF8() EMIT(0x74); EMIT(0x0);

// JE: a - b == 0, imm32 offset (Stores a placeholder that's filled in later)
#define EMIT_JE_OFF32() EMIT(0x0f); EMIT(0x84); EMIT_IMM32(0)

// JL: a - b < 0 (Stores a placeholder that's filled in later)
#define EMIT_JL_OFF8() EMIT(0x7c); EMIT(0x0);

// JL: a - b < 0, imm32 offset (Stores a placeholder that's filled in later)
#define EMIT_JL_OFF32() EMIT(0x0f); EMIT(0x8c); EMIT_IMM32(0)

// Patches the jump point
#define PATCH_JMP8(ptr, off) ptr[-1] = off;

// Patches the jump point
void PATCH_JMP32(uint8_t* ptr, uint32_t off);

// CALL, address is stored in register
#define EMIT_CALL_REG(a) EMIT(0xff); EMIT(0xc0 | OP_REG(0x2) | OP_RM(a));

// RET
#define EMIT_RET() EMIT(0xc3);

#define EMIT_PROLOGUE() EMIT_PUSH(REG_RBP); EMIT_MOV64_REG_REG(REG_RBP, REG_RSP)

#define EMIT_EPILOGUE() EMIT_POP(REG_RBP)

#define RESERVE_STACK(size) EMIT_PROLOGUE(); if (size < 128) {EMIT_SUB64_IMM8(REG_STACK, size);} else {EMIT_SUB64_IMM32(REG_STACK, size);}
#define FREE_STACK(size) if (size < 128) {EMIT_ADD64_IMM8(REG_STACK, size);} else {EMIT_ADD64_IMM32(REG_STACK, size);} EMIT_EPILOGUE();
