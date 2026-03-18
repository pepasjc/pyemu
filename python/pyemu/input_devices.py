from __future__ import annotations

from typing import Iterable

try:
    import pygame
except ImportError:
    pygame = None


class JoystickManager:
    def __init__(self) -> None:
        self.available = pygame is not None
        self._joystick = None
        if pygame is not None:
            if not pygame.get_init():
                pygame.init()
            if not pygame.joystick.get_init():
                pygame.joystick.init()

    def poll(self, action_keys: Iterable[str], bindings: dict[str, str]) -> dict[str, bool]:
        states = {key: False for key in action_keys}
        if pygame is None:
            return states
        self._ensure_joystick()
        if self._joystick is None:
            return states
        try:
            pygame.event.pump()
        except Exception:
            return states
        for action_key in states:
            binding = bindings.get(action_key, "")
            states[action_key] = self._binding_active(binding)
        return states

    def _ensure_joystick(self) -> None:
        if pygame is None:
            self._joystick = None
            return
        count = pygame.joystick.get_count()
        if count <= 0:
            self._joystick = None
            return
        if self._joystick is None:
            joystick = pygame.joystick.Joystick(0)
            joystick.init()
            self._joystick = joystick

    def _binding_active(self, binding: str) -> bool:
        if self._joystick is None or not binding:
            return False
        for part in binding.split("|"):
            if self._binding_part_active(part.strip()):
                return True
        return False

    def _binding_part_active(self, binding: str) -> bool:
        if self._joystick is None or not binding:
            return False
        try:
            if binding.startswith("button:"):
                index = int(binding.split(":", 1)[1])
                return bool(self._joystick.get_button(index))
            if binding.startswith("hat"):
                hat_name, direction = binding.split(":", 1)
                hat_index = int(hat_name[3:])
                x, y = self._joystick.get_hat(hat_index)
                direction = direction.lower()
                return ((direction == "left" and x < 0) or
                        (direction == "right" and x > 0) or
                        (direction == "up" and y > 0) or
                        (direction == "down" and y < 0))
            if binding.startswith("axis"):
                axis_name, sign = binding.split(":", 1)
                axis_index = int(axis_name[4:])
                value = float(self._joystick.get_axis(axis_index))
                return (sign == "+" and value > 0.5) or (sign == "-" and value < -0.5)
        except Exception:
            return False
        return False
