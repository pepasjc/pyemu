#include "gameboy_internal.h"

/* Top-level DMG system glue. This file wires subsystem modules into the emulator vtable and keeps the block-cache driven execution loop in one place. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pyemu_gameboy_step_instruction_internal(pyemu_gameboy_system* gb, int render_frame);
static void pyemu_gameboy_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info);
/* Thin vtable adapter that forwards save-state requests into the DMG state module. */
static int pyemu_gameboy_save_state(const pyemu_system* system, const char* path) {
    return pyemu_gameboy_save_state_file((const pyemu_gameboy_system*)system, path);
}
/* Thin vtable adapter that forwards load-state requests into the DMG state module. */
static int pyemu_gameboy_load_state(pyemu_system* system, const char* path) {
    return pyemu_gameboy_load_state_file((pyemu_gameboy_system*)system, path);
}
static int pyemu_gameboy_execute_hotpath(pyemu_gameboy_system* gb, int* out_cycles);
static uint8_t pyemu_gameboy_current_bank_for_address(const pyemu_gameboy_system* gb, uint16_t address);
static pyemu_block_cache_entry* pyemu_gameboy_get_block_cache_entry(pyemu_gameboy_system* gb, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_decode_block(pyemu_gameboy_system* gb, pyemu_block_cache_entry* entry, uint16_t pc, uint8_t bank);
static int pyemu_gameboy_execute_block(pyemu_gameboy_system* gb, const pyemu_block_cache_entry* entry, int* out_cycles);
static int pyemu_gameboy_execute_opcode(pyemu_gameboy_system* gb, uint8_t opcode);
static const char* pyemu_gameboy_name(const pyemu_system* system);
/* Return the registry key for this core. */
static const char* pyemu_gameboy_name(const pyemu_system* system) {
    (void)system;
    return "gameboy";
}
/* Thin vtable adapter that resets the live machine while preserving the loaded cartridge. */
static void pyemu_gameboy_reset(pyemu_system* system) {
    pyemu_gameboy_reset_state((pyemu_gameboy_system*)system);
}
/* Thin vtable adapter that loads a new ROM into this system instance. */
static int pyemu_gameboy_load_rom(pyemu_system* system, const char* path) {
    return pyemu_gameboy_load_rom_file((pyemu_gameboy_system*)system, path);
}
static void pyemu_gameboy_step_instruction(pyemu_system* system);
static void pyemu_gameboy_step_frame(pyemu_system* system);
static void pyemu_gameboy_destroy(pyemu_system* system);

/* Main single-instruction fallback executor used when an opcode is not handled by the decoded block hotpath. */
static int pyemu_gameboy_execute_opcode(pyemu_gameboy_system* gb, uint8_t opcode) {
    int cycles = 4;
    gb->last_opcode = opcode;

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t value = pyemu_gameboy_read_r8(gb, src);
        pyemu_gameboy_write_r8(gb, dst, value);
        return (src == 6 || dst == 6) ? 8 : 4;
    }

    cycles = pyemu_gameboy_execute_load_store(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gameboy_execute_control_flow(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }
    cycles = pyemu_gameboy_execute_alu(gb, opcode);
    if (cycles >= 0) {
        return cycles;
    }

    switch (opcode) {
        case 0x00:
            cycles = 4;
            break;
        case 0x76: {
            uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
            if (!gb->ime && pending_interrupts != 0) {
                gb->halt_bug = 1;
                gb->cpu.halted = 0;
            } else {
                gb->cpu.halted = 1;
            }
            cycles = 4;
            break;
        }
        case 0xE0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            cycles = 12;
            break;
        }
        case 0xE2:
            pyemu_gameboy_write_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c), gb->cpu.a);
            cycles = 8;
            break;
        case 0xE8:
            pyemu_gameboy_add_sp_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb));
            cycles = 16;
            break;
        case 0xEA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            cycles = 16;
            break;
        }
        case 0xF0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            cycles = 12;
            break;
        }
        case 0xF2:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c));
            cycles = 8;
            break;
        case 0xCB:
            cycles = pyemu_gameboy_execute_cb(gb);
            break;
        case 0xF3:
            gb->ime = 0;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            cycles = 4;
            break;
        case 0xF8:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_sp_plus_signed(gb, (int8_t)pyemu_gameboy_fetch_u8(gb)));
            cycles = 12;
            break;
        case 0xFA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            cycles = 16;
            break;
        }
        case 0xFB:
            gb->ime_pending = 1;
            gb->ime_delay = 1;
            cycles = 4;
            break;
        default:
            gb->cpu.halted = 1;
            gb->faulted = 1;
            gb->cpu.pc = (uint16_t)(gb->cpu.pc - 1);
            cycles = 4;
            break;
    }

    return cycles;
}

