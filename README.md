# pyemu

`pyemu` is an educational emulator project with a native core in C and a Python-first debugger/UI.

The current system is the Nintendo Game Boy, but the project is structured as a multi-core framework:
- a stable native emulator handle in C
- per-system implementations behind a `pyemu_system_vtable`
- a Python runtime layer via `cffi`
- a desktop debugger built with `PySide6` and a pluggable display backend

## Current capabilities

- run, pause, step instruction, step frame
- save/load state and rewind
- trace capture for debugging tricky game paths
- battery-backed save RAM persistence
- zipped ROM loading
- live debugger panels for CPU, memory, cartridge state, and hardware access

## Project layout

- `native/`
  - C core framework and system implementations
- `python/pyemu/`
  - `cffi` runtime, debugger UI, and display backends
- `docs/`
  - architecture and contributor notes
- `roms/`, `states/`, `traces/`
  - local testing assets and debugging artifacts

## Architecture

Start here if you want to understand or extend the emulator:
- [Architecture](E:/projects/pyemu/docs/architecture.md)
- [Adding a Core](E:/projects/pyemu/docs/adding-a-core.md)

## Setup with uv

1. Create the virtual environment:
   - `uv venv`
2. Sync dependencies:
   - `uv sync`
3. Run the debugger:
   - `uv run python -m pyemu`

## Native build notes

The Python runtime loads the newest known-good native DLL from `build/native/` on Windows.

A direct local build command we use frequently is:

```powershell
gcc -shared -O2 -std=c11 -Wall -Wextra -DPYEMU_BUILD_DLL -I native\include native\src\core\emulator.c native\src\systems\gameboy\gameboy_system.c -o build
ative\pyemu_native_timed110.dll
```

## Design goals

- keep the core easy to step through and study
- expose emulator state cleanly to Python
- make debugging and experimentation first-class
- keep the system boundary generic so new cores can be added without rewriting the UI
