#include "pyemu/core/emulator.h"
#include "pyemu/core/plugin.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE pyemu_core_module_handle;
#else
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
typedef void* pyemu_core_module_handle;
#endif

struct pyemu_emulator {
    pyemu_system* system;
    pyemu_run_state run_state;
    const pyemu_core_plugin_descriptor* plugin;
};

typedef struct pyemu_loaded_core {
    pyemu_core_module_handle module;
    const pyemu_core_plugin_descriptor* descriptor;
} pyemu_loaded_core;

#define PYEMU_MAX_CORES 32
#define PYEMU_PLUGIN_DIR_BUFFER 1024

static pyemu_loaded_core PYEMU_LOADED_CORES[PYEMU_MAX_CORES];
static size_t PYEMU_LOADED_CORE_COUNT = 0;
static int PYEMU_CORE_SCAN_COMPLETE = 0;

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

static int pyemu_has_loaded_core_key(const char* key) {
    size_t index;
    for (index = 0; index < PYEMU_LOADED_CORE_COUNT; ++index) {
        if (strcmp(PYEMU_LOADED_CORES[index].descriptor->key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static void pyemu_register_loaded_core(pyemu_core_module_handle module, const pyemu_core_plugin_descriptor* descriptor) {
    if (descriptor == NULL || descriptor->key == NULL || descriptor->key[0] == '\0' || descriptor->create == NULL) {
        return;
    }
    if (PYEMU_LOADED_CORE_COUNT >= PYEMU_MAX_CORES || pyemu_has_loaded_core_key(descriptor->key)) {
        return;
    }
    PYEMU_LOADED_CORES[PYEMU_LOADED_CORE_COUNT].module = module;
    PYEMU_LOADED_CORES[PYEMU_LOADED_CORE_COUNT].descriptor = descriptor;
    PYEMU_LOADED_CORE_COUNT += 1;
}

#if defined(_WIN32)
static int pyemu_get_host_directory(char* out_directory, size_t out_size) {
    HMODULE module = NULL;
    DWORD length;
    char path[PYEMU_PLUGIN_DIR_BUFFER];
    char* slash;

    if (out_directory == NULL || out_size == 0) {
        return 0;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&pyemu_get_supported_system_count,
            &module)) {
        return 0;
    }
    length = GetModuleFileNameA(module, path, (DWORD)sizeof(path));
    if (length == 0 || length >= sizeof(path)) {
        return 0;
    }
    slash = strrchr(path, '\\');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    strncpy(out_directory, path, out_size - 1);
    out_directory[out_size - 1] = '\0';
    return 1;
}

static void pyemu_try_load_core_library(const char* full_path) {
    pyemu_core_module_handle module;
    pyemu_get_core_plugin_fn get_plugin;
    const pyemu_core_plugin_descriptor* descriptor;

    module = LoadLibraryA(full_path);
    if (module == NULL) {
        return;
    }
    get_plugin = (pyemu_get_core_plugin_fn)GetProcAddress(module, PYEMU_CORE_PLUGIN_EXPORT_NAME);
    if (get_plugin == NULL) {
        FreeLibrary(module);
        return;
    }
    descriptor = get_plugin();
    if (descriptor == NULL || descriptor->key == NULL || descriptor->create == NULL || pyemu_has_loaded_core_key(descriptor->key)) {
        FreeLibrary(module);
        return;
    }
    pyemu_register_loaded_core(module, descriptor);
}

static void pyemu_scan_core_directory(const char* directory) {
    char pattern[PYEMU_PLUGIN_DIR_BUFFER];
    WIN32_FIND_DATAA find_data;
    HANDLE handle;

    if (snprintf(pattern, sizeof(pattern), "%s\\pyemu_core_*.dll", directory) < 0) {
        return;
    }
    handle = FindFirstFileA(pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            char full_path[PYEMU_PLUGIN_DIR_BUFFER];
            if (snprintf(full_path, sizeof(full_path), "%s\\%s", directory, find_data.cFileName) >= 0) {
                pyemu_try_load_core_library(full_path);
            }
        }
    } while (FindNextFileA(handle, &find_data) != 0);

    FindClose(handle);
}
#else
static int pyemu_plugin_name_matches(const char* filename) {
    size_t length;
    if (filename == NULL) {
        return 0;
    }
    if (strncmp(filename, "pyemu_core_", 11) != 0) {
        return 0;
    }
    length = strlen(filename);
#if defined(__APPLE__)
    return length > 6 && strcmp(filename + length - 6, ".dylib") == 0;
#else
    return length > 3 && strcmp(filename + length - 3, ".so") == 0;
#endif
}

static int pyemu_get_host_directory(char* out_directory, size_t out_size) {
    Dl_info info;
    char path[PYEMU_PLUGIN_DIR_BUFFER];
    char* slash;

    if (out_directory == NULL || out_size == 0) {
        return 0;
    }
    if (dladdr((void*)&pyemu_get_supported_system_count, &info) == 0 || info.dli_fname == NULL) {
        return 0;
    }
    strncpy(path, info.dli_fname, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    strncpy(out_directory, path, out_size - 1);
    out_directory[out_size - 1] = '\0';
    return 1;
}

static void pyemu_try_load_core_library(const char* full_path) {
    pyemu_core_module_handle module;
    pyemu_get_core_plugin_fn get_plugin;
    const pyemu_core_plugin_descriptor* descriptor;

    module = dlopen(full_path, RTLD_NOW);
    if (module == NULL) {
        return;
    }
    get_plugin = (pyemu_get_core_plugin_fn)dlsym(module, PYEMU_CORE_PLUGIN_EXPORT_NAME);
    if (get_plugin == NULL) {
        dlclose(module);
        return;
    }
    descriptor = get_plugin();
    if (descriptor == NULL || descriptor->key == NULL || descriptor->create == NULL || pyemu_has_loaded_core_key(descriptor->key)) {
        dlclose(module);
        return;
    }
    pyemu_register_loaded_core(module, descriptor);
}

static void pyemu_scan_core_directory(const char* directory) {
    DIR* dir;
    struct dirent* entry;

    dir = opendir(directory);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (pyemu_plugin_name_matches(entry->d_name)) {
            char full_path[PYEMU_PLUGIN_DIR_BUFFER];
            if (snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name) >= 0) {
                pyemu_try_load_core_library(full_path);
            }
        }
    }

    closedir(dir);
}
#endif

static void pyemu_ensure_core_plugins_loaded(void) {
    char directory[PYEMU_PLUGIN_DIR_BUFFER];
    if (PYEMU_CORE_SCAN_COMPLETE) {
        return;
    }
    PYEMU_CORE_SCAN_COMPLETE = 1;
    if (!pyemu_get_host_directory(directory, sizeof(directory))) {
        return;
    }
    pyemu_scan_core_directory(directory);
}

static const pyemu_core_plugin_descriptor* pyemu_find_core_plugin(const char* system_key) {
    size_t index;
    const char* requested = system_key;

    pyemu_ensure_core_plugins_loaded();

    if (requested == NULL || requested[0] == '\0') {
        requested = pyemu_get_default_system_key();
    }

    for (index = 0; index < PYEMU_LOADED_CORE_COUNT; ++index) {
        if (strcmp(PYEMU_LOADED_CORES[index].descriptor->key, requested) == 0) {
            return PYEMU_LOADED_CORES[index].descriptor;
        }
    }

    return NULL;
}

PYEMU_API size_t pyemu_get_supported_system_count(void) {
    pyemu_ensure_core_plugins_loaded();
    return PYEMU_LOADED_CORE_COUNT;
}

PYEMU_API const char* pyemu_get_supported_system_key(size_t index) {
    pyemu_ensure_core_plugins_loaded();
    if (index >= PYEMU_LOADED_CORE_COUNT) {
        return "";
    }
    return PYEMU_LOADED_CORES[index].descriptor->key;
}

PYEMU_API const char* pyemu_get_default_system_key(void) {
    size_t index;
    pyemu_ensure_core_plugins_loaded();
    for (index = 0; index < PYEMU_LOADED_CORE_COUNT; ++index) {
        if (strcmp(PYEMU_LOADED_CORES[index].descriptor->key, "gameboy") == 0) {
            return PYEMU_LOADED_CORES[index].descriptor->key;
        }
    }
    return PYEMU_LOADED_CORE_COUNT > 0 ? PYEMU_LOADED_CORES[0].descriptor->key : "";
}

PYEMU_API pyemu_emulator* pyemu_create_emulator(const char* system_key) {
    const pyemu_core_plugin_descriptor* descriptor = pyemu_find_core_plugin(system_key);
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

    emulator->plugin = descriptor;
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

PYEMU_API uint8_t pyemu_peek_memory(pyemu_emulator* emulator, uint16_t address) {
    if (emulator == NULL || emulator->system == NULL || emulator->system->vtable->peek_memory == NULL) {
        return 0xFF;
    }
    return emulator->system->vtable->peek_memory(emulator->system, address);
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
    if (emulator == NULL || emulator->system == NULL || emulator->plugin == NULL || emulator->plugin->get_audio_channel_buffer == NULL) {
        return pyemu_zero_audio_buffer();
    }
    return emulator->plugin->get_audio_channel_buffer(emulator->system, channel);
}

PYEMU_API int pyemu_get_gameboy_audio_debug_info(const pyemu_emulator* emulator, pyemu_gameboy_audio_debug_info* out_info) {
    if (out_info == NULL) {
        return 0;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (emulator == NULL || emulator->system == NULL || emulator->plugin == NULL || emulator->plugin->get_audio_debug_info == NULL) {
        return 0;
    }
    emulator->plugin->get_audio_debug_info(emulator->system, out_info);
    return 1;
}

PYEMU_API void pyemu_set_gameboy_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions) {
    if (emulator == NULL || emulator->system == NULL || emulator->plugin == NULL || emulator->plugin->set_joypad_state == NULL) {
        return;
    }
    if (strcmp(emulator->plugin->key, "gameboy") != 0) {
        return;
    }
    emulator->plugin->set_joypad_state(emulator->system, buttons, directions);
}

PYEMU_API void pyemu_set_gbc_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions) {
    if (emulator == NULL || emulator->system == NULL || emulator->plugin == NULL || emulator->plugin->set_joypad_state == NULL) {
        return;
    }
    if (strcmp(emulator->plugin->key, "gbc") != 0) {
        return;
    }
    emulator->plugin->set_joypad_state(emulator->system, buttons, directions);
}

PYEMU_API void pyemu_set_bus_tracking(pyemu_emulator* emulator, int enabled) {
    if (emulator == NULL || emulator->system == NULL || emulator->plugin == NULL || emulator->plugin->set_bus_tracking == NULL) {
        return;
    }
    emulator->plugin->set_bus_tracking(emulator->system, enabled);
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
