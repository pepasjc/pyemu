#pragma once

#include "pyemu/core/system.h"

#ifdef _WIN32
  #ifdef PYEMU_BUILD_CORE_DLL
    #define PYEMU_CORE_PLUGIN_API __declspec(dllexport)
  #else
    #define PYEMU_CORE_PLUGIN_API __declspec(dllimport)
  #endif
#else
  #define PYEMU_CORE_PLUGIN_API
#endif

#define PYEMU_CORE_PLUGIN_EXPORT_NAME "pyemu_get_core_plugin"

typedef struct pyemu_core_plugin_descriptor {
    const char* key;
    pyemu_system* (*create)(void);
    pyemu_audio_buffer (*get_audio_channel_buffer)(const pyemu_system* system, int channel);
    void (*get_audio_debug_info)(const pyemu_system* system, pyemu_gameboy_audio_debug_info* out_info);
    void (*set_joypad_state)(pyemu_system* system, uint8_t buttons, uint8_t directions);
    void (*set_bus_tracking)(pyemu_system* system, int enabled);
} pyemu_core_plugin_descriptor;

typedef const pyemu_core_plugin_descriptor* (*pyemu_get_core_plugin_fn)(void);

PYEMU_CORE_PLUGIN_API const pyemu_core_plugin_descriptor* pyemu_get_core_plugin(void);
