#include "gameboy_internal.h"

/* Small LR35902 helper primitives. These wrap register-pair packing, flag handling, ALU micro-ops, and rotate/shift behavior so opcode dispatch stays readable. */

/* Pack the AF register pair from the split CPU register struct. */
uint16_t pyemu_gameboy_get_af(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.a << 8) | (gb->cpu.f & 0xF0));
}

/* Pack the BC register pair from the split CPU register struct. */
uint16_t pyemu_gameboy_get_bc(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.b << 8) | gb->cpu.c);
}

/* Pack the DE register pair from the split CPU register struct. */
uint16_t pyemu_gameboy_get_de(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.d << 8) | gb->cpu.e);
}

/* Pack the HL register pair from the split CPU register struct. */
uint16_t pyemu_gameboy_get_hl(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.h << 8) | gb->cpu.l);
}

/* Unpack a 16-bit AF value into the split CPU register struct, masking the unused flag bits. */
void pyemu_gameboy_set_af(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.a = (uint8_t)(value >> 8);
    gb->cpu.f = (uint8_t)(value & 0xF0);
}

/* Unpack a 16-bit BC value into the split CPU register struct. */
void pyemu_gameboy_set_bc(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.b = (uint8_t)(value >> 8);
    gb->cpu.c = (uint8_t)(value & 0xFF);
}

/* Unpack a 16-bit DE value into the split CPU register struct. */
void pyemu_gameboy_set_de(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.d = (uint8_t)(value >> 8);
    gb->cpu.e = (uint8_t)(value & 0xFF);
}

/* Unpack a 16-bit HL value into the split CPU register struct. */
void pyemu_gameboy_set_hl(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.h = (uint8_t)(value >> 8);
    gb->cpu.l = (uint8_t)(value & 0xFF);
}

/* Set or clear a single flag bit in F without disturbing the other architecturally visible bits. */
void pyemu_gameboy_set_flag(pyemu_gameboy_system* gb, uint8_t mask, int enabled) {
    if (enabled) {
        gb->cpu.f = (uint8_t)(gb->cpu.f | mask);
    } else {
        gb->cpu.f = (uint8_t)(gb->cpu.f & (uint8_t)~mask);
    }
    gb->cpu.f &= 0xF0;
}

/* Return whether a given flag bit is currently set. */
int pyemu_gameboy_get_flag(const pyemu_gameboy_system* gb, uint8_t mask) {
    return (gb->cpu.f & mask) != 0;
}

/* Convenience helper for instructions that compute all four visible flags at once. */
void pyemu_gameboy_set_flags_znhc(pyemu_gameboy_system* gb, int z, int n, int h, int c) {
    gb->cpu.f = 0;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, z);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, n);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, h);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, c);
}

/* Implement the 8-bit INC micro-op and update flags accordingly. */
uint8_t pyemu_gameboy_inc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value + 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((value & 0x0F) + 1) > 0x0F);
    return result;
}

/* Implement the 8-bit DEC micro-op and update flags accordingly. */
uint8_t pyemu_gameboy_dec8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value - 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, (value & 0x0F) == 0);
    return result;
}

/* Swap the upper and lower nibbles of an 8-bit value and update flags. */
uint8_t pyemu_gameboy_swap8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)((value << 4) | (value >> 4));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, 0);
    return result;
}

/* Implement RLCA on register A. */
void pyemu_gameboy_rlca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

/* Implement RRCA on register A. */
void pyemu_gameboy_rrca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

/* Implement RLA on register A. */
void pyemu_gameboy_rla(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

/* Implement RRA on register A. */
void pyemu_gameboy_rra(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

/* Rotate an 8-bit value left with carry-out feeding bit 0. */
uint8_t pyemu_gameboy_rlc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

/* Rotate an 8-bit value right with carry-out feeding bit 7. */
uint8_t pyemu_gameboy_rrc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

/* Rotate an 8-bit value left through the carry flag. */
uint8_t pyemu_gameboy_rl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

/* Rotate an 8-bit value right through the carry flag. */
uint8_t pyemu_gameboy_rr8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

/* Shift an 8-bit value left arithmetically, exposing bit 7 as carry. */
uint8_t pyemu_gameboy_sla8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)(value << 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

/* Shift an 8-bit value right arithmetically, preserving the sign bit. */
uint8_t pyemu_gameboy_sra8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (value & 0x80));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

/* Shift an 8-bit value right logically, clearing the new top bit. */
uint8_t pyemu_gameboy_srl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)(value >> 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

/* Implement ADD/ADC into A. */
void pyemu_gameboy_add_a(pyemu_gameboy_system* gb, uint8_t value, int with_carry) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = with_carry && pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1 : 0;
    uint16_t result = (uint16_t)a + value + carry;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, ((a & 0x0F) + (value & 0x0F) + carry) > 0x0F, result > 0xFF);
}

