from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtGui import QImage

ROOT = Path(__file__).resolve().parents[1]
PYTHON_DIR = ROOT / "python"
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from pyemu.runtime import Emulator
from fetch_public_test_roms import CACHE_DIR, ensure_public_test_roms

FIB_REGS = (0x03, 0x05, 0x08, 0x0D, 0x15, 0x22)


@dataclass(frozen=True)
class PublicRomTest:
    name: str
    rom_relative: tuple[str, ...]
    frames: int
    expected_png_relative: tuple[str, ...] | None = None
    expect_fibonacci_regs: bool = False
    expect_terminal_jr_loop: bool = False


TESTS: list[PublicRomTest] = [
    PublicRomTest(
        name='blargg_instr_timing',
        rom_relative=('blargg', 'instr_timing', 'instr_timing.gb'),
        frames=120,
        expected_png_relative=('blargg', 'instr_timing', 'instr_timing-dmg-cgb.png'),
    ),
    PublicRomTest(
        name='blargg_halt_bug',
        rom_relative=('blargg', 'halt_bug.gb'),
        frames=180,
        expected_png_relative=('blargg', 'halt_bug-dmg-cgb.png'),
    ),
    PublicRomTest(
        name='mooneye_reg_f',
        rom_relative=('mooneye-test-suite', 'acceptance', 'bits', 'reg_f.gb'),
        frames=300,
        expect_fibonacci_regs=True,
        expect_terminal_jr_loop=True,
    ),
    PublicRomTest(
        name='mooneye_div_write',
        rom_relative=('mooneye-test-suite', 'acceptance', 'timer', 'div_write.gb'),
        frames=300,
        expect_fibonacci_regs=True,
        expect_terminal_jr_loop=True,
    ),
]


def resolve_test_root(download: bool) -> Path:
    override = os.environ.get('PYEMU_PUBLIC_TEST_ROMS_DIR')
    if override:
        return Path(override)
    if CACHE_DIR.exists() or download:
        return ensure_public_test_roms(force=False)
    raise FileNotFoundError(
        'public test rom pack not found. Run scripts/fetch_public_test_roms.py or set PYEMU_PUBLIC_TEST_ROMS_DIR.'
    )


def framebuffer_mismatches(emu: Emulator, expected_png: Path) -> int:
    image = QImage(str(expected_png)).convertToFormat(QImage.Format.Format_RGBA8888)
    rgba = emu.frame_buffer.rgba or []
    expected_size = image.width() * image.height() * 4
    if len(rgba) != expected_size:
        raise AssertionError(
            f'frame size mismatch: emulator={len(rgba)} expected={expected_size} ({image.width()}x{image.height()})'
        )
    mismatches = 0
    for y in range(image.height()):
        for x in range(image.width()):
            color = image.pixelColor(x, y)
            idx = (y * image.width() + x) * 4
            if (color.red(), color.green(), color.blue(), color.alpha()) != tuple(rgba[idx : idx + 4]):
                mismatches += 1
    return mismatches


def assert_result(test: PublicRomTest, emu: Emulator, expected_png: Path | None) -> None:
    if emu.faulted:
        raise AssertionError(f'faulted at PC={emu.cpu_state.pc:#06x}')

    memory = emu.memory_snapshot()
    pc = emu.cpu_state.pc
    opcode = memory[pc] if memory else None

    if test.expect_terminal_jr_loop and opcode != 0x18:
        raise AssertionError(f'expected terminal JR loop, got opcode {opcode:#04x} at PC={pc:#06x}')

    if test.expect_fibonacci_regs:
        regs = (emu.cpu_state.b, emu.cpu_state.c, emu.cpu_state.d, emu.cpu_state.e, emu.cpu_state.h, emu.cpu_state.l)
        if regs != FIB_REGS:
            raise AssertionError(f'expected Fibonacci registers {FIB_REGS}, got {regs}')

    if expected_png is not None:
        mismatches = framebuffer_mismatches(emu, expected_png)
        if mismatches != 0:
            raise AssertionError(f'screenshot mismatch: {mismatches} pixels differ')



def run_test(test_root: Path, test: PublicRomTest) -> None:
    rom_path = test_root.joinpath(*test.rom_relative)
    expected_png = test_root.joinpath(*test.expected_png_relative) if test.expected_png_relative is not None else None
    if not rom_path.exists():
        raise FileNotFoundError(f'missing rom: {rom_path}')
    if expected_png is not None and not expected_png.exists():
        raise FileNotFoundError(f'missing expected image: {expected_png}')

    emu = Emulator()
    if not emu.load_media(str(rom_path)):
        raise AssertionError(f'failed to load {rom_path}')

    for _ in range(test.frames):
        emu.step_frame()
        if emu.faulted:
            break

    assert_result(test, emu, expected_png)
    print(f'PASS {test.name}: pc={emu.cpu_state.pc:#06x}')



def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--download', action='store_true', help='download the public test-rom pack into artifacts/ if missing')
    args = parser.parse_args(argv)

    test_root = resolve_test_root(download=args.download)
    emu = Emulator()
    print('using', emu.native_library_path)
    print('test-rom root', test_root)
    for test in TESTS:
        run_test(test_root, test)
    print('ALL PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
