# GBC Bring-Up Notes

This document records the current Game Boy Color bring-up status, what was fixed after the initial fork from the DMG core, how the issues were identified, and what still needs work.

## Current State

The GBC core is no longer in the original "boots but black screen" phase.

As of the current bring-up:

- `Tetris DX` boots and renders real gameplay
- `Pokemon Gold` boots and gets into meaningful runtime state
- `Pokemon Crystal` now gets much farther into boot and renders real framebuffer data
- GBC audio is wired up and produces real output
- MBC3 RTC support is present
- the main active real-game blocker is now `Super Mario Bros. Deluxe`

The current GBC work is therefore in the "real game correctness" phase rather than the "basic boot" phase.

## What Was Fixed

### 1. Core Selection and App Routing

Files:

- [E:\projects\pyemu\python\pyemu\runtime.py](E:/projects/pyemu/python/pyemu/runtime.py)
- [E:\projects\pyemu\python\pyemu\app.py](E:/projects/pyemu/python/pyemu/app.py)
- [E:\projects\pyemu\python\pyemu\widgets.py](E:/projects/pyemu/python/pyemu/widgets.py)

Changes:

- `.gbc` ROMs are routed to the `gbc` core
- `.gb` ROMs can be loaded on either `gameboy` or `gbc`
- `.zip` files are inspected so inner ROM type can influence core selection
- `View` was generalized into `Core Settings`
- per-core settings now come from metadata instead of assuming DMG palette behavior

This made the GBC core usable from the app before deeper native debugging started.

### 2. Early CPU Coverage and Helper Parity

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The initial GBC fork had drifted from the stable GB opcode-family helpers, which caused early valid boot code to fault.

Fixes:

- replaced the GBC load/store dispatcher bodies with the stable GB equivalents adapted to GBC helpers
- replaced control-flow helpers the same way
- replaced ALU-family helpers the same way
- replaced the broken GBC `CB` executor with the stable GB logic adapted for CGB

Result:

- the immediate startup faults disappeared
- real CGB software could execute much farther into boot

### 3. Framebuffer Refresh and Basic Rendering

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)
- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

Early black-screen issues came from several independent problems:

- `step_frame()` was not reliably updating the framebuffer
- the renderer still had DMG-style assumptions in CGB paths
- tile attributes and tile-bank handling were wrong

Fixes:

- `step_frame()` now refreshes the GBC framebuffer correctly
- BG/window tile attributes are read from VRAM bank 1
- tile bank selection uses tile attribute bit 3
- BG/window flip bits use the tile attribute bits
- sprite tile bank uses OAM attribute bit 3
- removed the DMG-style LCDC bit 0 background gate from the GBC renderer

Result:

- the GBC core moved from "CPU alive but black screen" to "real image path exists"

### 4. WRAM Banking (`SVBK`)

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_internal.h](E:/projects/pyemu/native/src/systems/gbc/gbc_internal.h)
- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

`Tetris DX` exposed a missing CGB memory feature: banked WRAM.

Fixes:

- added full CGB WRAM bank storage
- implemented `SVBK` (`FF70`)
- normalized bank `0` to bank `1`
- mapped `D000-DFFF` to the selected bank
- mirrored the correct banked regions into `E000-FDFF`

Result:

- `Tetris DX` began populating VRAM and could progress visually

### 5. Interrupt / HALT Resume Behavior

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The GBC interrupt service path was not resuming from `HALT` the same way the stable GB core did.

Fix:

- ported the stable GB interrupt service behavior into the GBC path
- pending interrupts now clear `cpu.halted`
- `IME`, `ime_pending`, and `ime_delay` are cleared consistently during service

Result:

- CGB titles stopped getting stuck around interrupt-driven wakeup paths

### 6. GBC Timing Model Port

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

`Pokemon Crystal` exposed a deeper gap: the GBC core still had placeholder `STAT`, timer, and VRAM/OAM access logic.

The strongest clue came from boot code around `0x4233-0x4245`, where the game polls `STAT & 0x03` waiting for the expected LCD mode transitions before continuing its setup path.

Fixes:

- ported the stable GB `STAT` mode model into the GBC helper path
- `STAT` mode bits now come from `LY` + `ppu_counter`, not stale low bits
- ported the stable GB timer edge model for `DIV/TIMA/TAC`
- fixed CPU VRAM/OAM accessibility checks to depend on the real LCD mode timing instead of placeholder mode values
- kept the earlier interrupt resume fix in place

