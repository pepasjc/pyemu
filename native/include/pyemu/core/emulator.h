#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #ifdef PYEMU_BUILD_DLL
    #define PYEMU_API __declspec(dllexport)
  #else
    #define PYEMU_API __declspec(dllimport)
  #endif
#else
  #define PYEMU_API
#endif

typedef enum pyemu_run_state {
    PYEMU_RUN_STATE_STOPPED = 0,
    PYEMU_RUN_STATE_PAUSED = 1,
    PYEMU_RUN_STATE_RUNNING = 2
} pyemu_run_state;

typedef struct pyemu_cpu_state {
    uint16_t pc;
    uint16_t sp;
    uint8_t a;
    uint8_t f;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
    uint8_t h;
    uint8_t l;
    uint8_t halted;
    uint8_t ime;
} pyemu_cpu_state;

typedef struct pyemu_frame_buffer {
    int width;
    int height;
    const uint8_t* rgba;
    size_t rgba_size;
} pyemu_frame_buffer;

typedef struct pyemu_last_bus_access {
    uint16_t address;
    uint8_t value;
    uint8_t is_write;
    uint8_t valid;
} pyemu_last_bus_access;

typedef struct pyemu_cartridge_debug_info {
    uint8_t cartridge_type;
    uint8_t rom_size_code;
    uint8_t ram_size_code;
    uint8_t ram_enabled;
    uint8_t rom_bank;
    uint8_t ram_bank;
    uint8_t banking_mode;
    uint8_t has_battery;
    uint8_t save_file_present;
    uint8_t last_mapper_value;
    uint8_t last_mapper_valid;
    uint8_t reserved[2];
    uint16_t last_mapper_address;
    uint32_t rom_bank_count;
    uint32_t ram_bank_count;
} pyemu_cartridge_debug_info;

typedef struct pyemu_emulator pyemu_emulator;

PYEMU_API size_t pyemu_get_supported_system_count(void);
PYEMU_API const char* pyemu_get_supported_system_key(size_t index);
PYEMU_API const char* pyemu_get_default_system_key(void);
PYEMU_API pyemu_emulator* pyemu_create_emulator(const char* system_key);
PYEMU_API pyemu_emulator* pyemu_create_gameboy(void);
PYEMU_API void pyemu_destroy(pyemu_emulator* emulator);
PYEMU_API void pyemu_reset(pyemu_emulator* emulator);
PYEMU_API int pyemu_load_rom(pyemu_emulator* emulator, const char* path);
PYEMU_API int pyemu_save_state(pyemu_emulator* emulator, const char* path);
PYEMU_API int pyemu_load_state(pyemu_emulator* emulator, const char* path);
PYEMU_API void pyemu_run(pyemu_emulator* emulator);
PYEMU_API void pyemu_pause(pyemu_emulator* emulator);
PYEMU_API void pyemu_stop(pyemu_emulator* emulator);
PYEMU_API void pyemu_step_instruction(pyemu_emulator* emulator);
PYEMU_API void pyemu_step_frame(pyemu_emulator* emulator);
PYEMU_API pyemu_run_state pyemu_get_run_state(const pyemu_emulator* emulator);
PYEMU_API const char* pyemu_get_system_name(const pyemu_emulator* emulator);
PYEMU_API pyemu_cpu_state pyemu_get_cpu_state(const pyemu_emulator* emulator);
PYEMU_API pyemu_frame_buffer pyemu_get_frame_buffer(const pyemu_emulator* emulator);
PYEMU_API const uint8_t* pyemu_get_memory(const pyemu_emulator* emulator, size_t* size);
PYEMU_API int pyemu_has_rom_loaded(const pyemu_emulator* emulator);
PYEMU_API const char* pyemu_get_rom_path(const pyemu_emulator* emulator);
PYEMU_API const char* pyemu_get_cartridge_title(const pyemu_emulator* emulator);
PYEMU_API size_t pyemu_get_rom_size(const pyemu_emulator* emulator);
PYEMU_API uint64_t pyemu_get_cycle_count(const pyemu_emulator* emulator);
PYEMU_API pyemu_last_bus_access pyemu_get_last_bus_access(const pyemu_emulator* emulator);
PYEMU_API pyemu_cartridge_debug_info pyemu_get_cartridge_debug_info(const pyemu_emulator* emulator);
PYEMU_API int pyemu_is_faulted(const pyemu_emulator* emulator);
PYEMU_API void pyemu_set_gameboy_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions);
PYEMU_API void pyemu_set_bus_tracking(pyemu_emulator* emulator, int enabled);