/* Resolve which ROM bank currently backs a given code address so the block cache does not cross mapper contexts. */
static uint8_t pyemu_gameboy_current_bank_for_address(const pyemu_gameboy_system* gb, uint16_t address) {
    if (address < 0x4000) {
        if (pyemu_gameboy_uses_mbc1(gb) && pyemu_gameboy_mbc1_mode(gb)) {
            uint8_t bank = (uint8_t)(pyemu_gameboy_mbc1_upper_bits(gb) << 5);
            if (gb->rom_bank_count > 0) {
                bank = (uint8_t)(bank % gb->rom_bank_count);
            }
            return bank;
        }
        return 0;
    }
    if (address < 0x8000) {
        return pyemu_gameboy_current_rom_bank(gb);
    }
    return 0xFF;
}

/* Pick the hash-table slot used to cache a decoded basic block for a PC/bank pair. */
static pyemu_block_cache_entry* pyemu_gameboy_get_block_cache_entry(pyemu_gameboy_system* gb, uint16_t pc, uint8_t bank) {
    uint32_t index = ((((uint32_t)bank) << 16) ^ pc) & (PYEMU_GAMEBOY_BLOCK_CACHE_SIZE - 1);
    return &gb->block_cache[index];
}

/* Decode a short straight-line ROM block into the lightweight cached instruction format used by the hotpath executor. */
static int pyemu_gameboy_decode_block(pyemu_gameboy_system* gb, pyemu_block_cache_entry* entry, uint16_t pc, uint8_t bank) {
    uint16_t cursor = pc;
    uint8_t count = 0;

    memset(entry, 0, sizeof(*entry));
    entry->pc = pc;
    entry->bank = bank;

    while (count < PYEMU_GAMEBOY_BLOCK_MAX_INSNS && cursor < 0x8000) {
        pyemu_block_insn* insn = &entry->insns[count];
        uint8_t opcode = pyemu_gameboy_peek_memory(gb, cursor);
        insn->length = 1;

        switch (opcode) {
            case 0x00: insn->type = PYEMU_BLOCK_OP_NOP; break;
            case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
                insn->type = PYEMU_BLOCK_OP_LD_R_D8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0x11:
                insn->type = PYEMU_BLOCK_OP_LD_DE_D16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x21:
                insn->type = PYEMU_BLOCK_OP_LD_HL_D16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0x18: insn->type = PYEMU_BLOCK_OP_JR; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x20: insn->type = PYEMU_BLOCK_OP_JR_NZ; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x28: insn->type = PYEMU_BLOCK_OP_JR_Z; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x30: insn->type = PYEMU_BLOCK_OP_JR_NC; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x38: insn->type = PYEMU_BLOCK_OP_JR_C; insn->relative = (int8_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)); insn->length = 2; entry->terminates = 1; break;
            case 0x05: insn->type = PYEMU_BLOCK_OP_DEC_B; break;
            case 0x22: insn->type = PYEMU_BLOCK_OP_LD_HL_INC_A; break;
            case 0x2A: insn->type = PYEMU_BLOCK_OP_LD_A_HL_INC; break;
            case 0x32: insn->type = PYEMU_BLOCK_OP_LD_HL_DEC_A; break;
            case 0x3A: insn->type = PYEMU_BLOCK_OP_LD_A_HL_DEC; break;
            case 0x3C: insn->type = PYEMU_BLOCK_OP_INC_A; break;
            case 0x6F: insn->type = PYEMU_BLOCK_OP_LD_L_A; break;
            case 0x7D: insn->type = PYEMU_BLOCK_OP_LD_A_L; break;
            case 0x76: insn->type = PYEMU_BLOCK_OP_HALT; entry->terminates = 1; break;
            case 0xA7: insn->type = PYEMU_BLOCK_OP_AND_A; break;
            case 0xAF: insn->type = PYEMU_BLOCK_OP_XOR_A; break;
            case 0xC9: insn->type = PYEMU_BLOCK_OP_RET; entry->terminates = 1; break;
            case 0xE0:
                insn->type = PYEMU_BLOCK_OP_LDH_A8_A;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xEA:
                insn->type = PYEMU_BLOCK_OP_LD_A16_A;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xF0:
                insn->type = PYEMU_BLOCK_OP_LDH_A_A8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            case 0xFA:
                insn->type = PYEMU_BLOCK_OP_LD_A_A16;
                insn->operand16 = (uint16_t)(pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1)) | ((uint16_t)pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 2)) << 8));
                insn->length = 3;
                break;
            case 0xFE:
                insn->type = PYEMU_BLOCK_OP_CP_D8;
                insn->operand8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(cursor + 1));
                insn->length = 2;
                break;
            default:
                entry->valid = 0;
                return 0;
        }

        count = (uint8_t)(count + 1);
        cursor = (uint16_t)(cursor + insn->length);
        if (entry->terminates) {
            break;
        }
    }

    entry->insn_count = count;
    entry->valid = count > 0 ? 1 : 0;
    return entry->valid;
}

