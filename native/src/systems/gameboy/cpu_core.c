#include "gameboy_internal.h"

/* CPU fetch, stack, and interrupt plumbing shared by the opcode engine and hotpath executor. */

/* Fetch the next opcode/immediate byte, including the DMG halt-bug PC quirk. */
uint8_t pyemu_gameboy_fetch_u8(pyemu_gameboy_system* gb) {
    uint8_t value = pyemu_gameboy_peek_memory(gb, gb->cpu.pc);
    if (gb->halt_bug) {
        gb->halt_bug = 0;
    } else {
        gb->cpu.pc = (uint16_t)(gb->cpu.pc + 1);
    }
    return value;
}

/* Fetch a little-endian 16-bit immediate using the same PC rules as byte fetches. */
uint16_t pyemu_gameboy_fetch_u16(pyemu_gameboy_system* gb) {
    uint8_t low = pyemu_gameboy_fetch_u8(gb);
    uint8_t high = pyemu_gameboy_fetch_u8(gb);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

/* Push a 16-bit value to the emulated stack in LR35902 byte order. */
void pyemu_gameboy_push_u16(pyemu_gameboy_system* gb, uint16_t value) {
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    pyemu_gameboy_write_memory(gb, gb->cpu.sp, (uint8_t)(value >> 8));
    gb->cpu.sp = (uint16_t)(gb->cpu.sp - 1);
    pyemu_gameboy_write_memory(gb, gb->cpu.sp, (uint8_t)(value & 0xFF));
}

/* Pop a 16-bit value from the emulated stack in LR35902 byte order. */
uint16_t pyemu_gameboy_pop_u16(pyemu_gameboy_system* gb) {
    uint8_t low = pyemu_gameboy_read_memory(gb, gb->cpu.sp);
    uint8_t high;
    gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
    high = pyemu_gameboy_read_memory(gb, gb->cpu.sp);
    gb->cpu.sp = (uint16_t)(gb->cpu.sp + 1);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

/* Raise one or more interrupt request bits in IF while preserving the unused high bits. */
void pyemu_gameboy_request_interrupt(pyemu_gameboy_system* gb, uint8_t mask) {
    gb->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gb->memory[PYEMU_IO_IF] | mask) & 0x1F));
}

/* Service the highest-priority pending interrupt, including HALT wakeup and vector dispatch timing. */
int pyemu_gameboy_service_interrupts(pyemu_gameboy_system* gb) {
    uint8_t pending = pyemu_gameboy_pending_interrupts(gb);
    uint8_t mask = 0;
    uint16_t vector = 0;

    if (!pyemu_gameboy_lcd_enabled(gb)) {
        pending = (uint8_t)(pending & (uint8_t)~(PYEMU_INTERRUPT_VBLANK | PYEMU_INTERRUPT_LCD));
    }

    if (pending == 0) {
        return 0;
    }

    if (gb->cpu.halted) {
        gb->cpu.halted = 0;
    }

    if (!gb->ime) {
        return 0;
    }

    if (pending & PYEMU_INTERRUPT_VBLANK) {
        mask = PYEMU_INTERRUPT_VBLANK;
        vector = 0x0040;
    } else if (pending & PYEMU_INTERRUPT_LCD) {
        mask = PYEMU_INTERRUPT_LCD;
        vector = 0x0048;
    } else if (pending & PYEMU_INTERRUPT_TIMER) {
        mask = PYEMU_INTERRUPT_TIMER;
        vector = 0x0050;
    } else if (pending & PYEMU_INTERRUPT_SERIAL) {
        mask = PYEMU_INTERRUPT_SERIAL;
        vector = 0x0058;
    } else if (pending & PYEMU_INTERRUPT_JOYPAD) {
        mask = PYEMU_INTERRUPT_JOYPAD;
        vector = 0x0060;
    }

    if (mask == 0) {
        return 0;
    }

    gb->ime = 0;
    gb->ime_pending = 0;
    gb->ime_delay = 0;
    gb->memory[PYEMU_IO_IF] = (uint8_t)(0xE0 | ((gb->memory[PYEMU_IO_IF] & (uint8_t)~mask) & 0x1F));
    pyemu_gameboy_push_u16(gb, gb->cpu.pc);
    gb->cpu.pc = vector;
    pyemu_gameboy_tick(gb, 20);
    return 20;
}
