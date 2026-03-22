#include "pyemu/core/plugin.h"
#include "pyemu/systems/gameboy/gameboy_system.h"

static const pyemu_core_plugin_descriptor PYEMU_GAMEBOY_PLUGIN = {
    "gameboy",
    pyemu_gameboy_create,
    pyemu_gameboy_get_audio_channel_buffer,
    pyemu_gameboy_get_audio_debug_info,
    pyemu_gameboy_set_joypad_state,
    pyemu_gameboy_set_bus_tracking,
};

PYEMU_CORE_PLUGIN_API const pyemu_core_plugin_descriptor* pyemu_get_core_plugin(void) {
    return &PYEMU_GAMEBOY_PLUGIN;
}