/* Execute a previously decoded block and report the total cycles consumed. */
static int pyemu_gameboy_execute_block(pyemu_gameboy_system* gb, const pyemu_block_cache_entry* entry, int* out_cycles) {
    int cycles = 0;
    uint8_t index;

    gb->cpu.pc = entry->pc;
    for (index = 0; index < entry->insn_count; ++index) {
        const pyemu_block_insn* insn = &entry->insns[index];
        uint16_t opcode_pc = gb->cpu.pc;
        uint16_t next_pc = (uint16_t)(opcode_pc + insn->length);
        uint8_t opcode = pyemu_gameboy_peek_memory(gb, opcode_pc);

        switch (insn->type) {
            case PYEMU_BLOCK_OP_NOP: gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_R_D8:
                switch (opcode) {
                    case 0x06: gb->cpu.b = insn->operand8; break;
                    case 0x0E: gb->cpu.c = insn->operand8; break;
                    case 0x16: gb->cpu.d = insn->operand8; break;
                    case 0x1E: gb->cpu.e = insn->operand8; break;
                    case 0x26: gb->cpu.h = insn->operand8; break;
                    case 0x2E: gb->cpu.l = insn->operand8; break;
                    default: gb->cpu.a = insn->operand8; break;
                }
                gb->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_BLOCK_OP_LD_HL_D16: pyemu_gameboy_set_hl(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LD_DE_D16: pyemu_gameboy_set_de(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LDH_A_A8: gb->cpu.a = pyemu_gameboy_read_memory(gb, (uint16_t)(0xFF00 | insn->operand8)); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LDH_A8_A: pyemu_gameboy_write_memory(gb, (uint16_t)(0xFF00 | insn->operand8), gb->cpu.a); gb->cpu.pc = next_pc; cycles += 12; break;
            case PYEMU_BLOCK_OP_LD_A_A16: gb->cpu.a = pyemu_gameboy_read_memory(gb, insn->operand16); gb->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_BLOCK_OP_LD_A16_A: pyemu_gameboy_write_memory(gb, insn->operand16, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 16; break;
            case PYEMU_BLOCK_OP_XOR_A: pyemu_gameboy_xor_a(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_AND_A: pyemu_gameboy_and_a(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_INC_A: gb->cpu.a = pyemu_gameboy_inc8(gb, gb->cpu.a); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_CP_D8: pyemu_gameboy_cp_a(gb, insn->operand8); gb->cpu.pc = next_pc; cycles += 8; break;
            case PYEMU_BLOCK_OP_LD_HL_INC_A: { uint16_t hl = pyemu_gameboy_get_hl(gb); pyemu_gameboy_write_memory(gb, hl, gb->cpu.a); pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_A_HL_INC: { uint16_t hl = pyemu_gameboy_get_hl(gb); gb->cpu.a = pyemu_gameboy_read_memory(gb, hl); pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_A_HL_DEC: { uint16_t hl = pyemu_gameboy_get_hl(gb); gb->cpu.a = pyemu_gameboy_read_memory(gb, hl); pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_LD_HL_DEC_A: { uint16_t hl = pyemu_gameboy_get_hl(gb); pyemu_gameboy_write_memory(gb, hl, gb->cpu.a); pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1)); gb->cpu.pc = next_pc; cycles += 8; break; }
            case PYEMU_BLOCK_OP_DEC_B: gb->cpu.b = pyemu_gameboy_dec8(gb, gb->cpu.b); gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_A_L: gb->cpu.a = gb->cpu.l; gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_LD_L_A: gb->cpu.l = gb->cpu.a; gb->cpu.pc = next_pc; cycles += 4; break;
            case PYEMU_BLOCK_OP_JR: gb->cpu.pc = (uint16_t)(next_pc + insn->relative); cycles += 12; *out_cycles = cycles; return 1;
            case PYEMU_BLOCK_OP_JR_NZ: { int cond = !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_Z: { int cond = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_NC: { int cond = !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_JR_C: { int cond = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C); gb->cpu.pc = cond ? (uint16_t)(next_pc + insn->relative) : next_pc; cycles += cond ? 12 : 8; *out_cycles = cycles; return 1; }
            case PYEMU_BLOCK_OP_RET: gb->cpu.pc = pyemu_gameboy_pop_u16(gb); cycles += 16; *out_cycles = cycles; return 1;
            case PYEMU_BLOCK_OP_HALT: {
                uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
                if (!gb->ime && pending_interrupts != 0) {
                    gb->halt_bug = 1;
                    gb->cpu.halted = 0;
                } else {
                    gb->cpu.halted = 1;
                }
                gb->cpu.pc = next_pc;
                cycles += 4;
                *out_cycles = cycles;
                return 1;
            }
            default: return 0;
        }
    }

    *out_cycles = cycles;
    return 1;
}

/* Try to run the current PC through the ROM block cache before falling back to the full interpreter. */
static int pyemu_gameboy_execute_hotpath(pyemu_gameboy_system* gb, int* out_cycles) {
    uint16_t pc = gb->cpu.pc;

    if (pc <= 0xFFF6U) {
        uint8_t b0 = pyemu_gameboy_peek_memory(gb, pc);
        uint8_t b1 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 1));
        uint8_t b2 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 2));
        uint8_t b3 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 3));
        uint8_t b4 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 4));
        uint8_t b5 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 5));
        uint8_t b6 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 6));
        uint8_t b7 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 7));
        uint8_t b8 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 8));
        uint8_t b9 = pyemu_gameboy_peek_memory(gb, (uint16_t)(pc + 9));

        if (b0 == 0x21 && b3 == 0x06 && b5 == 0x22 && b6 == 0x05 && b7 == 0x20 && b8 == 0xFC && b9 == 0xC9) {
            uint16_t address = (uint16_t)(b1 | ((uint16_t)b2 << 8));
            uint8_t count = b4;
            uint8_t value = gb->cpu.a;
            uint16_t index;

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), value);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.b = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(24U * (uint32_t)count + 32U);
            return 1;
        }

        if (b0 == 0xAF && b1 == 0x22 && b2 == 0x7D && b3 == 0xFE && b5 == 0x38 && b6 == 0xF9 && b7 == 0xC9) {
            uint8_t limit = b4;
            uint16_t address = pyemu_gameboy_get_hl(gb);
            uint8_t start_low = gb->cpu.l;
            uint8_t count = limit > start_low ? (uint8_t)(limit - start_low) : 0;
            uint16_t index;

            gb->cpu.a = 0;
            if (count == 0) {
                gb->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 24;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), 0);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.a = limit;
            gb->cpu.f = (uint8_t)(PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(32U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x22 && b1 == 0x05 && b2 == 0x20 && b3 == 0xFC && b4 == 0xC9) {
            uint16_t address = pyemu_gameboy_get_hl(gb);
            uint8_t count = gb->cpu.b;
            uint8_t value = gb->cpu.a;
            uint16_t index;

            if (count == 0) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 16;
                return 1;
            }

            for (index = 0; index < count; ++index) {
                pyemu_gameboy_write_memory(gb, (uint16_t)(address + index), value);
            }

            pyemu_gameboy_set_hl(gb, (uint16_t)(address + count));
            gb->cpu.b = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(20U * (uint32_t)count + 16U);
            return 1;
        }

        if (b0 == 0x3D && b1 == 0x20 && b2 == 0xFD && b3 == 0xC9) {
            uint8_t count = gb->cpu.a;
            if (count == 0) {
                gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                *out_cycles = 16;
                return 1;
            }
            gb->cpu.a = 0;
            gb->cpu.f = (uint8_t)((gb->cpu.f & PYEMU_FLAG_C) | PYEMU_FLAG_Z | PYEMU_FLAG_N);
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            *out_cycles = (int)(12U * (uint32_t)count + 4U);
            return 1;
        }
    }

    return 0;
}

