#include "pyemu/core/emulator.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pyemu/core/system.h"
#include "pyemu/systems/gameboy/gameboy_system.h"

struct pyemu_emulator {
    pyemu_system* system;
    pyemu_run_state run_state;
};

typedef struct pyemu_system_descriptor {
    const char* key;
    pyemu_system* (*create)(void);
} pyemu_system_descriptor;

static const pyemu_system_descriptor PYEMU_SYSTEMS[] = {
    {"gameboy", pyemu_gameboy_create}
};

static const size_t PYEMU_SYSTEM_COUNT = sizeof(PYEMU_SYSTEMS) / sizeof(PYEMU_SYSTEMS[0]);

static const pyemu_system_descriptor* pyemu_find_system_descriptor(const char* system_key) {
    size_t index;
    const char* requested = system_key;

    if (requested == NULL || requested[0] == '\0') {
        requested = PYEMU_SYSTEMS[0].key;
    }

    for (index = 0; index < PYEMU_SYSTEM_COUNT; ++index) {
        if (strcmp(PYEMU_SYSTEMS[index].key, requested) == 0) {
            return &PYEMU_SYSTEMS[index];
        }
    }

    return NULL;
}

static pyemu_cpu_state pyemu_zero_cpu_state(void) {
    pyemu_cpu_state state;
    state.pc = 0;
    state.sp = 0;
    state.a = 0;
    state.f = 0;
    state.b = 0;
    state.c = 0;
    state.d = 0;
    state.e = 0;
    state.h = 0;
    state.l = 0;
    state.halted = 0;
    state.ime = 0;
    return state;
}

static pyemu_last_bus_access pyemu_zero_last_bus_access(void) {
    pyemu_last_bus_access access;
    access.address = 0;
    access.value = 0;
    access.is_write = 0;
    access.valid = 0;
    return access;
}

static pyemu_cartridge_debug_info pyemu_zero_cartridge_debug_info(void) {
    pyemu_cartridge_debug_info info;
    memset(&info, 0, sizeof(info));
    return info;
}

static pyemu_frame_buffer pyemu_zero_frame_buffer(void) {
    pyemu_frame_buffer frame;
    frame.width = 0;
    frame.height = 0;
    frame.rgba = NULL;
    frame.rgba_size = 0;
    return frame;
}

static pyemu_audio_buffer pyemu_zero_audio_buffer(void) {
    pyemu_audio_buffer audio;
    audio.sample_rate = 0;
    audio.channels = 0;
    audio.samples = NULL;
    audio.sample_count = 0;
    return audio;
}

PYEMU_API size_t pyemu_get_supported_system_count(void) {
    return PYEMU_SYSTEM_COUNT;
}

PYEMU_API const char* pyemu_get_supported_system_key(size_t index) {
    if (index >= PYEMU_SYSTEM_COUNT) {
        return "";
    }
    return PYEMU_SYSTEMS[index].key;
}

PYEMU_API const char* pyemu_get_default_system_key(void) {
    return PYEMU_SYSTEMS[0].key;
}

PYEMU_API pyemu_emulator* pyemu_create_emulator(const char* system_key) {
    const pyemu_system_descriptor* descriptor = pyemu_find_system_descriptor(system_key);
    pyemu_emulator* emulator;

    if (descriptor == NULL) {
        return NULL;
    }

    emulator = (pyemu_emulator*)calloc(1, sizeof(pyemu_emulator));
    if (emulator == NULL) {
        return NULL;
    }

    emulator->system = descriptor->create();
    if (emulator->system == NULL) {
        free(emulator);
        return NULL;
    }

    emulator->run_state = PYEMU_RUN_STATE_STOPPED;
    return emulator;
}

PYEMU_API pyemu_emulator* pyemu_create_gameboy(void) {
    return pyemu_create_emulator("gameboy");
}

PYEMU_API void pyemu_destroy(pyemu_emulator* emulator) {
    if (emulator == NULL) {
        return;
    }
    if (emulator->system != NULL && emulator->system->vtable != NULL && emulator->system->vtable->destroy != NULL) {
        emulator->system->vtable->destroy(emulator->system);
    }
    free(emulator);
}

PYEMU_API void pyemu_reset(pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return;
    }
    emulator->system->vtable->reset(emulator->system);
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
}

PYEMU_API int pyemu_load_rom(pyemu_emulator* emulator, const char* path) {
    if (emulator == NULL || emulator->system == NULL) {
        return 0;
    }
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
    return emulator->system->vtable->load_rom(emulator->system, path);
}

PYEMU_API int pyemu_save_state(pyemu_emulator* emulator, const char* path) {
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->save_state == NULL) {
        return 0;
    }
    return emulator->system->vtable->save_state(emulator->system, path);
}

PYEMU_API int pyemu_load_state(pyemu_emulator* emulator, const char* path) {
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->load_state == NULL) {
        return 0;
    }
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
    return emulator->system->vtable->load_state(emulator->system, path);
}

PYEMU_API void pyemu_run(pyemu_emulator* emulator) {
    if (emulator == NULL) {
        return;
    }
    emulator->run_state = PYEMU_RUN_STATE_RUNNING;
}

PYEMU_API void pyemu_pause(pyemu_emulator* emulator) {
    if (emulator == NULL) {
        return;
    }
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
}

PYEMU_API void pyemu_stop(pyemu_emulator* emulator) {
    if (emulator == NULL) {
        return;
    }
    pyemu_reset(emulator);
    emulator->run_state = PYEMU_RUN_STATE_STOPPED;
}

PYEMU_API void pyemu_step_instruction(pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return;
    }
    emulator->system->vtable->step_instruction(emulator->system);
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
}

