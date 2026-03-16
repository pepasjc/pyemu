from __future__ import annotations

import sys
import tempfile
import zipfile
from contextlib import suppress
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path, PurePosixPath

from cffi import FFI


class RunState(IntEnum):
    STOPPED = 0
    PAUSED = 1
    RUNNING = 2


@dataclass
class CPUState:
    pc: int = 0
    sp: int = 0
    a: int = 0
    f: int = 0
    b: int = 0
    c: int = 0
    d: int = 0
    e: int = 0
    h: int = 0
    l: int = 0
    halted: bool = False
    ime: bool = False


@dataclass
class LastBusAccess:
    address: int = 0
    value: int = 0
    is_write: bool = False
    valid: bool = False


@dataclass
class CartridgeDebugInfo:
    cartridge_type: int = 0
    rom_size_code: int = 0
    ram_size_code: int = 0
    ram_enabled: bool = False
    rom_bank: int = 0
    ram_bank: int = 0
    banking_mode: int = 0
    has_battery: bool = False
    save_file_present: bool = False
    last_mapper_value: int = 0
    last_mapper_valid: bool = False
    last_mapper_address: int = 0
    rom_bank_count: int = 0
    ram_bank_count: int = 0


@dataclass
class FrameBuffer:
    width: int = 160
    height: int = 144
    rgba: list[int] | None = None

    def __post_init__(self) -> None:
        if self.rgba is None:
            self.rgba = [0] * (self.width * self.height * 4)


@dataclass(frozen=True)
class SystemInfo:
    key: str
    display_name: str
    screen_width: int
    screen_height: int
    cpu_hz: int
    frame_rate: float
    media_label: str
    media_extensions: tuple[str, ...]

    @property
    def file_dialog_filter(self) -> str:
        patterns = " ".join(f"*.{extension}" for extension in self.media_extensions)
        return f"{self.media_label} ({patterns});;All files (*.*)"


@dataclass
class MediaInfo:
    loaded: bool = False
    path: str = ""
    title: str = ""
    size_bytes: int = 0


SUPPORTED_SYSTEMS: dict[str, SystemInfo] = {
    "gameboy": SystemInfo(
        key="gameboy",
        display_name="Nintendo Game Boy",
        screen_width=160,
        screen_height=144,
        cpu_hz=4_194_304,
        frame_rate=59.7275,
        media_label="Game Boy ROM",
        media_extensions=("gb", "gbc", "zip"),
    )
}


