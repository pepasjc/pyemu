cmake_minimum_required(VERSION 3.15)
project(pyemu_native C)

set(PYEMU_NATIVE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/native")

function(pyemu_configure_native_target target)
    target_include_directories(${target}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PYEMU_NATIVE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PYEMU_NATIVE_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${PYEMU_NATIVE_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PYEMU_NATIVE_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PYEMU_NATIVE_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PYEMU_NATIVE_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${PYEMU_NATIVE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PYEMU_NATIVE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PYEMU_NATIVE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PYEMU_NATIVE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${PYEMU_NATIVE_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${PYEMU_NATIVE_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${PYEMU_NATIVE_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PYEMU_NATIVE_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${PYEMU_NATIVE_OUTPUT_DIR}"
    )
endfunction()

add_library(pyemu_native SHARED
    src/core/emulator.c
)
pyemu_configure_native_target(pyemu_native)
if(WIN32)
    target_compile_definitions(pyemu_native PRIVATE PYEMU_BUILD_DLL)
endif()

add_library(pyemu_core_gameboy SHARED
    src/systems/gameboy/gameboy_plugin.c
    src/systems/gameboy/gameboy_system.c
    src/systems/gameboy/apu.c
    src/systems/gameboy/mapper.c
    src/systems/gameboy/ppu.c
    src/systems/gameboy/input.c
    src/systems/gameboy/memory.c
    src/systems/gameboy/state.c
    src/systems/gameboy/cpu_helpers.c
    src/systems/gameboy/cpu_core.c
    src/systems/gameboy/cpu_exec.c
)
pyemu_configure_native_target(pyemu_core_gameboy)
if(WIN32)
    target_compile_definitions(pyemu_core_gameboy PRIVATE PYEMU_BUILD_CORE_DLL)
endif()

add_library(pyemu_core_gbc SHARED
    src/systems/gbc/gbc_plugin.c
    src/systems/gbc/gbc_system.c
    src/systems/gbc/gbc_helpers.c
    src/systems/gbc/gbc_apu.c
)
pyemu_configure_native_target(pyemu_core_gbc)
if(WIN32)
    target_compile_definitions(pyemu_core_gbc PRIVATE PYEMU_BUILD_CORE_DLL)
endif()
