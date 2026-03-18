from __future__ import annotations

import argparse
import shutil
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE_DIR = ROOT / 'artifacts' / 'game-boy-test-roms-v7.0'
ARCHIVE_URL = 'https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip'


def _looks_like_rom_pack(path: Path) -> bool:
    return (path / 'blargg').exists() and (path / 'mooneye-test-suite').exists()


def ensure_public_test_roms(force: bool = False) -> Path:
    if CACHE_DIR.exists() and not force and _looks_like_rom_pack(CACHE_DIR):
        return CACHE_DIR

    CACHE_DIR.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pyemu-public-roms-') as temp_dir:
        temp_dir_path = Path(temp_dir)
        archive_path = temp_dir_path / 'game-boy-test-roms-v7.0.zip'
        print(f'downloading {ARCHIVE_URL}')
        urllib.request.urlretrieve(ARCHIVE_URL, archive_path)
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(temp_dir_path)
        extracted_roots = [p for p in temp_dir_path.iterdir() if p.is_dir()]
        if not extracted_roots:
            raise RuntimeError('downloaded archive did not contain an extracted test-rom directory')
        if _looks_like_rom_pack(temp_dir_path):
            extracted_root = temp_dir_path
        else:
            extracted_root = extracted_roots[0]
            if not _looks_like_rom_pack(extracted_root):
                raise RuntimeError(f'downloaded archive does not look like the compiled rom pack: {extracted_root}')
        if CACHE_DIR.exists():
            shutil.rmtree(CACHE_DIR)
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        if extracted_root == temp_dir_path:
            for child in temp_dir_path.iterdir():
                if child == archive_path:
                    continue
                shutil.move(str(child), str(CACHE_DIR / child.name))
        else:
            shutil.move(str(extracted_root), str(CACHE_DIR))
    return CACHE_DIR


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--force', action='store_true', help='re-download even if a cached pack already exists')
    args = parser.parse_args(argv)
    path = ensure_public_test_roms(force=args.force)
    print(path)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
