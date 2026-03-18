# Adding a Core

This is the current path for adding another system to `pyemu`.

## 1. Add the native system implementation

Create a new system pair under `native/` similar to the Game Boy core:
- `native/include/pyemu/systems/<system>/<system>_system.h`
- `native/src/systems/<system>/<system>_system.c`

At minimum, expose a create function that returns `pyemu_system*`.

## 2. Implement the vtable

Your system must fill out `pyemu_system_vtable` from:
- [native/include/pyemu/core/system.h](E:/projects/pyemu/native/include/pyemu/core/system.h)

Required behaviors:
- `reset`
- `load_rom`
- `step_instruction`
- `step_frame`
- `destroy`
- CPU/frame/memory getters
- ROM metadata getters
- cycle count
- fault state

Strong recommendation:
- make `step_instruction` the correctness reference path
- make `step_frame` the user-facing fast path

## 3. Register the system

Add the new key to the registry in:
- [native/src/core/emulator.c](E:/projects/pyemu/native/src/core/emulator.c)

Example shape:
- system key: `"nes"`
- create function: `pyemu_nes_create`

That immediately makes it visible through the generic C API.

## 4. Add Python metadata

Add a `SystemInfo` entry in:
- [python/pyemu/runtime.py](E:/projects/pyemu/python/pyemu/runtime.py)

You need:
- unique `key`
- display name
- screen size
- CPU frequency
- frame rate
- media label and extensions

Once that exists, Python can construct `Emulator('<key>')` generically.

## 5. Keep the first UI pass generic

Before adding system-specific widgets, try to reuse the existing debugger using:
- frame buffer
- CPU state
- memory snapshot
- cartridge/media metadata
- trace/save-state support

That gets you a usable bring-up environment quickly.

## 6. Add system-specific helpers only after bring-up

Examples:
- Game Boy joypad helper
- tile viewers
- cartridge mapper panels
- PPU register panels

These should come after the core can boot software through the generic path.

## Suggested bring-up order

1. ROM loading and reset state
2. `step_instruction`
3. memory inspection
4. frame buffer output
5. `step_frame`
6. save/load state
7. debugger-specific helpers

## Practical advice from the current Game Boy core

- keep one stable public API and hide system-specific details behind the vtable
- expose enough debug information early; it saves huge amounts of time later
- make the Python debugger useful before the emulator is perfect
- prefer correctness in the interpreter path before chasing optimization
- only add system-specific public API when the generic surface truly is not enough
