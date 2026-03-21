#include "gameboy_internal.h"

/* Opcode-family helpers for the DMG CPU. The top-level executor delegates here so large instruction groups stay isolated and reusable across other LR35902-style cores. */

/* Decode the compact r8 register index used by many opcodes, including the (HL) pseudo-register. */
uint8_t pyemu_gameboy_read_r8(pyemu_gameboy_system* gb, int index) {
    switch (index & 0x07) {
        case 0: return gb->cpu.b;
        case 1: return gb->cpu.c;
        case 2: return gb->cpu.d;
        case 3: return gb->cpu.e;
        case 4: return gb->cpu.h;
        case 5: return gb->cpu.l;
        case 6: return pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
        default: return gb->cpu.a;
    }
}

/* Write through the compact r8 register index used by many opcodes, including the (HL) pseudo-register. */
void pyemu_gameboy_write_r8(pyemu_gameboy_system* gb, int index, uint8_t value) {
    switch (index & 0x07) {
        case 0: gb->cpu.b = value; break;
        case 1: gb->cpu.c = value; break;
        case 2: gb->cpu.d = value; break;
        case 3: gb->cpu.e = value; break;
        case 4: gb->cpu.h = value; break;
        case 5: gb->cpu.l = value; break;
        case 6: pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_hl(gb), value); break;
        default: gb->cpu.a = value; break;
    }
}

/* Execute the CB-prefixed rotate/shift/bit instruction family. */
int pyemu_gameboy_execute_cb(pyemu_gameboy_system* gb) {
    uint8_t cb_opcode = pyemu_gameboy_fetch_u8(gb);
    int reg_index = cb_opcode & 0x07;
    uint8_t value;
    int cycles = 8;

    if (cb_opcode <= 0x07) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rlc8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x0F) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rrc8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x17) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rl8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x1F) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_rr8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x27) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_sla8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x2F) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_sra8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if ((cb_opcode & 0xF8) == 0x30) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_swap8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode <= 0x3F) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_write_r8(gb, reg_index, pyemu_gameboy_srl8(gb, value));
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode >= 0x40 && cb_opcode <= 0x7F) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        pyemu_gameboy_bit_test(gb, value, (uint8_t)((cb_opcode - 0x40) / 8));
        return reg_index == 6 ? 12 : 8;
    }
    if (cb_opcode >= 0x80 && cb_opcode <= 0xBF) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        value = (uint8_t)(value & (uint8_t)~(1U << ((cb_opcode - 0x80) / 8)));
        pyemu_gameboy_write_r8(gb, reg_index, value);
        return reg_index == 6 ? 16 : 8;
    }
    if (cb_opcode >= 0xC0) {
        value = pyemu_gameboy_read_r8(gb, reg_index);
        value = (uint8_t)(value | (uint8_t)(1U << ((cb_opcode - 0xC0) / 8)));
        pyemu_gameboy_write_r8(gb, reg_index, value);
        return reg_index == 6 ? 16 : 8;
    }

    gb->cpu.halted = 1;
    gb->faulted = 0;
    gb->cpu.pc = (uint16_t)(gb->cpu.pc - 2);
    return cycles;
}

