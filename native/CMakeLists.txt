add_library(pyemu_native SHARED
    src/core/emulator.c
    src/systems/gameboy/gameboy_system.c
)

target_include_directories(pyemu_native
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(pyemu_native PRIVATE PYEMU_BUILD_DLL)
endif()