class Emulator:
    def __init__(self, system: str = "gameboy") -> None:
        if system not in SUPPORTED_SYSTEMS:
            available = ", ".join(sorted(SUPPORTED_SYSTEMS))
            raise ValueError(f"Unsupported system '{system}'. Available systems: {available}")

        self._system = SUPPORTED_SYSTEMS[system]
        self._temp_dir = tempfile.TemporaryDirectory(prefix="pyemu-media-")
        native = _load_native_library()
        if native is None:
            self._backend = _FallbackEmulator(self._system)
            self.native_available = False
        else:
            ffi, lib = native
            self._backend = _NativeEmulator(self._system, ffi, lib)
            self.native_available = True

    @property
    def system(self) -> SystemInfo:
        return self._system

    @property
    def system_name(self) -> str:
        return self._backend.system_name

    @property
    def run_state(self) -> RunState:
        return self._backend.run_state

    @property
    def cpu_state(self) -> CPUState:
        return self._backend.cpu_state

    @property
    def frame_buffer(self) -> FrameBuffer:
        return self._backend.frame_buffer

    @property
    def media(self) -> MediaInfo:
        return self._backend.media

    @property
    def cycle_count(self) -> int:
        return self._backend.cycle_count

    @property
    def faulted(self) -> bool:
        return self._backend.faulted

    @property
    def cartridge(self) -> MediaInfo:
        return self.media

    def reset(self) -> None:
        self._backend.reset()

    def load_rom(self, path: str) -> bool:
        return self.load_media(path)

    def load_media(self, path: str) -> bool:
        resolved_path = Path(path).expanduser().resolve(strict=False)
        load_path = self._prepare_media_path(resolved_path)
        return self._backend.load_media(str(load_path))

    def save_state(self, path: str) -> bool:
        target_path = Path(path).expanduser().resolve(strict=False)
        return self._backend.save_state(str(target_path))

    def load_state(self, path: str) -> bool:
        source_path = Path(path).expanduser().resolve(strict=False)
        return self._backend.load_state(str(source_path))

    def run(self) -> None:
        self._backend.run()

    def pause(self) -> None:
        self._backend.pause()

    def stop(self) -> None:
        self._backend.stop()

    def step_instruction(self) -> None:
        self._backend.step_instruction()

    def step_frame(self) -> None:
        self._backend.step_frame()

    def memory_snapshot(self) -> list[int]:
        return self._backend.memory_snapshot()

    def set_gameboy_joypad_state(self, buttons: int, directions: int) -> None:
        if hasattr(self._backend, "set_gameboy_joypad_state"):
            self._backend.set_gameboy_joypad_state(buttons, directions)

    def set_bus_tracking(self, enabled: bool) -> None:
        if hasattr(self._backend, "set_bus_tracking"):
            self._backend.set_bus_tracking(enabled)

    @property
    def cartridge_debug(self) -> CartridgeDebugInfo:
        if hasattr(self._lib, "pyemu_get_cartridge_debug_info"):
            info = self._lib.pyemu_get_cartridge_debug_info(self._handle)
            return CartridgeDebugInfo(
                cartridge_type=int(info.cartridge_type),
                rom_size_code=int(info.rom_size_code),
                ram_size_code=int(info.ram_size_code),
                ram_enabled=bool(info.ram_enabled),
                rom_bank=int(info.rom_bank),
                ram_bank=int(info.ram_bank),
                banking_mode=int(info.banking_mode),
                has_battery=bool(info.has_battery),
                save_file_present=bool(info.save_file_present),
                last_mapper_value=int(info.last_mapper_value),
                last_mapper_valid=bool(info.last_mapper_valid),
                last_mapper_address=int(info.last_mapper_address),
                rom_bank_count=int(info.rom_bank_count),
                ram_bank_count=int(info.ram_bank_count),
            )
        return CartridgeDebugInfo()

    @property
    def last_bus_access(self) -> LastBusAccess:
        if hasattr(self._backend, "last_bus_access"):
            return self._backend.last_bus_access
        return LastBusAccess()

    @property
    def cartridge_debug(self) -> CartridgeDebugInfo:
        if hasattr(self._backend, "cartridge_debug"):
            return self._backend.cartridge_debug
        return CartridgeDebugInfo()

    def _prepare_media_path(self, path: Path) -> Path:
        if path.suffix.lower() != ".zip":
            return path

        rom_extensions = {f".{extension.lower()}" for extension in self._system.media_extensions if extension.lower() != "zip"}
        with zipfile.ZipFile(path) as archive:
            candidates = [
                member
                for member in archive.infolist()
                if not member.is_dir() and PurePosixPath(member.filename).suffix.lower() in rom_extensions
            ]
            if not candidates:
                raise ValueError(f"No supported {self._system.media_label.lower()} found inside {path.name}.")

            candidates.sort(key=lambda member: (PurePosixPath(member.filename).parts.__len__(), member.file_size, member.filename.lower()))
            member = candidates[0]
            target_dir = Path(self._temp_dir.name) / path.stem
            target_dir.mkdir(parents=True, exist_ok=True)
            target_path = target_dir / PurePosixPath(member.filename).name
            with archive.open(member) as source, target_path.open("wb") as destination:
                destination.write(source.read())
            return target_path


