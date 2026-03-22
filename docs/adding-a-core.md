# Adding a Core

This is the current path for adding another system to `pyemu`.

## 1. Start From the Baseline Layout

The Game Boy core is now the baseline template for new systems.

Create a new system directory shaped like this:

- `native/include/pyemu/systems/<system>/<system>_system.h`
- `native/src/systems/<system>/<system>_system.c`
- `native/src/systems/<system>/<system>_internal.h`

And strongly consider splitting subsystems immediately instead of waiting for a monolith:

- `memory.c`
- `state.c`
- `input.c`
- `ppu.c` or other video module
- `apu.c` or other audio module
- CPU files if the core follows the same execution model

The practical goal is to keep `<system>_system.c` thin and orchestration-focused.

Documentation expectation:
- every non-trivial native function should have a short contract comment describing what it owns, what side effects it is responsible for, and when callers should use it
- prefer comments that explain intent and boundaries, not line-by-line mechanics

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
- keep reset/load/save-state logic outside the main execution file if possible

## 3. Export the core plugin

Add a small plugin source file for the system that exports a descriptor through:
- [native/include/pyemu/core/plugin.h](E:/projects/pyemu/native/include/pyemu/core/plugin.h)

Example shape:
- system key: `"nes"`
- create function: `pyemu_nes_create`
- optional debugger/input helpers for audio channels, input, and bus tracking

The host DLL discovers these per-core plugin DLLs at runtime, so new systems no longer need a hardcoded registry entry in `emulator.c`.

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

Once that exists, Python can construct `Emulator('<key>')` generically as long as the matching core DLL is present beside the host DLL.

## 5. Keep the first UI pass generic

Before adding system-specific widgets, try to reuse the existing debugger using:
- frame buffer
- CPU state
- memory snapshot
- cartridge/media metadata
- trace/save-state support

Also define the system metadata in Python early:

- input actions
- debug memory regions
- debugger watch entries
- register layout
- flags
- core settings

That keeps the UI generic and makes the new core feel integrated much earlier.

That gets you a usable bring-up environment quickly.

## 6. Add system-specific helpers only after bring-up

Examples:
- Game Boy joypad helper
- tile viewers
- cartridge mapper panels
- PPU register panels

These should come after the core can boot software through the generic path.

## Suggested bring-up order

1. ROM loading and reset/post-boot state
2. memory map and read/write plumbing
3. `step_instruction`
4. frame buffer output
5. `step_frame`
6. save/load state
7. debugger metadata
8. system-specific helpers only if the generic debugger is not enough

## Practical advice from the current Game Boy core

- keep one stable public API and hide system-specific details behind the vtable
- copy the refactored Game Boy subsystem split instead of starting from one giant file
- expose enough debug information early; it saves huge amounts of time later
- make the Python debugger useful before the emulator is perfect
- prefer correctness in the interpreter path before chasing optimization
- only add system-specific public API when the generic surface truly is not enough

## Recommended baseline structure

If you are drafting a new core, this is the recommended starting shape:

- `<system>_system.c`
  - vtable
  - top-level orchestration
  - minimal glue only
- `<system>_internal.h`
  - internal structs
  - private helper declarations
- `memory.c`
  - memory map and bus semantics
- `state.c`
  - reset/load/save state
- `input.c`
  - controller/joypad state
- `ppu.c` or equivalent
  - video timing and rendering
- `apu.c` or equivalent
  - audio generation
- CPU support files
  - helpers
  - core fetch/interrupt logic
  - opcode-family dispatch

That is the closest thing `pyemu` currently has to a standard native-core blueprint.
