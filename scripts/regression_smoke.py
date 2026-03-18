from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PYTHON_DIR = ROOT / "python"
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from pyemu.runtime import Emulator


def nonwhite_count(emu: Emulator) -> int:
    rgba = emu.frame_buffer.rgba or []
    return sum(1 for i in range(0, len(rgba), 4) if rgba[i:i+3] != [255, 255, 255])


def run_frames(emu: Emulator, count: int) -> None:
    for _ in range(count):
        emu.step_frame()
        if emu.faulted:
            break


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_tetris() -> None:
    emu = Emulator()
    rom = ROOT / 'tetris.gb'
    check(emu.load_media(str(rom)), 'failed to load tetris.gb')
    run_frames(emu, 30)
    check(not emu.faulted, f'Tetris faulted at PC={emu.cpu_state.pc:#06x}')
    print('PASS tetris', hex(emu.cpu_state.pc), nonwhite_count(emu))


def test_dkl3() -> None:
    emu = Emulator()
    rom = ROOT / 'roms' / 'Donkey Kong Land III (USA, Europe) (Rev A) (SGB Enhanced).gb'
    check(emu.load_media(str(rom)), 'failed to load Donkey Kong Land III')
    run_frames(emu, 60)
    check(not emu.faulted, f'DKL3 faulted at PC={emu.cpu_state.pc:#06x}')
    print('PASS dkl3', hex(emu.cpu_state.pc), nonwhite_count(emu))


def test_sequential_swap() -> None:
    emu = Emulator()
    rom1 = ROOT / 'roms' / 'Donkey Kong Land III (USA, Europe) (Rev A) (SGB Enhanced).gb'
    rom2 = ROOT / 'roms' / 'Wario Land II (USA, Europe) (SGB Enhanced).gb'
    check(emu.load_media(str(rom1)), 'failed to load Donkey Kong Land III for swap test')
    run_frames(emu, 30)
    check(not emu.faulted, f'DKL3 faulted before swap at PC={emu.cpu_state.pc:#06x}')
    check(emu.load_media(str(rom2)), 'failed to load Wario Land II after DKL3')
    run_frames(emu, 120)
    check(not emu.faulted, f'Wario Land II faulted after swap at PC={emu.cpu_state.pc:#06x}')
    check(nonwhite_count(emu) > 0, 'Wario Land II produced an all-white framebuffer after swap')
    print('PASS swap', hex(emu.cpu_state.pc), nonwhite_count(emu))


def main() -> int:
    print('DLL check...')
    emu = Emulator()
    print('using', emu.native_library_path)
    test_tetris()
    test_dkl3()
    test_sequential_swap()
    print('ALL PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