/* Core execution step used by both debugger stepping and frame-running paths. */
static void pyemu_gameboy_step_instruction_internal(pyemu_gameboy_system* gb, int render_frame) {
    uint8_t opcode;
    int cycles;

    if (gb->faulted) {
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (pyemu_gameboy_service_interrupts(gb) > 0) {
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (gb->cpu.halted) {
        int halted_cycles = 4;
        if (!render_frame) {
            uint8_t enabled_interrupts = (uint8_t)(gb->memory[PYEMU_IO_IE] & 0x1F);
            uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
            if (pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
                uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
                if ((gb->memory[PYEMU_IO_TAC] & 0x04) == 0 && skip_cycles > 4U) {
                    pyemu_gameboy_fast_forward_to_vblank(gb);
                    if (render_frame) {
                        pyemu_gameboy_update_demo_frame(gb);
                    }
                    return;
                }
                if (skip_cycles > 4U) {
                    halted_cycles = (int)skip_cycles;
                }
            }
        }
        pyemu_gameboy_tick(gb, halted_cycles);
        if (render_frame) {
            pyemu_gameboy_update_demo_frame(gb);
        }
        return;
    }

    if (gb->cpu.pc < 0x8000) {
        uint8_t bank = pyemu_gameboy_current_bank_for_address(gb, gb->cpu.pc);
        if (bank != 0xFF) {
            pyemu_block_cache_entry* entry = pyemu_gameboy_get_block_cache_entry(gb, gb->cpu.pc, bank);
            if ((!entry->valid || entry->pc != gb->cpu.pc || entry->bank != bank) && !pyemu_gameboy_decode_block(gb, entry, gb->cpu.pc, bank)) {
                entry->valid = 0;
            }
            if (entry->valid && entry->pc == gb->cpu.pc && entry->bank == bank && pyemu_gameboy_execute_block(gb, entry, &cycles)) {
                pyemu_gameboy_tick(gb, cycles);
            } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
                pyemu_gameboy_tick(gb, cycles);
            } else {
                opcode = pyemu_gameboy_fetch_u8(gb);
                cycles = pyemu_gameboy_execute_opcode(gb, opcode);
                pyemu_gameboy_tick(gb, cycles);
            }
        } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
            pyemu_gameboy_tick(gb, cycles);
        } else {
            opcode = pyemu_gameboy_fetch_u8(gb);
            cycles = pyemu_gameboy_execute_opcode(gb, opcode);
            pyemu_gameboy_tick(gb, cycles);
        }
    } else if (pyemu_gameboy_execute_hotpath(gb, &cycles)) {
        pyemu_gameboy_tick(gb, cycles);
    } else {
        opcode = pyemu_gameboy_fetch_u8(gb);
        cycles = pyemu_gameboy_execute_opcode(gb, opcode);
        pyemu_gameboy_tick(gb, cycles);
    }

    if (gb->ime_pending) {
        if (gb->ime_delay > 0) {
            gb->ime_delay -= 1;
        } else {
            gb->ime = 1;
            gb->ime_pending = 0;
        }
    }

    if (render_frame) {
        pyemu_gameboy_update_demo_frame(gb);
    }
}