class _NativeEmulator:
    def __init__(self, system: SystemInfo, ffi: FFI, lib) -> None:
        self._system = system
        self._ffi = ffi
        self._lib = lib
        self._handle = _create_native_emulator(ffi, lib, system.key)
        if self._handle == ffi.NULL:
            raise RuntimeError(f"Failed to create native emulator for {system.key}")

    @property
    def system_name(self) -> str:
        name = self._lib.pyemu_get_system_name(self._handle)
        return self._ffi.string(name).decode("utf-8")

    @property
    def run_state(self) -> RunState:
        return RunState(int(self._lib.pyemu_get_run_state(self._handle)))

    @property
    def cpu_state(self) -> CPUState:
        state = self._lib.pyemu_get_cpu_state(self._handle)
        return CPUState(
            pc=int(state.pc),
            sp=int(state.sp),
            a=int(state.a),
            f=int(state.f),
            b=int(state.b),
            c=int(state.c),
            d=int(state.d),
            e=int(state.e),
            h=int(state.h),
            l=int(state.l),
            halted=bool(state.halted),
        )

    @property
    def frame_buffer(self) -> FrameBuffer:
        frame = self._lib.pyemu_get_frame_buffer(self._handle)
        rgba = []
        if frame.rgba != self._ffi.NULL and int(frame.rgba_size) > 0:
            rgba = list(bytes(self._ffi.buffer(frame.rgba, int(frame.rgba_size))))
        return FrameBuffer(width=int(frame.width), height=int(frame.height), rgba=rgba)

    @property
    def media(self) -> MediaInfo:
        title_ptr = self._lib.pyemu_get_cartridge_title(self._handle)
        path_ptr = self._lib.pyemu_get_rom_path(self._handle)
        return MediaInfo(
            loaded=bool(self._lib.pyemu_has_rom_loaded(self._handle)),
            path=self._ffi.string(path_ptr).decode("utf-8") if path_ptr != self._ffi.NULL else "",
            title=self._ffi.string(title_ptr).decode("utf-8") if title_ptr != self._ffi.NULL else "",
            size_bytes=int(self._lib.pyemu_get_rom_size(self._handle)),
        )

    @property
    def cycle_count(self) -> int:
        return int(self._lib.pyemu_get_cycle_count(self._handle))

    @property
    def faulted(self) -> bool:
        if hasattr(self._lib, "pyemu_is_faulted"):
            return bool(self._lib.pyemu_is_faulted(self._handle))
        return False

    def reset(self) -> None:
        self._lib.pyemu_reset(self._handle)

    def load_media(self, path: str) -> bool:
        encoded = self._ffi.new("char[]", path.encode("utf-8"))
        return bool(self._lib.pyemu_load_rom(self._handle, encoded))

    def save_state(self, path: str) -> bool:
        if not hasattr(self._lib, "pyemu_save_state"):
            return False
        encoded = self._ffi.new("char[]", path.encode("utf-8"))
        return bool(self._lib.pyemu_save_state(self._handle, encoded))

    def load_state(self, path: str) -> bool:
        if not hasattr(self._lib, "pyemu_load_state"):
            return False
        encoded = self._ffi.new("char[]", path.encode("utf-8"))
        return bool(self._lib.pyemu_load_state(self._handle, encoded))

    def run(self) -> None:
        self._lib.pyemu_run(self._handle)

    def pause(self) -> None:
        self._lib.pyemu_pause(self._handle)

    def stop(self) -> None:
        self._lib.pyemu_stop(self._handle)

    def step_instruction(self) -> None:
        self._lib.pyemu_step_instruction(self._handle)

    def step_frame(self) -> None:
        self._lib.pyemu_step_frame(self._handle)

    def memory_snapshot(self) -> list[int]:
        size_ptr = self._ffi.new("size_t *")
        memory = self._lib.pyemu_get_memory(self._handle, size_ptr)
        size = int(size_ptr[0])
        if memory == self._ffi.NULL or size == 0:
            return []
        return list(bytes(self._ffi.buffer(memory, size)))

    def set_gameboy_joypad_state(self, buttons: int, directions: int) -> None:
        if hasattr(self._lib, "pyemu_set_gameboy_joypad_state"):
            self._lib.pyemu_set_gameboy_joypad_state(self._handle, int(buttons) & 0x0F, int(directions) & 0x0F)

    def set_bus_tracking(self, enabled: bool) -> None:
        if hasattr(self._lib, "pyemu_set_bus_tracking"):
            self._lib.pyemu_set_bus_tracking(self._handle, 1 if enabled else 0)

    @property
    def cartridge_debug(self) -> CartridgeDebugInfo:
        if hasattr(self._lib, "pyemu_get_cartridge_debug_info"):
            info = self._lib.pyemu_get_cartridge_debug_info(self._handle)
            return CartridgeDebugInfo(
                cartridge_type=int(info.cartridge_type),
                rom_size_code=int(info.rom_size_code),
                ram_size_code=int(info.ram_size_code),
                ram_enabled=bool(info.ram_enabled),
                rom_bank=int(info.rom_bank),
                ram_bank=int(info.ram_bank),
                banking_mode=int(info.banking_mode),
                has_battery=bool(info.has_battery),
                save_file_present=bool(info.save_file_present),
                last_mapper_value=int(info.last_mapper_value),
                last_mapper_valid=bool(info.last_mapper_valid),
                last_mapper_address=int(info.last_mapper_address),
                rom_bank_count=int(info.rom_bank_count),
                ram_bank_count=int(info.ram_bank_count),
            )
        return CartridgeDebugInfo()

    @property
    def last_bus_access(self) -> LastBusAccess:
        if hasattr(self._lib, "pyemu_get_last_bus_access"):
            access = self._lib.pyemu_get_last_bus_access(self._handle)
            return LastBusAccess(
                address=int(access.address),
                value=int(access.value),
                is_write=bool(access.is_write),
                valid=bool(access.valid),
            )
        return LastBusAccess()

    def __del__(self) -> None:
        handle = getattr(self, "_handle", None)
        ffi = getattr(self, "_ffi", None)
        lib = getattr(self, "_lib", None)
        if handle is not None and ffi is not None and lib is not None and handle != ffi.NULL:
            with suppress(Exception):
                lib.pyemu_destroy(handle)
            self._handle = ffi.NULL