Result:

- `Pokemon Crystal` no longer sits in the old `STAT` polling dead path
- the game now gets much farther into boot and produces real VRAM plus non-black framebuffer output

### 7. MBC5 Bring-Up and Banking Fixes

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)
- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

The GBC core needed MBC5 support for titles like `Tetris DX` and `Super Mario Bros. Deluxe`.

Fixes:

- added MBC5 mapper support
- corrected MBC5 ROM bank handling so the low ROM bank byte and high ROM bank bit are decoded on the proper write ranges

Result:

- MBC5 titles now get onto valid banked code/data paths instead of obviously wrong ones

### 8. Audio and RTC

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_apu.c](E:/projects/pyemu/native/src/systems/gbc/gbc_apu.c)
- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)
- [E:\projects\pyemu\python\pyemu\app.py](E:/projects/pyemu/python/pyemu/app.py)

Added:

- GBC sound path integration
- RTC behavior for MBC3-backed titles

This is enough for real GBC titles to produce audio and for RTC-backed games to get past basic cartridge-state expectations.

One important note:

- audio is present, but it is still approximate and not yet considered accurate for all titles

## How The Issues Were Found

The debugging approach has been a mix of deterministic public tests, headless runtime checks, and real-game traces.

### Public / deterministic bring-up

Used:

- `cgb-acid2.gbc`

This was useful early to confirm whether the CGB boot path and renderer were even alive.

### Real games

Used repeatedly:

- `Tetris DX (World) (SGB Enhanced).gbc`
- `Pokemon - Gold Version (USA, Europe) (SGB Enhanced).gbc`
- `Pokemon - Crystal Version (USA, Europe) (Rev A).gbc`
- `Super Mario Bros. Deluxe (USA, Europe) (Rev B).gbc`

These games were useful for different reasons:

- `Tetris DX`: WRAM banking, early rendering, general CGB bring-up
- `Pokemon Gold`: real runtime sanity check
- `Pokemon Crystal`: exposed the weak `STAT`/timer/PPU-mode model
- `Mario Deluxe`: currently exposes remaining transition / banking / APU correctness issues

### Headless probing

I repeatedly stepped frames or instructions with `Emulator('gbc')` and inspected:

- `PC`, `SP`, `IME`
- `IF/IE`
- `LCDC`, `STAT`, `LY`
- `VBK`, `SVBK`
- `FF68-FF6B`
- `FF55`
- VRAM nonzero counts
- framebuffer color counts and non-black pixel counts

This was especially useful for distinguishing:

- CPU coverage bugs
- interrupt/HALT bugs
- memory banking bugs
- timing/model bugs
- rendering bugs

### Trace-driven debugging

The current Mario Deluxe investigation is trace-driven.

Trace used:

- [E:\projects\pyemu\traces\20260320-203315-Super_Mario_Bros._Deluxe_(USA,_Europe)_(Rev_B)](E:/projects/pyemu/traces/20260320-203315-Super_Mario_Bros._Deluxe_(USA,_Europe)_(Rev_B))

The trace showed:

- repeated time inside a banked copy loop around `0x145A`
- LCD mostly off during the bad phase
- brief per-frame wakeups around the same interrupt-driven path
- state oscillating between valid-looking and obviously wrong visual output

That was the signal that pushed the investigation away from "generic black screen" and toward transition/copy-state correctness.

## Verified Working Targets

These are the practical bring-up wins that are currently established:

### Tetris DX

- boots without fault
- produces real framebuffer output
- produces audio

### Pokemon Gold

- boots and reaches meaningful runtime state
- produces real framebuffer output

### Pokemon Crystal

- no longer stuck in the earlier `STAT`-polling boot dead path
- reaches much later boot/runtime code
- produces real framebuffer output instead of staying permanently black

### 9. Renderer VBK tile-index bug fix

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_helpers.c](E:/projects/pyemu/native/src/systems/gbc/gbc_helpers.c)