PYEMU_API void pyemu_step_frame(pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return;
    }
    emulator->system->vtable->step_frame(emulator->system);
    emulator->run_state = PYEMU_RUN_STATE_PAUSED;
}

PYEMU_API pyemu_run_state pyemu_get_run_state(const pyemu_emulator* emulator) {
    if (emulator == NULL) {
        return PYEMU_RUN_STATE_STOPPED;
    }
    return emulator->run_state;
}

PYEMU_API const char* pyemu_get_system_name(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return "unknown";
    }
    return emulator->system->vtable->name(emulator->system);
}

PYEMU_API pyemu_cpu_state pyemu_get_cpu_state(const pyemu_emulator* emulator) {
    pyemu_cpu_state state = pyemu_zero_cpu_state();
    if (emulator == NULL || emulator->system == NULL) {
        return state;
    }
    emulator->system->vtable->get_cpu_state(emulator->system, &state);
    return state;
}

PYEMU_API pyemu_frame_buffer pyemu_get_frame_buffer(const pyemu_emulator* emulator) {
    pyemu_frame_buffer frame = pyemu_zero_frame_buffer();
    if (emulator == NULL || emulator->system == NULL) {
        return frame;
    }
    emulator->system->vtable->get_frame_buffer(emulator->system, &frame);
    return frame;
}

PYEMU_API pyemu_audio_buffer pyemu_get_audio_buffer(const pyemu_emulator* emulator) {
    pyemu_audio_buffer audio = pyemu_zero_audio_buffer();
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->get_audio_buffer == NULL) {
        return audio;
    }
    emulator->system->vtable->get_audio_buffer(emulator->system, &audio);
    return audio;
}

PYEMU_API const uint8_t* pyemu_get_memory(const pyemu_emulator* emulator, size_t* size) {
    if (size != NULL) {
        *size = 0;
    }
    if (emulator == NULL || emulator->system == NULL) {
        return NULL;
    }
    return emulator->system->vtable->get_memory(emulator->system, size);
}

PYEMU_API void pyemu_poke_memory(pyemu_emulator* emulator, uint16_t address, uint8_t value) {
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->poke_memory == NULL) {
        return;
    }
    emulator->system->vtable->poke_memory(emulator->system, address, value);
}

PYEMU_API int pyemu_has_rom_loaded(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return 0;
    }
    return emulator->system->vtable->has_rom_loaded(emulator->system);
}

PYEMU_API const char* pyemu_get_rom_path(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return "";
    }
    return emulator->system->vtable->get_rom_path(emulator->system);
}

PYEMU_API const char* pyemu_get_cartridge_title(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return "";
    }
    return emulator->system->vtable->get_cartridge_title(emulator->system);
}

PYEMU_API size_t pyemu_get_rom_size(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return 0;
    }
    return emulator->system->vtable->get_rom_size(emulator->system);
}

PYEMU_API uint64_t pyemu_get_cycle_count(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL) {
        return 0;
    }
    return emulator->system->vtable->get_cycle_count(emulator->system);
}

PYEMU_API pyemu_audio_buffer pyemu_get_gameboy_audio_channel_buffer(const pyemu_emulator* emulator, int channel) {
    if (emulator == NULL || emulator->system == NULL) {
        return pyemu_zero_audio_buffer();
    }
    if (strcmp(emulator->system->vtable->name(emulator->system), "gameboy") != 0) {
        return pyemu_zero_audio_buffer();
    }
    return pyemu_gameboy_get_audio_channel_buffer(emulator->system, channel);
}

PYEMU_API int pyemu_get_gameboy_audio_debug_info(const pyemu_emulator* emulator, pyemu_gameboy_audio_debug_info* out_info) {
    if (out_info == NULL) {
        return 0;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (emulator == NULL || emulator->system == NULL) {
        return 0;
    }
    if (strcmp(emulator->system->vtable->name(emulator->system), "gameboy") != 0) {
        return 0;
    }
    pyemu_gameboy_get_audio_debug_info(emulator->system, out_info);
    return 1;
}

PYEMU_API void pyemu_set_gameboy_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions) {
    if (emulator == NULL || emulator->system == NULL) {
        return;
    }
    if (strcmp(emulator->system->vtable->name(emulator->system), "gameboy") != 0) {
        return;
    }
    pyemu_gameboy_set_joypad_state(emulator->system, buttons, directions);
}

PYEMU_API void pyemu_set_bus_tracking(pyemu_emulator* emulator, int enabled) {
    if (emulator == NULL || emulator->system == NULL) {
        return;
    }
    if (strcmp(emulator->system->vtable->name(emulator->system), "gameboy") != 0) {
        return;
    }
    pyemu_gameboy_set_bus_tracking(emulator->system, enabled);
}

PYEMU_API int pyemu_is_faulted(const pyemu_emulator* emulator) {
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->is_faulted == NULL) {
        return 0;
    }
    return emulator->system->vtable->is_faulted(emulator->system);
}

PYEMU_API pyemu_last_bus_access pyemu_get_last_bus_access(const pyemu_emulator* emulator) {
    pyemu_last_bus_access access = pyemu_zero_last_bus_access();
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->get_last_bus_access == NULL) {
        return access;
    }
    emulator->system->vtable->get_last_bus_access(emulator->system, &access);
    return access;
}

PYEMU_API pyemu_cartridge_debug_info pyemu_get_cartridge_debug_info(const pyemu_emulator* emulator) {
    pyemu_cartridge_debug_info info = pyemu_zero_cartridge_debug_info();
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->get_cartridge_debug_info == NULL) {
        return info;
    }
    emulator->system->vtable->get_cartridge_debug_info(emulator->system, &info);
    return info;
}