class _FallbackEmulator:
    def __init__(self, system: SystemInfo) -> None:
        self._system = system
        self.system_name = system.key
        self.run_state = RunState.STOPPED
        self.cpu_state = CPUState(pc=0x0100, sp=0xFFFE)
        self.frame_buffer = FrameBuffer(width=system.screen_width, height=system.screen_height)
        self.media = MediaInfo()
        self._memory = [0] * 0x10000
        self.cycle_count = 0
        self.faulted = False

    def reset(self) -> None:
        self.run_state = RunState.PAUSED
        self.cpu_state = CPUState(pc=0x0100, sp=0xFFFE)
        self.frame_buffer = FrameBuffer(width=self._system.screen_width, height=self._system.screen_height)
        self._memory = [0] * 0x10000
        self.cycle_count = 0
        self.faulted = False

    def load_media(self, path: str) -> bool:
        self.run_state = RunState.PAUSED
        if path:
            title_bytes = b"TETRIS" if Path(path).stem.lower().startswith("tetris") else b"PYEMU DEMO"
            for offset, value in enumerate(title_bytes):
                self._memory[0x0134 + offset] = value
            self.media = MediaInfo(loaded=True, path=path, title=title_bytes.decode("ascii"), size_bytes=0x8000)
            return True
        return False

    def save_state(self, path: str) -> bool:
        return False

    def load_state(self, path: str) -> bool:
        return False

    def run(self) -> None:
        self.run_state = RunState.RUNNING

    def pause(self) -> None:
        self.run_state = RunState.PAUSED

    def stop(self) -> None:
        self.reset()
        self.run_state = RunState.STOPPED

    def step_instruction(self) -> None:
        opcode = self._memory[self.cpu_state.pc]
        self.cpu_state.pc = (self.cpu_state.pc + 1) & 0xFFFF
        self.cpu_state.a = opcode
        self._memory[0xC000] = opcode
        self.cycle_count += 4
        self.run_state = RunState.PAUSED

    def step_frame(self) -> None:
        rgba = self.frame_buffer.rgba or []
        for y in range(self.frame_buffer.height):
            for x in range(self.frame_buffer.width):
                pixel = (y * self.frame_buffer.width + x) * 4
                shade = self._memory[(x + y) & 0x7FFF] if self.media.loaded else (x + y + self.cpu_state.a) % 255
                rgba[pixel : pixel + 4] = [shade, shade, shade, 255]
        self.frame_buffer.rgba = rgba
        self.step_instruction()

    def memory_snapshot(self) -> list[int]:
        return list(self._memory)

    def set_gameboy_joypad_state(self, buttons: int, directions: int) -> None:
        self._memory[0xFF80] = buttons & 0x0F
        self._memory[0xFF81] = directions & 0x0F

    def set_bus_tracking(self, enabled: bool) -> None:
        return

    @property
    def cartridge_debug(self) -> CartridgeDebugInfo:
        if hasattr(self._lib, "pyemu_get_cartridge_debug_info"):
            info = self._lib.pyemu_get_cartridge_debug_info(self._handle)
            return CartridgeDebugInfo(
                cartridge_type=int(info.cartridge_type),
                rom_size_code=int(info.rom_size_code),
                ram_size_code=int(info.ram_size_code),
                ram_enabled=bool(info.ram_enabled),
                rom_bank=int(info.rom_bank),
                ram_bank=int(info.ram_bank),
                banking_mode=int(info.banking_mode),
                has_battery=bool(info.has_battery),
                save_file_present=bool(info.save_file_present),
                last_mapper_value=int(info.last_mapper_value),
                last_mapper_valid=bool(info.last_mapper_valid),
                last_mapper_address=int(info.last_mapper_address),
                rom_bank_count=int(info.rom_bank_count),
                ram_bank_count=int(info.ram_bank_count),
            )
        return CartridgeDebugInfo()

    @property
    def last_bus_access(self) -> LastBusAccess:
        return LastBusAccess()


