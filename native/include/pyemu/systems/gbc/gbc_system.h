#pragma once

#include "pyemu/core/emulator.h"
#include "pyemu/core/system.h"

/* Game Boy Color core entry point registered under the "gbc" system key. */
pyemu_system* pyemu_gbc_create(void);

/* GBC-specific debugger/input helpers. */
pyemu_audio_buffer pyemu_gbc_get_audio_channel_buffer(const pyemu_system* system, int channel);
void pyemu_gbc_get_audio_debug_info(const pyemu_system* system, pyemu_gameboy_audio_debug_info* out_info);
void pyemu_gbc_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions);
void pyemu_gbc_set_bus_tracking(pyemu_system* system, int enabled);
