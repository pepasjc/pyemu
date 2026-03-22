#include "pyemu/core/plugin.h"
#include "pyemu/systems/gbc/gbc_system.h"

static const pyemu_core_plugin_descriptor PYEMU_GBC_PLUGIN = {
    "gbc",
    pyemu_gbc_create,
    pyemu_gbc_get_audio_channel_buffer,
    pyemu_gbc_get_audio_debug_info,
    pyemu_gbc_set_joypad_state,
    pyemu_gbc_set_bus_tracking,
};

PYEMU_CORE_PLUGIN_API const pyemu_core_plugin_descriptor* pyemu_get_core_plugin(void) {
    return &PYEMU_GBC_PLUGIN;
}