/* Vtable entry for single-instruction stepping. */
static void pyemu_gameboy_step_instruction(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    pyemu_gameboy_step_instruction_internal(gb, 1);
}

/* Vtable entry that advances the emulator until a full video frame has elapsed. */
static void pyemu_gameboy_step_frame(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    uint64_t target_cycles = gb->cycle_count + PYEMU_GAMEBOY_CYCLES_PER_FRAME;

    while (gb->cycle_count < target_cycles) {
        uint8_t enabled_interrupts = (uint8_t)(gb->memory[PYEMU_IO_IE] & 0x1F);
        uint8_t pending_interrupts = pyemu_gameboy_pending_interrupts(gb);
        uint64_t remaining_cycles = target_cycles - gb->cycle_count;

        if (gb->cpu.halted && !gb->faulted && pending_interrupts == 0 && enabled_interrupts == PYEMU_INTERRUPT_VBLANK) {
            uint32_t skip_cycles = pyemu_gameboy_cycles_until_vblank(gb);
            if (skip_cycles > 4U) {
                if ((gb->memory[PYEMU_IO_TAC] & 0x04) == 0 && (uint64_t)skip_cycles <= remaining_cycles) {
                    pyemu_gameboy_fast_forward_to_vblank(gb);
                    continue;
                }
                if ((uint64_t)skip_cycles > remaining_cycles) {
                    skip_cycles = (uint32_t)remaining_cycles;
                }
                pyemu_gameboy_tick(gb, (int)skip_cycles);
                continue;
            }
        }

        pyemu_gameboy_step_instruction_internal(gb, 0);
        if (gb->faulted) {
            break;
        }
    }

    pyemu_gameboy_update_audio_frame(gb);
}