def _create_native_emulator(ffi: FFI, lib, system_key: str):
    if hasattr(lib, "pyemu_create_emulator"):
        encoded = ffi.new("char[]", system_key.encode("utf-8"))
        return lib.pyemu_create_emulator(encoded)
    return lib.pyemu_create_gameboy()


def _load_native_library() -> tuple[FFI, object] | None:
    ffi = FFI()
    ffi.cdef(
        """
        typedef enum pyemu_run_state {
            PYEMU_RUN_STATE_STOPPED = 0,
            PYEMU_RUN_STATE_PAUSED = 1,
            PYEMU_RUN_STATE_RUNNING = 2
        } pyemu_run_state;

        typedef struct pyemu_cpu_state {
            uint16_t pc;
            uint16_t sp;
            uint8_t a;
            uint8_t f;
            uint8_t b;
            uint8_t c;
            uint8_t d;
            uint8_t e;
            uint8_t h;
            uint8_t l;
            uint8_t halted;
            uint8_t ime;
        } pyemu_cpu_state;

        typedef struct pyemu_frame_buffer {
            int width;
            int height;
            const uint8_t* rgba;
            size_t rgba_size;
        } pyemu_frame_buffer;

        typedef struct pyemu_last_bus_access {
            uint16_t address;
            uint8_t value;
            uint8_t is_write;
            uint8_t valid;
        } pyemu_last_bus_access;

        typedef struct pyemu_cartridge_debug_info {
            uint8_t cartridge_type;
            uint8_t rom_size_code;
            uint8_t ram_size_code;
            uint8_t ram_enabled;
            uint8_t rom_bank;
            uint8_t ram_bank;
            uint8_t banking_mode;
            uint8_t has_battery;
            uint8_t save_file_present;
            uint8_t last_mapper_value;
            uint8_t last_mapper_valid;
            uint8_t reserved[2];
            uint16_t last_mapper_address;
            uint32_t rom_bank_count;
            uint32_t ram_bank_count;
        } pyemu_cartridge_debug_info;

        typedef struct pyemu_emulator pyemu_emulator;

        size_t pyemu_get_supported_system_count(void);
        const char* pyemu_get_supported_system_key(size_t index);
        const char* pyemu_get_default_system_key(void);
        pyemu_emulator* pyemu_create_emulator(const char* system_key);
        pyemu_emulator* pyemu_create_gameboy(void);
        void pyemu_destroy(pyemu_emulator* emulator);
        void pyemu_reset(pyemu_emulator* emulator);
        int pyemu_load_rom(pyemu_emulator* emulator, const char* path);
        int pyemu_save_state(pyemu_emulator* emulator, const char* path);
        int pyemu_load_state(pyemu_emulator* emulator, const char* path);
        void pyemu_run(pyemu_emulator* emulator);
        void pyemu_pause(pyemu_emulator* emulator);
        void pyemu_stop(pyemu_emulator* emulator);
        void pyemu_step_instruction(pyemu_emulator* emulator);
        void pyemu_step_frame(pyemu_emulator* emulator);
        pyemu_run_state pyemu_get_run_state(const pyemu_emulator* emulator);
        const char* pyemu_get_system_name(const pyemu_emulator* emulator);
        pyemu_cpu_state pyemu_get_cpu_state(const pyemu_emulator* emulator);
        pyemu_frame_buffer pyemu_get_frame_buffer(const pyemu_emulator* emulator);
        const uint8_t* pyemu_get_memory(const pyemu_emulator* emulator, size_t* size);
        int pyemu_has_rom_loaded(const pyemu_emulator* emulator);
        const char* pyemu_get_rom_path(const pyemu_emulator* emulator);
        const char* pyemu_get_cartridge_title(const pyemu_emulator* emulator);
        size_t pyemu_get_rom_size(const pyemu_emulator* emulator);
        uint64_t pyemu_get_cycle_count(const pyemu_emulator* emulator);
        pyemu_last_bus_access pyemu_get_last_bus_access(const pyemu_emulator* emulator);
        pyemu_cartridge_debug_info pyemu_get_cartridge_debug_info(const pyemu_emulator* emulator);
        int pyemu_is_faulted(const pyemu_emulator* emulator);
        void pyemu_set_gameboy_joypad_state(pyemu_emulator* emulator, uint8_t buttons, uint8_t directions);
        void pyemu_set_bus_tracking(pyemu_emulator* emulator, int enabled);
        """
    )

    for candidate in _library_candidates():
        if candidate.exists():
            return ffi, ffi.dlopen(str(candidate))
    return None


