# GBC Bring-Up Notes

This document records what was fixed after the initial Game Boy Color fork, how the issues were found, and what still needs work.

## Starting Point

The initial `gbc` core was forked from the stable DMG/Game Boy core. At handoff, it could be selected from the app, but it was not yet stable enough to boot real CGB software correctly.

Observed early symptoms:

- `.gbc` ROMs could route through the wrong core
- early boot could fault on valid opcodes
- some ROMs ran without crashing but only showed a black screen
- real games like `Tetris DX` stayed visually blank even when the CPU appeared alive

## What Was Fixed

### 1. Core Selection and UI Routing

Files:

- [E:\projects\pyemu\python\pyemu\runtime.py](E:/projects/pyemu/python/pyemu/runtime.py)
- [E:\projects\pyemu\python\pyemu\app.py](E:/projects/pyemu/python/pyemu/app.py)
- [E:\projects\pyemu\python\pyemu\widgets.py](E:/projects/pyemu/python/pyemu/widgets.py)

Changes:

- `.gbc` ROMs are now routed to the `gbc` core only
- `.gb` ROMs can be opened on either `gameboy` or `gbc`
- `.zip` files are inspected so inner `.gb` vs `.gbc` content can influence core selection
- `View` was renamed to `Core Settings`
- core-specific options are now driven by metadata instead of assuming DMG palette behavior for every core

This made the GBC flow usable from the app before deeper core debugging started.

### 2. Early GBC CPU Faults

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The first major fault showed up very early in boot around `0x0155`. The root cause was that the GBC opcode-family helpers had drifted from the stable Game Boy versions.

Fixes:

- replaced the GBC load/store dispatcher body with a GBC-adapted copy of the stable GB implementation
- replaced the GBC control-flow dispatcher body the same way
- replaced the GBC ALU dispatcher body the same way

Result:

- the immediate boot fault disappeared
- the GBC core could execute much farther into real startup code

### 3. Broken CB-Prefixed Execution

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

Later tracing found the GBC `CB` executor was badly corrupted:

- some op families wrote results into `A` incorrectly
- some groups used the wrong operation entirely
- bit/set/reset families were not behaving like the stable GB implementation

Fix:

- replaced `pyemu_gbc_execute_cb()` with the stable GB version adapted to the GBC helper names

Result:

- later boot faults disappeared
- CGB boot code could continue past the first helper-heavy paths

### 4. Frame Rendering Was Never Updated

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

One black-screen bug was very simple: `step_frame()` advanced execution, but did not refresh the framebuffer at the end of the frame.

Fix:

- `pyemu_gbc_step_frame()` now calls `pyemu_gbc_update_demo_frame()` after frame stepping

Result:

- the UI could finally receive updated GBC frame data instead of a permanently stale black buffer

### 5. CGB Tile Attribute and Tile Bank Handling

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The first version of the GBC renderer treated `VBK` like the live tile-bank selector for all rendering. That is not how CGB background/window rendering works.

Fixes:

- tile attributes for BG/window are read from VRAM bank 1 tilemap attribute bytes
- tile bank selection now comes from tile attribute bit 3
- x/y flip for BG/window tiles now comes from tile attributes
- sprite tile-bank selection now comes from sprite attribute bit 3

Result:

- the renderer moved closer to real CGB tile behavior
- this removed one major reason for seeing blank or nonsensical output

### 6. DMG-Style LCDC Bit 0 Background Gate Removed

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The GBC renderer was still treating `LCDC` bit 0 like a DMG "blank the background" switch. That is not the right interpretation for CGB rendering.

Fix:

- removed the DMG-style BG gate from the GBC renderer path

Result:

- prevented the whole frame from being incorrectly forced to white/black during CGB scenes

### 7. CGB WRAM Banking (`SVBK`)

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_internal.h](E:/projects/pyemu/native/src/systems/gbc/gbc_internal.h)
- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

`Tetris DX` exposed a more serious gap: VRAM never got populated even though the game was running. The key missing feature was banked WRAM.

Previously:

- the GBC core only had a flat 8 KB WRAM model
- `FF70` / `SVBK` was not implemented
- `D000-DFFF` and the echo region did not map to the selected WRAM bank

Fixes:

- expanded WRAM storage to support all CGB WRAM banks
- added `PYEMU_GBC_SVBK`
- implemented WRAM bank normalization so bank `0` maps to bank `1`
- `C000-CFFF` now maps to WRAM bank 0
- `D000-DFFF` now maps to the selected WRAM bank
- `E000-EFFF` and `F000-FDFF` now mirror the correct banked WRAM regions

Result:

