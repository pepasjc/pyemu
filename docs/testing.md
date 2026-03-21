# Testing Cores

This document describes how `pyemu` tests emulator cores today and how to use that process when bringing up a new system.

The testing strategy has two goals:

- catch regressions quickly while refactoring
- build confidence that a core is becoming accurate, not just "able to boot one ROM"

## Testing Layers

There are three practical layers:

1. Public deterministic test ROMs
2. Local gameplay smoke tests
3. Manual debugger-driven validation

All three matter.

Public test ROMs are best for correctness.
Gameplay smoke tests are best for catching regressions in real software.
Manual validation is best for diagnosing what went wrong when a test fails.

## 1. Public Test ROMs

The public test runner is:

- [E:\projects\pyemu\scripts\public_test_roms.py](/E:/projects/pyemu/scripts/public_test_roms.py)

It uses downloadable public ROM packs, not commercial games.

The downloader is:

- [E:\projects\pyemu\scripts\fetch_public_test_roms.py](/E:/projects/pyemu/scripts/fetch_public_test_roms.py)

### What it currently covers

Today the curated suite checks:

- `blargg/instr_timing`
- `blargg/halt_bug`
- `mooneye acceptance/bits/reg_f`
- `mooneye acceptance/timer/div_write`

The assertions are a mix of:

- framebuffer comparison against expected PNG output
- known-success CPU register patterns
- terminal self-loop checks
- fault detection

### How to run it

If you want the script to download the pack automatically:

```powershell
uv run python scripts/public_test_roms.py --download
```

If you already have the pack locally:

```powershell
$env:PYEMU_PUBLIC_TEST_ROMS_DIR='E:\path\to\game-boy-test-roms'
uv run python scripts/public_test_roms.py
```

### Why this layer matters

Use this layer to validate:

- instruction timing
- interrupt behavior
- timer correctness
- specific hardware behaviors with deterministic expected output

This is the most important layer when a core is still becoming correct.

## 2. Local Gameplay Smoke Tests

The local gameplay runner is:

- [E:\projects\pyemu\scripts\regression_smoke.py](/E:/projects/pyemu/scripts/regression_smoke.py)

This script uses real games already present on your machine and is intentionally separate from the public suite.

### What it currently checks

Today it covers:

- `Tetris`
- `Donkey Kong Land III`
- same-process ROM swapping from `Donkey Kong Land III` to `Wario Land II`

The checks are intentionally simple:

- ROM loads successfully
- the core does not fault after a fixed number of frames
- the framebuffer is not blank in cases where it should clearly show output

### How to run it

```powershell
uv run python scripts/regression_smoke.py
```

### Why this layer matters

Use this layer to catch:

- "it boots test ROMs but broke real games"
- ROM swap regressions
- state-reset bugs
- obvious rendering regressions
- refactor fallout

This is the best fast sanity check after core refactors.

## 3. Manual Validation

Automated tests are necessary, but not sufficient.

When bringing up a new core, you should also use:

- save states
- rewind
- trace capture
- debugger memory/register views
- audio dump / per-channel audio dump when debugging sound

Useful workflow:

1. Get the core to boot a public test ROM.
2. If it fails, inspect `PC`, `IF/IE`, `STAT`, `LCDC`, mapper state, and framebuffer behavior.
3. If a real game fails, save a state close to the bad behavior.
4. If the failure is timing-sensitive, capture a trace.
5. Compare the failing path against a stable core or a known-good emulator when needed.

This layer is where most real bug-fixing happens.

## What To Test For A New Core

When drafting a new core, do not treat "boots one game" as enough.

At minimum, try to build confidence in this order:

1. ROM loading and reset state
2. CPU instruction stepping
3. memory inspection and writes
4. visible framebuffer output
5. frame stepping
6. save/load state
7. multiple ROM loads in the same emulator instance
8. input handling
9. audio, if the system has sound

Once that baseline works, expand into:

- public deterministic hardware tests
- one or two real games that stress different parts of the hardware
- regression tests for any bug you fix

## Recommended Test Categories Per Core

For any new system, try to build tests in these categories:

### CPU / execution

- instruction stepping does not fault on known-good test ROMs
- timing-sensitive CPU tests where available
- interrupt entry/exit behavior

### Memory / mapper

- ROM banking
- RAM banking
- battery-backed save persistence
- same-process cartridge swapping

### Video

- nonblank framebuffer generation
- expected image output on deterministic tests
- transition effects / LCD state changes / raster effects where relevant

### Audio

- nonzero audio output on a known scene
- channel-isolated dump when debugging
- no obvious pop/hiss/incorrect instrument regressions after APU changes

### State and tooling

- save state round-trip
- rewind
- trace capture
- debugger memory poke/freeze if the core supports it

## What Counts As "Good Enough" Before Moving On

Before using a core as the baseline for another one, it should have:

- a stable native build
- at least one public deterministic test pass
- at least one real-game smoke path
- save/load state working
- enough debugger visibility to inspect failures without blind guessing

That is the bar the refactored GB core is currently closest to meeting.

## When A Test Fails

Use this order:

1. Reproduce with the smallest possible ROM or state.
2. Determine whether it is:
   - CPU bug
   - timing bug
   - mapper bug
   - renderer bug
   - frontend/runtime bug
3. Add or update a test after the fix if the failure is regression-prone.

If the bug came from a real game and not from a public suite, try to preserve at least one of these:

- a local smoke test
- a save state
- a trace folder
- a short doc note about the failure mode

## Current Best Practice

For everyday work on `pyemu`, the best routine is:

1. run the public suite when touching correctness-sensitive code
2. run the local gameplay smoke suite after refactors
3. use traces and save states for hard game-specific bugs

That combination gives good coverage without making every edit painfully slow.