def _library_candidates() -> list[Path]:
    package_root = Path(__file__).resolve().parent
    workspace_root = package_root.parent.parent
    if sys.platform.startswith("win"):
        return [
            workspace_root / "build" / "native" / "pyemu_native_timed103.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed97.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed96.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed86.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed85.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed76.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed75.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed74.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed73.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed72.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed71.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed70.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed69.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed68.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed67.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed66.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed65.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed64.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed63.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed62.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed61.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed60.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed59.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed58.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed57.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed56.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed55.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed44.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed45.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed43.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed42.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed41.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed40.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed39.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed38.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed37.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed36.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed35.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed34.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed33.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed32.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed31.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed30.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed29.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed28.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed27.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed26.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed25.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed24.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed23.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed22.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed21.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed20.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed19.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed18.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed17.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed16.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed15.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed14.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed13.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed12.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed11.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed10.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed9.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed8.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed7.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed6.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed5.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed4.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed3.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed2.dll",
            workspace_root / "build" / "native" / "pyemu_native_timed.dll",
            workspace_root / "build" / "native" / "pyemu_native.dll",
            workspace_root / "build" / "native" / "Debug" / "pyemu_native.dll",
            workspace_root / "build" / "native" / "Release" / "pyemu_native.dll",
            package_root / "_native" / "pyemu_native.dll",
            package_root / "pyemu_native.dll",
        ]
    return [
        workspace_root / "build" / "native" / "libpyemu_native.so",
        package_root / "_native" / "libpyemu_native.so",
        package_root / "libpyemu_native.so",
    ]
