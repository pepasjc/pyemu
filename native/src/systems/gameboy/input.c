#include "gameboy_internal.h"

/* Joypad helpers for the DMG core. These keep host input, FF00 selection bits, and joypad interrupt edges in one place. */

#include <string.h>

/* Synthesize the current FF00 value from the selected input rows and the latched host button state. */
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

/* Recompute FF00 after an input or row-selection change and request a joypad interrupt on falling edges. */
void pyemu_gameboy_refresh_joypad(pyemu_gameboy_system* gb, uint8_t previous_value) {
    uint8_t current_value = pyemu_gameboy_current_joypad_value(gb);
    uint8_t falling_edges = (uint8_t)((previous_value ^ current_value) & previous_value & 0x0F);

    gb->memory[0xFF00] = (uint8_t)(0xC0 | (gb->memory[0xFF00] & 0x30) | 0x0F);
    if (falling_edges != 0) {
        pyemu_gameboy_request_interrupt(gb, PYEMU_INTERRUPT_JOYPAD);
    }
}

/* Entry point used by the frontend to update the pressed-button masks for this frame. */
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

/* Allow the debugger to disable expensive bus tracking while the emulator is running normally. */
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