- `Tetris DX` finally started filling VRAM
- this confirmed the game had been blocked by missing CGB memory behavior rather than only renderer issues

### 8. Interrupt/HALT Resume Behavior

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

After WRAM banking, `Tetris DX` still got trapped around the VBlank interrupt vector. The cause was another mismatch with the stable GB core:

- the GBC interrupt service path did not clear `cpu.halted` before checking `IME`

Fix:

- ported the stable Game Boy interrupt service behavior:
  - clear `cpu.halted` when interrupts are pending
  - clear `IME`, `ime_pending`, and `ime_delay` when servicing
  - clear `IF` consistently
  - tick interrupt service time

Result:

- the game no longer stayed stuck forever at the interrupt vector because of halt-state handling

### 9. Default GBC Post-Boot Palettes

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

Even after CPU, WRAM, and renderer fixes, `Tetris DX` still produced only black output for a long time because it did not immediately populate CGB palette RAM during the observed path.

Fix:

- added default grayscale-style CGB palette initialization to the post-boot state

Result:

- the framebuffer moved from all-black to visible white/gray output
- this is not yet full CGB correctness, but it unblocked the "nothing visible" issue

## How Issues Were Found

The debugging approach was mostly:

1. Use a public CGB test ROM for deterministic bring-up.
   - `cgb-acid2.gbc` was useful to confirm whether the CGB boot path could execute at all.

2. Use a real game for practical validation.
   - `Tetris DX (World) (SGB Enhanced).gbc` was the main real-game target.

3. Probe state headlessly through Python.
   - stepped instructions and frames with `Emulator('gbc')`
   - watched:
     - `PC`
     - `SP`
     - `IME`
     - `IF` / `IE`
     - `LCDC`, `STAT`, `LY`
     - `VBK`, `SVBK`
     - `FF68-FF6B`
     - VRAM nonzero counts
     - framebuffer color histograms

4. Compare GBC code to the stable GB core.
   - when a helper looked suspicious, the stable GB implementation was used as the reference model

This was enough to distinguish:

- CPU coverage bugs
- HALT/interrupt bugs
- memory banking bugs
- framebuffer refresh bugs
- renderer/palette bugs

## Current State

The GBC core is no longer in the original broken state.

Working improvements:

- `.gbc` ROMs route through the GBC core
- the core no longer fails immediately in early boot
- the framebuffer is now refreshed each frame
- WRAM banking exists
- VRAM starts getting populated in real games
- the renderer is no longer stuck on a permanently black frame in the same way it was before

But it is still not accurate yet.

For example:

- `Tetris DX` is now beyond the original all-black failure, but the output is still not correct
- some boot-state assumptions are still approximate
- palette behavior is still partly guessed rather than fully driven by correct CGB hardware behavior

## What Needs To Happen Next

### 1. Finish CGB Memory Model

The biggest next target is correctness, not bring-up.

Focus areas:

- verify WRAM bank behavior thoroughly
- verify VRAM bank behavior during both CPU access and rendering
- verify boot-state assumptions for CGB-only games

### 2. Improve CGB PPU Accuracy

Still needed:

- tighter CGB LCD timing
- proper use of tile attributes across all paths
- better sprite priority/attribute behavior
- more accurate LCD-off/LCD-on transitions for CGB

### 3. Improve CGB Palette Behavior

Current default palettes are only a bring-up aid.

Still needed:

- better post-boot palette state
- palette update behavior from real software paths
- correct handling when games rely on boot ROM-initialized palette state

### 4. Add GBC Regression Coverage

Recommended additions:

- public CGB test ROM smoke coverage
- `cgb-acid2`
- one or two real-game CGB smoke checks such as `Tetris DX`

Useful checks:

- boots without fault
- framebuffer not all black
- VRAM becomes populated
- interrupts continue to advance normally

### 5. Fix Native Build Integration

Right now the normal native build still lags behind the manual GBC builds used during bring-up.

We should update:

- [E:\projects\pyemu\native\CMakeLists.txt](E:/projects/pyemu/native/CMakeLists.txt)

so the standard build includes the GBC sources properly and we stop relying on ad hoc manual DLL builds.

## Summary

The main work after the initial GBC fork was:

- restore CPU execution parity with the stable GB core
- fix the broken CB path
- make the framebuffer actually render
- implement missing CGB-specific memory behavior
- fix HALT/interrupt resume behavior
- correct the renderer so it behaves like a CGB renderer rather than a DMG one with color tacked on

The core is now in a real bring-up state instead of a nonfunctional fork. The next phase is no longer "make it show anything at all"; it is "make CGB output correct."