/* Handle the load/store, 16-bit register, and closely related misc instructions that form the bulk of DMG setup code. */
int pyemu_gameboy_execute_load_store(pyemu_gameboy_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0x01:
            pyemu_gameboy_set_bc(gb, pyemu_gameboy_fetch_u16(gb));
            return 12;
        case 0x02:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_bc(gb), gb->cpu.a);
            return 8;
        case 0x03:
            pyemu_gameboy_set_bc(gb, (uint16_t)(pyemu_gameboy_get_bc(gb) + 1));
            return 8;
        case 0x04:
            gb->cpu.b = pyemu_gameboy_inc8(gb, gb->cpu.b);
            return 4;
        case 0x05:
            gb->cpu.b = pyemu_gameboy_dec8(gb, gb->cpu.b);
            return 4;
        case 0x06:
            gb->cpu.b = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x07:
            pyemu_gameboy_rlca(gb);
            return 4;
        case 0x08: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, (uint8_t)(gb->cpu.sp & 0xFF));
            pyemu_gameboy_write_memory(gb, (uint16_t)(address + 1), (uint8_t)(gb->cpu.sp >> 8));
            return 20;
        }
        case 0x09:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_bc(gb));
            return 8;
        case 0x0A:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_bc(gb));
            return 8;
        case 0x0B:
            pyemu_gameboy_set_bc(gb, (uint16_t)(pyemu_gameboy_get_bc(gb) - 1));
            return 8;
        case 0x0C:
            gb->cpu.c = pyemu_gameboy_inc8(gb, gb->cpu.c);
            return 4;
        case 0x0D:
            gb->cpu.c = pyemu_gameboy_dec8(gb, gb->cpu.c);
            return 4;
        case 0x0E:
            gb->cpu.c = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x0F:
            pyemu_gameboy_rrca(gb);
            return 4;
        case 0x11:
            pyemu_gameboy_set_de(gb, pyemu_gameboy_fetch_u16(gb));
            return 12;
        case 0x12:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_de(gb), gb->cpu.a);
            return 8;
        case 0x13:
            pyemu_gameboy_set_de(gb, (uint16_t)(pyemu_gameboy_get_de(gb) + 1));
            return 8;
        case 0x14:
            gb->cpu.d = pyemu_gameboy_inc8(gb, gb->cpu.d);
            return 4;
        case 0x15:
            gb->cpu.d = pyemu_gameboy_dec8(gb, gb->cpu.d);
            return 4;
        case 0x16:
            gb->cpu.d = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x17:
            pyemu_gameboy_rla(gb);
            return 4;
        case 0x18: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
            return 12;
        }
        case 0x19:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_de(gb));
            return 8;
        case 0x1A:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_de(gb));
            return 8;
        case 0x1B:
            pyemu_gameboy_set_de(gb, (uint16_t)(pyemu_gameboy_get_de(gb) - 1));
            return 8;
        case 0x1C:
            gb->cpu.e = pyemu_gameboy_inc8(gb, gb->cpu.e);
            return 4;
        case 0x1D:
            gb->cpu.e = pyemu_gameboy_dec8(gb, gb->cpu.e);
            return 4;
        case 0x1E:
            gb->cpu.e = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x1F:
            pyemu_gameboy_rra(gb);
            return 4;
        case 0x20: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x21:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_fetch_u16(gb));
            return 12;
        case 0x22: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, gb->cpu.a);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1));
            return 8;
        }
        case 0x23:
            pyemu_gameboy_set_hl(gb, (uint16_t)(pyemu_gameboy_get_hl(gb) + 1));
            return 8;
        case 0x24:
            gb->cpu.h = pyemu_gameboy_inc8(gb, gb->cpu.h);
            return 4;
        case 0x25:
            gb->cpu.h = pyemu_gameboy_dec8(gb, gb->cpu.h);
            return 4;
        case 0x26:
            gb->cpu.h = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x27:
            pyemu_gameboy_daa(gb);
            return 4;
        case 0x28: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x29:
            pyemu_gameboy_add_hl(gb, pyemu_gameboy_get_hl(gb));
            return 8;
        case 0x2A: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl + 1));
            return 8;
        }
        case 0x2B:
            pyemu_gameboy_set_hl(gb, (uint16_t)(pyemu_gameboy_get_hl(gb) - 1));
            return 8;
        case 0x2C:
            gb->cpu.l = pyemu_gameboy_inc8(gb, gb->cpu.l);
            return 4;
        case 0x2D:
            gb->cpu.l = pyemu_gameboy_dec8(gb, gb->cpu.l);
            return 4;
        case 0x2E:
            gb->cpu.l = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x2F:
            gb->cpu.a = (uint8_t)~gb->cpu.a;
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 1);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 1);
            return 4;
        case 0x30: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x31:
            gb->cpu.sp = pyemu_gameboy_fetch_u16(gb);
            return 12;
        case 0x32: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, gb->cpu.a);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1));
            return 8;
        }
        case 0x33:
            gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
            return 8;
        case 0x34: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            uint8_t value = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_inc8(gb, value));
            return 12;
        }
        case 0x35: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            uint8_t value = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_dec8(gb, value));
            return 12;
        }
        case 0x36: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            pyemu_gameboy_write_memory(gb, hl, pyemu_gameboy_fetch_u8(gb));
            return 12;
        }
        case 0x37:
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, 1);
            return 4;
        case 0x38: {
            int8_t offset = (int8_t)pyemu_gameboy_fetch_u8(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = (uint16_t)(gb->cpu.pc + offset);
                return 12;
            }
            return 8;
        }
        case 0x39:
            pyemu_gameboy_add_hl(gb, gb->cpu.sp);
            return 8;
        case 0x3A: {
            uint16_t hl = pyemu_gameboy_get_hl(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, hl);
            pyemu_gameboy_set_hl(gb, (uint16_t)(hl - 1));
            return 8;
        }
        case 0x3B:
            gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
            return 8;
        case 0x3C:
            gb->cpu.a = pyemu_gameboy_inc8(gb, gb->cpu.a);
            return 4;
        case 0x3D:
            gb->cpu.a = pyemu_gameboy_dec8(gb, gb->cpu.a);
            return 4;
        case 0x3F:
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
            pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, !pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C));
            return 4;
        case 0x3E:
            gb->cpu.a = pyemu_gameboy_fetch_u8(gb);
            return 8;
        case 0x46:
            gb->cpu.b = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
            return 8;
        case 0x47:
        case 0x4F:
        case 0x57:
        case 0x5F:
        case 0x67:
        case 0x6F:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
            switch (opcode) {
                case 0x47: gb->cpu.b = gb->cpu.a; break;
                case 0x4F: gb->cpu.c = gb->cpu.a; break;
                case 0x57: gb->cpu.d = gb->cpu.a; break;
                case 0x5F: gb->cpu.e = gb->cpu.a; break;
                case 0x67: gb->cpu.h = gb->cpu.a; break;
                case 0x6F: gb->cpu.l = gb->cpu.a; break;
                case 0x78: gb->cpu.a = gb->cpu.b; break;
                case 0x79: gb->cpu.a = gb->cpu.c; break;
                case 0x7A: gb->cpu.a = gb->cpu.d; break;
                case 0x7B: gb->cpu.a = gb->cpu.e; break;
                case 0x7C: gb->cpu.a = gb->cpu.h; break;
                case 0x7D: gb->cpu.a = gb->cpu.l; break;
            }
            return 4;
        case 0x77:
            pyemu_gameboy_write_memory(gb, pyemu_gameboy_get_hl(gb), gb->cpu.a);
            return 8;
        case 0x7E:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb));
            return 8;
        case 0xE0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            return 12;
        }
        case 0xE2:
            pyemu_gameboy_write_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c), gb->cpu.a);
            return 8;
        case 0xEA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_write_memory(gb, address, gb->cpu.a);
            return 16;
        }
        case 0xF0: {
            uint16_t address = (uint16_t)(0xFF00 | pyemu_gameboy_fetch_u8(gb));
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            return 12;
        }
        case 0xF2:
            gb->cpu.a = pyemu_gameboy_read_memory(gb, (uint16_t)(0xFF00 | gb->cpu.c));
            return 8;
        case 0xFA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            gb->cpu.a = pyemu_gameboy_read_memory(gb, address);
            return 16;
        }
        default:
            return -1;
    }
}