The GBC renderer was reading tile map indices (tile numbers) from `gbc->memory[tile_map_address]`, which reflects whatever VRAM bank is currently mapped via `VBK`. When `VBK=1` was active during a scanline render, the tile map address range `0x9800-0x9FFF` in `gbc->memory` contained VRAM bank 1 data (tile attributes), not bank 0 tile indices. This produced garbage tile indices and corrupted visuals.

Fix:

- BG tile map: changed to `gbc->vram[tile_map_address - 0x8000]` (always VRAM bank 0)
- Window tile map: same fix applied

The tile attribute reads (`gbc->vram[... + 0x2000]`) were already always reading from bank 1 and were correct.

Result:

- visual corruption caused by wrong tile indices when `VBK=1` is eliminated

### 10. LCD-on transition: window counter and scanline register latch

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

When LCDC bit 7 transitions from 0 to 1 (LCD turns on), the window line counter was not reset and scanline registers for line 0 were not latched from the current IO state.

Fixes:

- reset `window_line_counter` to 0 on LCD turn-on
- call `pyemu_gbc_latch_scanline_registers(gbc, 0)` on LCD turn-on so line 0 uses the current SCX, SCY, LCDC, etc. rather than stale values from a previous frame

### 11. Opcode 0xF8 (`LD HL, SP+r8`) missing flag updates

Files:

- [E:\projects\pyemu\native\src\systems\gbc\gbc_system.c](E:/projects/pyemu/native/src/systems/gbc/gbc_system.c)

`LD HL, SP+r8` was computing the correct HL value but not updating the H and C flags. The SM83 specification requires this instruction to set H and C the same way as `ADD SP, r8` (based on the low-byte addition), with Z=0 and N=0.

Fix:

- compute H and C flags from `(SP ^ offset ^ result)` and apply with `pyemu_gbc_set_flags_znhc`

## Current Active Problem

### Super Mario Bros. Deluxe

Current symptoms:

- audio is present but sounds wrong
- the game may still have remaining visual issues beyond the VBK renderer fix

The main visual corruption (garbage frames when VBK=1 during rendering) has been fixed. The remaining issues are likely:

- APU accuracy (sounds wrong is a separate channel/envelope correctness problem)
- any remaining transition-phase bugs not yet triggered

Current strongest leads:

- APU channel correctness, envelopes, timing, sequencing
- run the game again and collect a new trace to re-evaluate visual stability

## What Needs To Happen Next

### 1. Validate Mario Deluxe after renderer fix

Recommended next steps:

- run `Super Mario Bros. Deluxe` and collect a new trace
- check whether visuals are now stable or whether new corruption patterns appear
- if new garbage appears, instrument `VBK`, `LCDC`, and tile map writes at scanline granularity

### 2. Add GBC regression coverage

Recommended additions:

- automated `cgb-acid2` smoke or pixel-comparison test
- `Tetris DX` smoke test
- `Pokemon Crystal` smoke test
- eventually a Mario Deluxe trace-based regression once the title is stable enough

### 3. Fix native build integration

The normal native build flow should include GBC sources cleanly so we stop relying on manually named DLL bring-up builds.

### 4. Improve GBC audio accuracy

Sound exists now, but accuracy work still remains:

- channel correctness
- envelopes / timing / sequencing details
- game-specific instrument quality

### 5. Keep pushing real-game correctness

The current phase is no longer about getting any pixels on screen. It is about making real CGB titles behave consistently through:

- transitions
- banked copy/setup code
- LCD state changes
- real mapper usage

## Summary

The GBC bring-up has progressed through these phases:

1. App/core routing and core selection
2. CPU helper parity with the stable GB core
3. CB-prefixed execution repair
4. Framebuffer refresh and renderer bring-up
5. WRAM banking (`SVBK`)
6. Interrupt/HALT correctness
7. `STAT` / timer / PPU timing model port
8. MBC5 bring-up and bank-write correction
9. Audio and RTC support
10. Real-game validation on `Tetris DX`, `Pokemon Gold`, and `Pokemon Crystal`
11. Renderer VBK tile-index fix (always read tile map from VRAM bank 0)
12. LCD-on window counter reset and scanline register latch
13. `LD HL, SP+r8` (opcode `0xF8`) H/C flag fix

The active front is now much narrower:

- the GBC core is alive and useful
- several real games now boot meaningfully
- `Super Mario Bros. Deluxe` is the current best title for finding the next real correctness gaps