/* Release heap-owned cartridge data and flush battery RAM before destroying the core instance. */
static void pyemu_gameboy_destroy(pyemu_system* system) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb != NULL) {
        if (gb->battery_dirty) {
            pyemu_gameboy_save_battery_ram(gb);
        }
        free(gb->rom_data);
    }
    free(system);
}

/* Export the current CPU register snapshot to the debugger/runtime layer. */
static void pyemu_gameboy_get_cpu_state(const pyemu_system* system, pyemu_cpu_state* out_state) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_state != NULL) {
        *out_state = gb->cpu;
        out_state->ime = (uint8_t)(gb->ime ? 1 : 0);
    }
}

/* Export the most recently rendered RGBA framebuffer. */
static void pyemu_gameboy_get_frame_buffer(const pyemu_system* system, pyemu_frame_buffer* out_frame) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_frame == NULL) {
        return;
    }
    out_frame->width = PYEMU_GAMEBOY_WIDTH;
    out_frame->height = PYEMU_GAMEBOY_HEIGHT;
    out_frame->rgba = gb->frame;
    out_frame->rgba_size = PYEMU_GAMEBOY_RGBA_SIZE;
}

/* Export the flat 64K memory image used by debugger views. */
static const uint8_t* pyemu_gameboy_get_memory(const pyemu_system* system, size_t* size) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (size != NULL) {
        *size = PYEMU_GAMEBOY_MEMORY_SIZE;
    }
    return gb->memory;
}

/* Vtable hook used by cheats and debugger memory editing. */
static void pyemu_gameboy_poke_memory(pyemu_system* system, uint16_t address, uint8_t value) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return;
    }
    pyemu_gameboy_write_memory(gb, address, value);
}

/* Vtable hook for side-effect-free debugger memory reads. */
static uint8_t pyemu_gameboy_peek_memory_vtable(pyemu_system* system, uint16_t address) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return 0xFF;
    }
    return pyemu_gameboy_peek_memory(gb, address);
}

/* Report whether this system instance currently has cartridge media loaded. */
static int pyemu_gameboy_has_rom_loaded(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->rom_loaded;
}