/* Execute jump, call, return, restart, and stack-manipulation opcodes. */
int pyemu_gameboy_execute_control_flow(pyemu_gameboy_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0xC0:
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xC1:
            pyemu_gameboy_set_bc(gb, pyemu_gameboy_pop_u16(gb));
            return 12;
        case 0xC2: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xC3:
            gb->cpu.pc = pyemu_gameboy_fetch_u16(gb);
            return 16;
        case 0xC4: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xC5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_bc(gb));
            return 16;
        case 0xC7:
        case 0xCF:
        case 0xD7:
        case 0xDF:
        case 0xE7:
        case 0xEF:
        case 0xF7:
        case 0xFF:
            pyemu_gameboy_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = (uint16_t)(opcode & 0x38);
            return 16;
        case 0xC8:
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xC9:
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            return 16;
        case 0xCA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xCC: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_Z)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xCD: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            pyemu_gameboy_push_u16(gb, gb->cpu.pc);
            gb->cpu.pc = address;
            return 24;
        }
        case 0xD0:
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xD1:
            pyemu_gameboy_set_de(gb, pyemu_gameboy_pop_u16(gb));
            return 12;
        case 0xD2: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xD4: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xD5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_de(gb));
            return 16;
        case 0xD8:
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
                return 20;
            }
            return 8;
        case 0xD9:
            gb->cpu.pc = pyemu_gameboy_pop_u16(gb);
            gb->ime = 1;
            gb->ime_pending = 0;
            gb->ime_delay = 0;
            return 16;
        case 0xDA: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                gb->cpu.pc = address;
                return 16;
            }
            return 12;
        }
        case 0xDC: {
            uint16_t address = pyemu_gameboy_fetch_u16(gb);
            if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C)) {
                pyemu_gameboy_push_u16(gb, gb->cpu.pc);
                gb->cpu.pc = address;
                return 24;
            }
            return 12;
        }
        case 0xE1:
            pyemu_gameboy_set_hl(gb, pyemu_gameboy_pop_u16(gb));
            return 12;
        case 0xE5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_hl(gb));
            return 16;
        case 0xE9:
            gb->cpu.pc = pyemu_gameboy_get_hl(gb);
            return 4;
        case 0xF1:
            pyemu_gameboy_set_af(gb, pyemu_gameboy_pop_u16(gb));
            return 12;
        case 0xF5:
            pyemu_gameboy_push_u16(gb, pyemu_gameboy_get_af(gb));
            return 16;
        case 0xF9:
            gb->cpu.sp = pyemu_gameboy_get_hl(gb);
            return 8;
        default:
            return -1;
    }
}

