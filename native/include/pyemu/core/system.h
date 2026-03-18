#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pyemu/core/emulator.h"

typedef struct pyemu_system pyemu_system;

/*
 * Per-system contract used by the generic emulator handle.
 *
 * A new core becomes available to the rest of the project by implementing this
 * vtable and registering a create function in native/src/core/emulator.c.
 */
typedef struct pyemu_system_vtable {
    const char* (*name)(const pyemu_system* system);
    void (*reset)(pyemu_system* system);
    int (*load_rom)(pyemu_system* system, const char* path);
    int (*save_state)(const pyemu_system* system, const char* path);
    int (*load_state)(pyemu_system* system, const char* path);
    void (*step_instruction)(pyemu_system* system);
    void (*step_frame)(pyemu_system* system);
    void (*destroy)(pyemu_system* system);
    void (*get_cpu_state)(const pyemu_system* system, pyemu_cpu_state* out_state);
    void (*get_frame_buffer)(const pyemu_system* system, pyemu_frame_buffer* out_frame);
    void (*get_audio_buffer)(const pyemu_system* system, pyemu_audio_buffer* out_audio);
    const uint8_t* (*get_memory)(const pyemu_system* system, size_t* size);
    void (*poke_memory)(pyemu_system* system, uint16_t address, uint8_t value);
    int (*has_rom_loaded)(const pyemu_system* system);
    const char* (*get_rom_path)(const pyemu_system* system);
    const char* (*get_cartridge_title)(const pyemu_system* system);
    size_t (*get_rom_size)(const pyemu_system* system);
    uint64_t (*get_cycle_count)(const pyemu_system* system);
    void (*get_last_bus_access)(const pyemu_system* system, pyemu_last_bus_access* out_access);
    void (*get_cartridge_debug_info)(const pyemu_system* system, pyemu_cartridge_debug_info* out_info);
    int (*is_faulted)(const pyemu_system* system);
} pyemu_system_vtable;

struct pyemu_system {
    const pyemu_system_vtable* vtable;
};
