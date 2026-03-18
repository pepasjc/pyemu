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

/* High-level lifecycle state for the emulator handle. */
typedef enum pyemu_run_state {
    PYEMU_RUN_STATE_STOPPED = 0,
    PYEMU_RUN_STATE_PAUSED = 1,
    PYEMU_RUN_STATE_RUNNING = 2
} pyemu_run_state;

/* Minimal CPU state exposed to debugger/frontends. */
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

/* Borrowed RGBA frame owned by the native core. */
typedef struct pyemu_frame_buffer {
    int width;
    int height;
    const uint8_t* rgba;
    size_t rgba_size;
} pyemu_frame_buffer;

/* Borrowed PCM16 stereo audio owned by the native core. */
typedef struct pyemu_audio_buffer {
    int sample_rate;
    int channels;
    const int16_t* samples;
    size_t sample_count;
} pyemu_audio_buffer;

typedef struct pyemu_gameboy_audio_debug_info {
    uint8_t nr10;
    uint8_t nr11;
    uint8_t nr12;
    uint8_t nr13;
    uint8_t nr14;
    uint8_t nr21;
    uint8_t nr22;
    uint8_t nr23;
    uint8_t nr24;
    uint8_t nr30;
    uint8_t nr32;
    uint8_t nr33;
    uint8_t nr34;
    uint8_t nr41;
    uint8_t nr42;
    uint8_t nr43;
    uint8_t nr44;
    uint8_t nr50;
    uint8_t nr51;
    uint8_t nr52;
    uint8_t ch1_enabled;
    uint8_t ch1_duty;
    uint8_t ch1_volume;
    uint8_t ch2_enabled;
    uint8_t ch2_duty;
    uint8_t ch2_volume;
    uint8_t ch3_enabled;
    uint8_t ch3_volume_code;
    uint8_t ch4_enabled;
    uint8_t ch4_volume;
    uint8_t ch4_width_mode;
    uint8_t ch4_divisor_code;
    uint8_t ch4_clock_shift;
    uint16_t ch1_frequency_raw;
    uint16_t ch2_frequency_raw;
    uint16_t ch3_frequency_raw;
    uint16_t ch4_lfsr;
} pyemu_gameboy_audio_debug_info;

/* Last meaningful bus access surfaced to the debugger. */
typedef struct pyemu_last_bus_access {
    uint16_t address;
    uint8_t value;
    uint8_t is_write;
    uint8_t valid;
} pyemu_last_bus_access;

/* Cartridge and mapper state surfaced for debugging. */
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

/* Generic multi-system entry points. */
PYEMU_API size_t pyemu_get_supported_system_count(void);
PYEMU_API const char* pyemu_get_supported_system_key(size_t index);
PYEMU_API const char* pyemu_get_default_system_key(void);
PYEMU_API pyemu_emulator* pyemu_create_emulator(const char* system_key);

/* Legacy convenience constructor for the first supported core. */
PYEMU_API pyemu_emulator* pyemu_create_gameboy(void);

/* Handle lifecycle and content loading. */
PYEMU_API void pyemu_destroy(pyemu_emulator* emulator);
PYEMU_API void pyemu_reset(pyemu_emulator* emulator);
PYEMU_API int pyemu_load_rom(pyemu_emulator* emulator, const char* path);
PYEMU_API int pyemu_save_state(pyemu_emulator* emulator, const char* path);
PYEMU_API int pyemu_load_state(pyemu_emulator* emulator, const char* path);

/* Coarse execution control. */
PYEMU_API void pyemu_run(pyemu_emulator* emulator);
PYEMU_API void pyemu_pause(pyemu_emulator* emulator);
PYEMU_API void pyemu_stop(pyemu_emulator* emulator);
PYEMU_API void pyemu_step_instruction(pyemu_emulator* emulator);
PYEMU_API void pyemu_step_frame(pyemu_emulator* emulator);
PYEMU_API pyemu_run_state pyemu_get_run_state(const pyemu_emulator* emulator);

/* Generic debugger/inspection surface. */
PYEMU_API const char* pyemu_get_system_name(const pyemu_emulator* emulator);
PYEMU_API pyemu_cpu_state pyemu_get_cpu_state(const pyemu_emulator* emulator);
PYEMU_API pyemu_frame_buffer pyemu_get_frame_buffer(const pyemu_emulator* emulator);
PYEMU_API pyemu_audio_buffer pyemu_get_audio_buffer(const pyemu_emulator* emulator);
PYEMU_API const uint8_t* pyemu_get_memory(const pyemu_emulator* emulator, size_t* size);
PYEMU_API void pyemu_poke_memory(pyemu_emulator* emulator, uint16_t address, uint8_t value);
PYEMU_API int pyemu_has_rom_loaded(const pyemu_emulator* emulator);
PYEMU_API const char* pyemu_get_rom_path(const pyemu_emulator* emulator);
PYEMU_API const char* pyemu_get_cartridge_title(const pyemu_emulator* emulator);
PYEMU_API size_t pyemu_get_rom_size(const pyemu_emulator* emulator);
PYEMU_API uint64_t pyemu_get_cycle_count(const pyemu_emulator* emulator);
PYEMU_API pyemu_last_bus_access pyemu_get_last_bus_access(const pyemu_emulator* emulator);
PYEMU_API pyemu_cartridge_debug_info pyemu_get_cartridge_debug_info(const pyemu_emulator* emulator);
PYEMU_API int pyemu_is_faulted(const pyemu_emulator* emulator);

/* Current Game Boy-specific helpers used by the debugger. */
PYEMU_API pyemu_audio_buffer pyemu_get_gameboy_audio_channel_buffer(const pyemu_emulator* emulator, int channel);
PYEMU_API int pyemu_get_gameboy_audio_debug_info(const pyemu_emulator* emulator, pyemu_gameboy_audio_debug_info* out_info);
PYEMU_API void pyemu_set_gameboy_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions);
PYEMU_API void pyemu_set_bus_tracking(pyemu_emulator* emulator, int enabled);