/* Execute arithmetic and logic opcodes, including the immediate ALU family. */
int pyemu_gameboy_execute_alu(pyemu_gameboy_system* gb, uint8_t opcode) {
    switch (opcode) {
        case 0x80: pyemu_gameboy_add_a(gb, gb->cpu.b, 0); return 4;
        case 0x81: pyemu_gameboy_add_a(gb, gb->cpu.c, 0); return 4;
        case 0x82: pyemu_gameboy_add_a(gb, gb->cpu.d, 0); return 4;
        case 0x83: pyemu_gameboy_add_a(gb, gb->cpu.e, 0); return 4;
        case 0x84: pyemu_gameboy_add_a(gb, gb->cpu.h, 0); return 4;
        case 0x85: pyemu_gameboy_add_a(gb, gb->cpu.l, 0); return 4;
        case 0x86: pyemu_gameboy_add_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)), 0); return 8;
        case 0x87: pyemu_gameboy_add_a(gb, gb->cpu.a, 0); return 4;
        case 0x88: pyemu_gameboy_add_a(gb, gb->cpu.b, 1); return 4;
        case 0x89: pyemu_gameboy_add_a(gb, gb->cpu.c, 1); return 4;
        case 0x8A: pyemu_gameboy_add_a(gb, gb->cpu.d, 1); return 4;
        case 0x8B: pyemu_gameboy_add_a(gb, gb->cpu.e, 1); return 4;
        case 0x8C: pyemu_gameboy_add_a(gb, gb->cpu.h, 1); return 4;
        case 0x8D: pyemu_gameboy_add_a(gb, gb->cpu.l, 1); return 4;
        case 0x8E: pyemu_gameboy_add_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb)), 1); return 8;
        case 0x8F: pyemu_gameboy_add_a(gb, gb->cpu.a, 1); return 4;
        case 0x90: pyemu_gameboy_sub_a(gb, gb->cpu.b); return 4;
        case 0x91: pyemu_gameboy_sub_a(gb, gb->cpu.c); return 4;
        case 0x92: pyemu_gameboy_sub_a(gb, gb->cpu.d); return 4;
        case 0x93: pyemu_gameboy_sub_a(gb, gb->cpu.e); return 4;
        case 0x94: pyemu_gameboy_sub_a(gb, gb->cpu.h); return 4;
        case 0x95: pyemu_gameboy_sub_a(gb, gb->cpu.l); return 4;
        case 0x96: pyemu_gameboy_sub_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0x97: pyemu_gameboy_sub_a(gb, gb->cpu.a); return 4;
        case 0x98: pyemu_gameboy_sbc_a(gb, gb->cpu.b); return 4;
        case 0x99: pyemu_gameboy_sbc_a(gb, gb->cpu.c); return 4;
        case 0x9A: pyemu_gameboy_sbc_a(gb, gb->cpu.d); return 4;
        case 0x9B: pyemu_gameboy_sbc_a(gb, gb->cpu.e); return 4;
        case 0x9C: pyemu_gameboy_sbc_a(gb, gb->cpu.h); return 4;
        case 0x9D: pyemu_gameboy_sbc_a(gb, gb->cpu.l); return 4;
        case 0x9E: pyemu_gameboy_sbc_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0x9F: pyemu_gameboy_sbc_a(gb, gb->cpu.a); return 4;
        case 0xA0: pyemu_gameboy_and_a(gb, gb->cpu.b); return 4;
        case 0xA1: pyemu_gameboy_and_a(gb, gb->cpu.c); return 4;
        case 0xA2: pyemu_gameboy_and_a(gb, gb->cpu.d); return 4;
        case 0xA3: pyemu_gameboy_and_a(gb, gb->cpu.e); return 4;
        case 0xA4: pyemu_gameboy_and_a(gb, gb->cpu.h); return 4;
        case 0xA5: pyemu_gameboy_and_a(gb, gb->cpu.l); return 4;
        case 0xA6: pyemu_gameboy_and_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0xA7: pyemu_gameboy_and_a(gb, gb->cpu.a); return 4;
        case 0xA8: pyemu_gameboy_xor_a(gb, gb->cpu.b); return 4;
        case 0xA9: pyemu_gameboy_xor_a(gb, gb->cpu.c); return 4;
        case 0xAA: pyemu_gameboy_xor_a(gb, gb->cpu.d); return 4;
        case 0xAB: pyemu_gameboy_xor_a(gb, gb->cpu.e); return 4;
        case 0xAC: pyemu_gameboy_xor_a(gb, gb->cpu.h); return 4;
        case 0xAD: pyemu_gameboy_xor_a(gb, gb->cpu.l); return 4;
        case 0xAE: pyemu_gameboy_xor_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0xAF: pyemu_gameboy_xor_a(gb, gb->cpu.a); return 4;
        case 0xB0: pyemu_gameboy_or_a(gb, gb->cpu.b); return 4;
        case 0xB1: pyemu_gameboy_or_a(gb, gb->cpu.c); return 4;
        case 0xB2: pyemu_gameboy_or_a(gb, gb->cpu.d); return 4;
        case 0xB3: pyemu_gameboy_or_a(gb, gb->cpu.e); return 4;
        case 0xB4: pyemu_gameboy_or_a(gb, gb->cpu.h); return 4;
        case 0xB5: pyemu_gameboy_or_a(gb, gb->cpu.l); return 4;
        case 0xB6: pyemu_gameboy_or_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0xB7: pyemu_gameboy_or_a(gb, gb->cpu.a); return 4;
        case 0xB8: pyemu_gameboy_cp_a(gb, gb->cpu.b); return 4;
        case 0xB9: pyemu_gameboy_cp_a(gb, gb->cpu.c); return 4;
        case 0xBA: pyemu_gameboy_cp_a(gb, gb->cpu.d); return 4;
        case 0xBB: pyemu_gameboy_cp_a(gb, gb->cpu.e); return 4;
        case 0xBC: pyemu_gameboy_cp_a(gb, gb->cpu.h); return 4;
        case 0xBD: pyemu_gameboy_cp_a(gb, gb->cpu.l); return 4;
        case 0xBE: pyemu_gameboy_cp_a(gb, pyemu_gameboy_read_memory(gb, pyemu_gameboy_get_hl(gb))); return 8;
        case 0xBF: pyemu_gameboy_cp_a(gb, gb->cpu.a); return 4;
        case 0xC6: pyemu_gameboy_add_a(gb, pyemu_gameboy_fetch_u8(gb), 0); return 8;
        case 0xCE: pyemu_gameboy_add_a(gb, pyemu_gameboy_fetch_u8(gb), 1); return 8;
        case 0xD6: pyemu_gameboy_sub_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        case 0xDE: pyemu_gameboy_sbc_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        case 0xE6: pyemu_gameboy_and_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        case 0xEE: pyemu_gameboy_xor_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        case 0xF6: pyemu_gameboy_or_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        case 0xFE: pyemu_gameboy_cp_a(gb, pyemu_gameboy_fetch_u8(gb)); return 8;
        default:
            return -1;
    }
}
