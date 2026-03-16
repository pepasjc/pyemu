# pyemu

`pyemu` is an educational emulator project with a native core in C and a
Python-first interface for inspection, scripting, and debugging.

The first target system is the Nintendo Game Boy. The codebase is organized so
we can add more systems later while keeping a stable Python API.

## Goals

- Keep emulation logic explicit and easy to study
- Expose CPU, memory, and frame state to Python
- Support stepping, tracing, and future debugger features
- Leave room for scripting and autopilot experiments

## Layout

- `native/`: C emulator core and exported API
- `python/pyemu/`: cffi bridge and PySide6 UI entry point

## Initial direction

This scaffold provides:

- a reusable emulator handle and system API
- a stub Game Boy backend
- a C ABI consumed from Python through `cffi`
- a small PySide6 UI shell

## Setup with uv

1. Create the virtual environment:
   - `uv venv`
2. Sync project dependencies:
   - `uv sync`
3. Build the native library with CMake once a C toolchain is available.
4. Run the UI:
   - `uv run python -m pyemu`

## Notes

- `uv sync` will install `cffi` and `PySide6` from `pyproject.toml`.
- The Python UI works now, but the native C library still needs a working local
  C toolchain before the `cffi` bridge can load the real backend.
