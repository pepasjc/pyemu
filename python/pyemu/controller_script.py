from __future__ import annotations

import importlib.util
import sys
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any

BUTTON_A = 0x01
BUTTON_B = 0x02
BUTTON_SELECT = 0x04
BUTTON_START = 0x08
DIR_RIGHT = 0x01
DIR_LEFT = 0x02
DIR_UP = 0x04
DIR_DOWN = 0x08

BUTTON_NAME_TO_MASK = {
    "A": BUTTON_A,
    "B": BUTTON_B,
    "SELECT": BUTTON_SELECT,
    "START": BUTTON_START,
}

DIRECTION_NAME_TO_MASK = {
    "RIGHT": DIR_RIGHT,
    "LEFT": DIR_LEFT,
    "UP": DIR_UP,
    "DOWN": DIR_DOWN,
}

ALL_BUTTONS = 0x0F
ALL_DIRECTIONS = 0x0F


@dataclass(frozen=True)
class ScriptInput:
    buttons: int = ALL_BUTTONS
    directions: int = ALL_DIRECTIONS
    actions: tuple[str, ...] = ()


class ScriptAPI:
    def __init__(self, emulator, frame_index: int) -> None:
        self._emulator = emulator
        self.frame_index = frame_index

    @property
    def system_actions(self) -> tuple[str, ...]:
        return tuple(action.key for action in self._emulator.system.input_actions)

    @property
    def cpu(self):
        return self._emulator.cpu_state

    @property
    def media(self):
        return self._emulator.media

    @property
    def cycle_count(self) -> int:
        return self._emulator.cycle_count

    def memory_snapshot(self) -> list[int]:
        return self._emulator.memory_snapshot()

    def peek8(self, address: int) -> int:
        memory = self._emulator.memory_snapshot()
        if 0 <= address < len(memory):
            return memory[address]
        return 0

    def peek16(self, address: int) -> int:
        low = self.peek8(address)
        high = self.peek8(address + 1)
        return low | (high << 8)

    @property
    def frame_buffer(self):
        return self._emulator.frame_buffer

    def buttons(self, *names: str) -> int:
        mask = ALL_BUTTONS
        for name in names:
            value = BUTTON_NAME_TO_MASK.get(str(name).upper())
            if value is not None:
                mask &= ~value
        return mask

    def directions(self, *names: str) -> int:
        mask = ALL_DIRECTIONS
        for name in names:
            value = DIRECTION_NAME_TO_MASK.get(str(name).upper())
            if value is not None:
                mask &= ~value
        return mask

    def input(self, *, buttons: int = ALL_BUTTONS, directions: int = ALL_DIRECTIONS, actions: tuple[str, ...] | list[str] = ()) -> ScriptInput:
        normalized_actions = tuple(str(action).lower() for action in actions)
        return ScriptInput(buttons=buttons & 0x0F, directions=directions & 0x0F, actions=normalized_actions)

    def actions(self, *names: str) -> ScriptInput:
        return ScriptInput(actions=tuple(str(name).lower() for name in names))

    def press(self, *names: str) -> ScriptInput:
        buttons = ALL_BUTTONS
        directions = ALL_DIRECTIONS
        action_names: list[str] = []
        for name in names:
            key = str(name).upper()
            if key in BUTTON_NAME_TO_MASK:
                buttons &= ~BUTTON_NAME_TO_MASK[key]
                action_names.append(key.lower())
            elif key in DIRECTION_NAME_TO_MASK:
                directions &= ~DIRECTION_NAME_TO_MASK[key]
                action_names.append(key.lower())
            else:
                action_names.append(str(name).lower())
        return ScriptInput(buttons=buttons, directions=directions, actions=tuple(action_names))


class ControllerScriptHost:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.module: ModuleType | None = None
        self.frame_index = 0
        self._load_module()

    def _load_module(self) -> None:
        module_name = f"pyemu_controller_{self.path.stem}_{abs(hash(self.path))}"
        spec = importlib.util.spec_from_file_location(module_name, self.path)
        if spec is None or spec.loader is None:
            raise ValueError(f"Could not load controller script from {self.path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        self.module = module

    @property
    def display_name(self) -> str:
        return self.path.name

    def reset(self, emulator) -> None:
        self.frame_index = 0
        if self.module is not None and hasattr(self.module, "on_reset"):
            self.module.on_reset(ScriptAPI(emulator, self.frame_index))

    def unload(self, emulator) -> None:
        if self.module is not None and hasattr(self.module, "on_unload"):
            self.module.on_unload(ScriptAPI(emulator, self.frame_index))

    def next_input(self, emulator) -> ScriptInput:
        if self.module is None or not hasattr(self.module, "on_frame"):
            return ScriptInput()
        api = ScriptAPI(emulator, self.frame_index)
        result = self.module.on_frame(api)
        self.frame_index += 1
        return self._normalize_result(result)

    def _normalize_result(self, result: Any) -> ScriptInput:
        if result is None:
            return ScriptInput()
        if isinstance(result, ScriptInput):
            return ScriptInput(result.buttons & 0x0F, result.directions & 0x0F, tuple(str(name).lower() for name in result.actions))
        if isinstance(result, tuple) and len(result) == 2 and all(isinstance(value, int) for value in result):
            return ScriptInput(int(result[0]) & 0x0F, int(result[1]) & 0x0F)
        if isinstance(result, dict):
            buttons = self._parse_side(result.get("buttons"), BUTTON_NAME_TO_MASK, ALL_BUTTONS)
            directions = self._parse_side(result.get("directions"), DIRECTION_NAME_TO_MASK, ALL_DIRECTIONS)
            actions = self._parse_actions(result.get("actions"))
            return ScriptInput(buttons, directions, actions)
        if isinstance(result, (list, set, tuple)):
            buttons = ALL_BUTTONS
            directions = ALL_DIRECTIONS
            actions: list[str] = []
            for item in result:
                key = str(item).upper()
                if key in BUTTON_NAME_TO_MASK:
                    buttons &= ~BUTTON_NAME_TO_MASK[key]
                    actions.append(key.lower())
                elif key in DIRECTION_NAME_TO_MASK:
                    directions &= ~DIRECTION_NAME_TO_MASK[key]
                    actions.append(key.lower())
                else:
                    actions.append(str(item).lower())
            return ScriptInput(buttons, directions, tuple(actions))
        raise ValueError(f"Unsupported controller script result: {type(result)!r}")

    def _parse_side(self, value: Any, mapping: dict[str, int], neutral: int) -> int:
        if value is None:
            return neutral
        if isinstance(value, int):
            return value & 0x0F
        if isinstance(value, str):
            value = [value]
        if isinstance(value, (list, tuple, set)):
            mask = neutral
            for item in value:
                key = str(item).upper()
                if key in mapping:
                    mask &= ~mapping[key]
            return mask
        return neutral

    def _parse_actions(self, value: Any) -> tuple[str, ...]:
        if value is None:
            return ()
        if isinstance(value, str):
            return (value.lower(),)
        if isinstance(value, (list, tuple, set)):
            return tuple(str(item).lower() for item in value)
        return ()