/* Implement AND into A. */
void pyemu_gameboy_and_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a & value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 1, 0);
}

/* Implement XOR into A. */
void pyemu_gameboy_xor_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a ^ value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

/* Implement OR into A. */
void pyemu_gameboy_or_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a | value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

/* Implement CP against A without storing the subtraction result. */
void pyemu_gameboy_cp_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

/* Implement SUB from A. */
void pyemu_gameboy_sub_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    gb->cpu.a = result;
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

/* Implement SBC from A. */
void pyemu_gameboy_sbc_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint16_t subtrahend = (uint16_t)value + carry;
    uint16_t result = (uint16_t)a - subtrahend;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 1, (a & 0x0F) < ((value & 0x0F) + carry), (uint16_t)a < subtrahend);
}

/* Implement ADD SP,e8 including the LR35902 half-carry/carry flag rules. */
void pyemu_gameboy_add_sp_signed(pyemu_gameboy_system* gb, int8_t value) {
    uint16_t sp = gb->cpu.sp;
    uint16_t result = (uint16_t)(sp + value);
    uint16_t unsigned_value = (uint16_t)(uint8_t)value;

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((sp & 0x0F) + (unsigned_value & 0x0F)) > 0x0F);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, ((sp & 0xFF) + (unsigned_value & 0xFF)) > 0xFF);
    gb->cpu.sp = result;
}

/* Compute SP+e8 using the same flag rules as LD HL,SP+e8. */
uint16_t pyemu_gameboy_sp_plus_signed(pyemu_gameboy_system* gb, int8_t value) {
    uint16_t sp = gb->cpu.sp;
    uint16_t result = (uint16_t)(sp + value);
    uint16_t unsigned_value = (uint16_t)(uint8_t)value;

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((sp & 0x0F) + (unsigned_value & 0x0F)) > 0x0F);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, ((sp & 0xFF) + (unsigned_value & 0xFF)) > 0xFF);
    return result;
}

/* Implement the CB BIT test helper. */
void pyemu_gameboy_bit_test(pyemu_gameboy_system* gb, uint8_t value, uint8_t bit) {
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, (value & (uint8_t)(1U << bit)) == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 1);
}

/* Implement ADD HL,rr. */
void pyemu_gameboy_add_hl(pyemu_gameboy_system* gb, uint16_t value) {
    uint16_t hl = pyemu_gameboy_get_hl(gb);
    uint32_t result = (uint32_t)hl + value;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((hl & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, result > 0xFFFF);
    pyemu_gameboy_set_hl(gb, (uint16_t)result);
}

/* Adjust A after BCD operations using the documented DMG DAA rules. */
void pyemu_gameboy_daa(pyemu_gameboy_system* gb) {
    uint8_t correction = 0;
    int carry = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C);

    if (!pyemu_gameboy_get_flag(gb, PYEMU_FLAG_N)) {
        if (carry || gb->cpu.a > 0x99) {
            correction |= 0x60;
            carry = 1;
        }
        if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_H) || (gb->cpu.a & 0x0F) > 0x09) {
            correction |= 0x06;
        }
        gb->cpu.a = (uint8_t)(gb->cpu.a + correction);
    } else {
        if (carry) {
            correction |= 0x60;
        }
        if (pyemu_gameboy_get_flag(gb, PYEMU_FLAG_H)) {
            correction |= 0x06;
        }
        gb->cpu.a = (uint8_t)(gb->cpu.a - correction);
    }

    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, gb->cpu.a == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, carry);
}
