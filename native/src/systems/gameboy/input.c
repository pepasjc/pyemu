#include "gameboy_internal.h"

#include <string.h>

uint8_t pyemu_gameboy_current_joypad_value(const pyemu_gameboy_system* gb) {
    uint8_t select = (uint8_t)(gb->memory[0xFF00] & 0x30);
    uint8_t low = 0x0F;

    if ((select & 0x10) == 0) {
        low &= gb->joypad_directions;
    }
    if ((select & 0x20) == 0) {
        low &= gb->joypad_buttons;
    }

    return (uint8_t)(0xC0 | select | low);
}

void pyemu_gameboy_refresh_joypad(pyemu_gameboy_system* gb, uint8_t previous_value) {
    uint8_t current_value = pyemu_gameboy_current_joypad_value(gb);
    uint8_t falling_edges = (uint8_t)((previous_value ^ current_value) & previous_value & 0x0F);

    gb->memory[0xFF00] = (uint8_t)(0xC0 | (gb->memory[0xFF00] & 0x30) | 0x0F);
    if (falling_edges != 0) {
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_JOYPAD);
    }
}

void pyemu_gameboy_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    uint8_t previous_value;
    if (gb == NULL) {
        return;
    }
    previous_value = pyemu_gameboy_current_joypad_value(gb);
    gb->joypad_buttons = (uint8_t)(buttons & 0x0F);
    gb->joypad_directions = (uint8_t)(directions & 0x0F);
    pyemu_gameboy_refresh_joypad(gb, previous_value);
}

void pyemu_gameboy_set_bus_tracking(pyemu_system* system, int enabled) {
    pyemu_gameboy_system* gb = (pyemu_gameboy_system*)system;
    if (gb == NULL) {
        return;
    }
    gb->bus_tracking_enabled = enabled ? 1 : 0;
    if (!gb->bus_tracking_enabled) {
        memset(&gb->last_access, 0, sizeof(gb->last_access));
    }
}
