cmake_minimum_required(VERSION 3.15)
project(pyemu_native C)

add_library(pyemu_native SHARED
    src/core/emulator.c
    src/systems/gameboy/gameboy_system.c
    src/systems/gameboy/apu.c
    src/systems/gameboy/mapper.c
    src/systems/gameboy/ppu.c
    src/systems/gameboy/input.c
    src/systems/gameboy/cpu_helpers.c
    src/systems/gameboy/cpu_core.c
    src/systems/gameboy/cpu_exec.c
    src/systems/gbc/gbc_system.c
    src/systems/gbc/gbc_helpers.c
    src/systems/gbc/gbc_apu.c
)

target_include_directories(pyemu_native
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(pyemu_native PRIVATE PYEMU_BUILD_DLL)
endif()