/* Return the path of the currently loaded ROM, if any. */
static const char* pyemu_gameboy_get_rom_path(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->loaded_rom;
}

/* Return the parsed cartridge title for UI and debugger display. */
static const char* pyemu_gameboy_get_cartridge_title(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->cartridge_title;
}

/* Return the byte size of the currently loaded ROM image. */
static size_t pyemu_gameboy_get_rom_size(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->rom_size;
}

/* Export the most recent tracked bus access for debugger hardware panels. */
static void pyemu_gameboy_get_last_bus_access(const pyemu_system* system, pyemu_last_bus_access* out_access) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    if (out_access != NULL) {
        *out_access = gb->last_access;
    }
}

/* Export mapper and external-RAM status for debugger inspection. */
static void pyemu_gameboy_get_cartridge_debug_info(const pyemu_system* system, pyemu_cartridge_debug_info* out_info) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;

    if (out_info == NULL) {
        return;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->cartridge_type = gb->cartridge_type;
    out_info->rom_size_code = (gb->rom_data != NULL && gb->rom_size > 0x0148) ? gb->rom_data[0x0148] : 0;
    out_info->ram_size_code = gb->ram_size_code;
    out_info->ram_enabled = gb->ram_enabled;
    out_info->rom_bank = pyemu_gameboy_current_rom_bank(gb);
    out_info->ram_bank = (uint8_t)(pyemu_gameboy_current_eram_offset(gb) / PYEMU_GAMEBOY_ERAM_BANK_SIZE);
    out_info->banking_mode = pyemu_gameboy_uses_mbc1(gb) ? pyemu_gameboy_mbc1_mode(gb) : 0;
    out_info->has_battery = (uint8_t)(pyemu_gameboy_has_battery(gb) ? 1 : 0);
    out_info->save_file_present = (uint8_t)(pyemu_gameboy_save_file_present(gb) ? 1 : 0);
    out_info->last_mapper_value = gb->last_mapper_access.value;
    out_info->last_mapper_valid = gb->last_mapper_access.valid;
    out_info->last_mapper_address = gb->last_mapper_access.address;
    out_info->rom_bank_count = (uint32_t)gb->rom_bank_count;
    out_info->ram_bank_count = (uint32_t)gb->eram_bank_count;
}

/* Return the cumulative machine cycle counter. */
static uint64_t pyemu_gameboy_get_cycle_count(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->cycle_count;
}

/* Report whether execution stopped on an unsupported or invalid path. */
static int pyemu_gameboy_is_faulted(const pyemu_system* system) {
    const pyemu_gameboy_system* gb = (const pyemu_gameboy_system*)system;
    return gb->faulted;
}

static const pyemu_system_vtable pyemu_gameboy_vtable = {
    pyemu_gameboy_name,
    pyemu_gameboy_reset,
    pyemu_gameboy_load_rom,
    pyemu_gameboy_save_state,
    pyemu_gameboy_load_state,
    pyemu_gameboy_step_instruction,
    pyemu_gameboy_step_frame,
    pyemu_gameboy_destroy,
    pyemu_gameboy_get_cpu_state,
    pyemu_gameboy_get_frame_buffer,
    pyemu_gameboy_get_audio_buffer,
    pyemu_gameboy_get_memory,
    pyemu_gameboy_poke_memory,
    pyemu_gameboy_peek_memory_vtable,
    pyemu_gameboy_has_rom_loaded,
    pyemu_gameboy_get_rom_path,
    pyemu_gameboy_get_cartridge_title,
    pyemu_gameboy_get_rom_size,
    pyemu_gameboy_get_cycle_count,
    pyemu_gameboy_get_last_bus_access,
    pyemu_gameboy_get_cartridge_debug_info,
    pyemu_gameboy_is_faulted
};

/* Allocate and initialize a new DMG core instance, then wire its vtable. */
pyemu_system* pyemu_gameboy_create(void) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)calloc(1, sizeof(pyemu_gameboy_system));
    if (gb == NULL) {
        return NULL;
    }

    gb->base.vtable = &pyemu_gameboy_vtable;
    pyemu_gameboy_reset((pyemu_system*)gb);
    return (pyemu_system*)gb;
}

