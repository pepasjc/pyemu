#pragma once

#include "pyemu/core/emulator.h"
#include "pyemu/core/system.h"

pyemu_system* pyemu_gameboy_create(void);
void pyemu_gameboy_set_joypad_state(pyemu_system* system, uint8_t buttons, uint8_t directions);
void pyemu_gameboy_set_bus_tracking(pyemu_system* system, int enabled);
