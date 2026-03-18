#include "gameboy_internal.h"

uint16_t pyemu_gameboy_get_af(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.a << 8) | (gb->cpu.f & 0xF0));
}

uint16_t pyemu_gameboy_get_bc(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.b << 8) | gb->cpu.c);
}

uint16_t pyemu_gameboy_get_de(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.d << 8) | gb->cpu.e);
}

uint16_t pyemu_gameboy_get_hl(const pyemu_gameboy_system* gb) {
    return (uint16_t)(((uint16_t)gb->cpu.h << 8) | gb->cpu.l);
}

void pyemu_gameboy_set_af(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.a = (uint8_t)(value >> 8);
    gb->cpu.f = (uint8_t)(value & 0xF0);
}

void pyemu_gameboy_set_bc(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.b = (uint8_t)(value >> 8);
    gb->cpu.c = (uint8_t)(value & 0xFF);
}

void pyemu_gameboy_set_de(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.d = (uint8_t)(value >> 8);
    gb->cpu.e = (uint8_t)(value & 0xFF);
}

void pyemu_gameboy_set_hl(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.h = (uint8_t)(value >> 8);
    gb->cpu.l = (uint8_t)(value & 0xFF);
}

void pyemu_gameboy_set_flag(pyemu_gameboy_system* gb, uint8_t mask, int enabled) {
    if (enabled) {
        gb->cpu.f = (uint8_t)(gb->cpu.f | mask);
    } else {
        gb->cpu.f = (uint8_t)(gb->cpu.f & (uint8_t)~mask);
    }
    gb->cpu.f &= 0xF0;
}

int pyemu_gameboy_get_flag(const pyemu_gameboy_system* gb, uint8_t mask) {
    return (gb->cpu.f & mask) != 0;
}

void pyemu_gameboy_set_flags_znhc(pyemu_gameboy_system* gb, int z, int n, int h, int c) {
    gb->cpu.f = 0;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, z);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, n);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, h);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, c);
}

uint8_t pyemu_gameboy_inc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value + 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((value & 0x0F) + 1) > 0x0F);
    return result;
}

uint8_t pyemu_gameboy_dec8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)(value - 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, result == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 1);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, (value & 0x0F) == 0);
    return result;
}

uint8_t pyemu_gameboy_swap8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t result = (uint8_t)((value << 4) | (value >> 4));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, 0);
    return result;
}

void pyemu_gameboy_rlca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

void pyemu_gameboy_rrca(pyemu_gameboy_system* gb) {
    uint8_t carry = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry != 0);
}

void pyemu_gameboy_rla(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a >> 7);
    gb->cpu.a = (uint8_t)((gb->cpu.a << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

void pyemu_gameboy_rra(pyemu_gameboy_system* gb) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(gb->cpu.a & 0x01);
    gb->cpu.a = (uint8_t)((gb->cpu.a >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, 0, 0, 0, carry_out != 0);
}

uint8_t pyemu_gameboy_rlc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

uint8_t pyemu_gameboy_rrc8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (carry << 7));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

uint8_t pyemu_gameboy_rl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint8_t carry_out = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)((value << 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

uint8_t pyemu_gameboy_rr8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry_in = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 0x80U : 0U;
    uint8_t carry_out = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | carry_in);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry_out != 0);
    return result;
}

uint8_t pyemu_gameboy_sla8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value >> 7);
    uint8_t result = (uint8_t)(value << 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

uint8_t pyemu_gameboy_sra8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)((value >> 1) | (value & 0x80));
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

uint8_t pyemu_gameboy_srl8(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t carry = (uint8_t)(value & 0x01);
    uint8_t result = (uint8_t)(value >> 1);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 0, 0, carry != 0);
    return result;
}

void pyemu_gameboy_add_a(pyemu_gameboy_system* gb, uint8_t value, int with_carry) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = with_carry && pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1 : 0;
    uint16_t result = (uint16_t)a + value + carry;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, ((a & 0x0F) + (value & 0x0F) + carry) > 0x0F, result > 0xFF);
}

void pyemu_gameboy_and_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a & value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 1, 0);
}

void pyemu_gameboy_xor_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a ^ value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

void pyemu_gameboy_or_a(pyemu_gameboy_system* gb, uint8_t value) {
    gb->cpu.a = (uint8_t)(gb->cpu.a | value);
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 0, 0, 0);
}

void pyemu_gameboy_cp_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

void pyemu_gameboy_sub_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t result = (uint8_t)(a - value);
    gb->cpu.a = result;
    pyemu_gameboy_set_flags_znhc(gb, result == 0, 1, (a & 0x0F) < (value & 0x0F), a < value);
}

void pyemu_gameboy_sbc_a(pyemu_gameboy_system* gb, uint8_t value) {
    uint8_t a = gb->cpu.a;
    uint8_t carry = pyemu_gameboy_get_flag(gb, PYEMU_FLAG_C) ? 1U : 0U;
    uint16_t subtrahend = (uint16_t)value + carry;
    uint16_t result = (uint16_t)a - subtrahend;
    gb->cpu.a = (uint8_t)result;
    pyemu_gameboy_set_flags_znhc(gb, gb->cpu.a == 0, 1, (a & 0x0F) < ((value & 0x0F) + carry), (uint16_t)a < subtrahend);
}

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

void pyemu_gameboy_bit_test(pyemu_gameboy_system* gb, uint8_t value, uint8_t bit) {
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_Z, (value & (uint8_t)(1U << bit)) == 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, 1);
}

void pyemu_gameboy_add_hl(pyemu_gameboy_system* gb, uint16_t value) {
    uint16_t hl = pyemu_gameboy_get_hl(gb);
    uint32_t result = (uint32_t)hl + value;
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_N, 0);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_H, ((hl & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF);
    pyemu_gameboy_set_flag(gb, PYEMU_FLAG_C, result > 0xFFFF);
    pyemu_gameboy_set_hl(gb, (uint16_t)result);
}

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
