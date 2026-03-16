from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass

from PySide6.QtCore import Qt
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QLabel, QMainWindow, QSizePolicy, QVBoxLayout, QWidget

from .runtime import FrameBuffer, SystemInfo

try:
    import pygame
except ImportError:
    pygame = None


class FrameBufferWidget(QWidget):
    def __init__(self, system: SystemInfo, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._system = system
        self._image = QImage()
        self._label = QLabel("No frame available")
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._label)

        self.setMinimumSize(system.screen_width * 2, system.screen_height * 2)

    def update_frame(self, frame: FrameBuffer) -> None:
        rgba = bytes(frame.rgba or [])
        expected_size = frame.width * frame.height * 4
        if expected_size == 0 or len(rgba) != expected_size:
            self._image = QImage()
            self._label.setText("No frame available")
            self._label.setPixmap(QPixmap())
            return

        image = QImage(rgba, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGBA8888)
        self._image = image.copy()
        self._refresh_pixmap()

    def resizeEvent(self, event) -> None:  # type: ignore[override]
        super().resizeEvent(event)
        self._refresh_pixmap()

    def _refresh_pixmap(self) -> None:
        if self._image.isNull():
            return
        scaled = QPixmap.fromImage(self._image).scaled(
            self._label.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.FastTransformation,
        )
        self._label.setText("")
        self._label.setPixmap(scaled)


@dataclass(frozen=True)
class DisplayBackendInfo:
    key: str
    label: str


class DisplayBackend:
    info: DisplayBackendInfo

    def show(self) -> None:
        raise NotImplementedError

    def focus(self) -> None:
        self.show()

    def present(self, frame: FrameBuffer) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    def reset_state(self) -> None:
        self.close()

    def poll_input(self) -> tuple[int, int]:
        return 0x0F, 0x0F

    def poll_actions(self) -> set[str]:
        return set()


class QtDisplayWindow(QMainWindow, DisplayBackend):
    def __init__(self, system: SystemInfo, title: str) -> None:
        super().__init__()
        self.info = DisplayBackendInfo("qt", "Qt")
        self._frame_widget = FrameBufferWidget(system)
        self.setWindowTitle(title)
        self.resize(420, 420)
        self.setCentralWidget(self._frame_widget)

    def focus(self) -> None:
        self.show()
        self.raise_()
        self.activateWindow()

    def present(self, frame: FrameBuffer) -> None:
        self._frame_widget.update_frame(frame)

    def reset_state(self) -> None:
        self._frame_widget.update_frame(FrameBuffer(width=self._frame_widget._system.screen_width, height=self._frame_widget._system.screen_height, rgba=[]))


class PygameDisplayBackend(DisplayBackend):
    _BUTTON_KEYS = {
        pygame.K_z: ("buttons", 0x01),
        pygame.K_x: ("buttons", 0x02),
        pygame.K_BACKSPACE: ("buttons", 0x04),
        pygame.K_RETURN: ("buttons", 0x08),
    }
    _DIRECTION_KEYS = {
        pygame.K_RIGHT: ("directions", 0x01),
        pygame.K_LEFT: ("directions", 0x02),
        pygame.K_UP: ("directions", 0x04),
        pygame.K_DOWN: ("directions", 0x08),
    }

    def __init__(self, system: SystemInfo, title: str, scale: int = 3) -> None:
        if pygame is None:
            raise RuntimeError("pygame is not installed")

        self.info = DisplayBackendInfo("pygame", "pygame")
        self._system = system
        self._title = title
        self._scale = max(1, scale)
        self._window_size = (system.screen_width * self._scale, system.screen_height * self._scale)
        self._screen = None
        self._frame_surface = None
        self._present_surface = None
        self._last_frame = None
        self._buttons = 0x0F
        self._directions = 0x0F
        self._pending_actions: set[str] = set()

        if not pygame.get_init():
            pygame.init()
        if not pygame.display.get_init():
            pygame.display.init()

    def _create_screen(self, size: tuple[int, int] | None = None):
        window_size = size or self._window_size
        flags = pygame.RESIZABLE | pygame.DOUBLEBUF
        if hasattr(pygame, "HWSURFACE"):
            flags |= pygame.HWSURFACE
        try:
            screen = pygame.display.set_mode(window_size, flags, vsync=1)
        except TypeError:
            screen = pygame.display.set_mode(window_size, flags)
        except pygame.error:
            screen = pygame.display.set_mode(window_size, flags)
        self._window_size = screen.get_size()
        self._recreate_surfaces()
        return screen

    def _recreate_surfaces(self) -> None:
        self._frame_surface = pygame.Surface((self._system.screen_width, self._system.screen_height), flags=pygame.SRCALPHA, depth=32)
        if self._screen is not None:
            self._present_surface = pygame.Surface(self._screen.get_size()).convert()
        else:
            self._present_surface = None

    def show(self) -> None:
        if not pygame.display.get_init():
            pygame.display.init()
        if self._screen is None:
            self._screen = self._create_screen()
            pygame.display.set_caption(self._title)
            if self._last_frame is not None:
                self.present(self._last_frame)
            else:
                self._clear_screen()
        else:
            pygame.display.set_caption(self._title)

    def _clear_screen(self) -> None:
        if self._screen is None:
            return
        self._screen.fill((0, 0, 0))
        pygame.display.flip()

    def focus(self) -> None:
        self.show()
        if os.name != "nt":
            return
        if self._screen is None:
            return
        try:
            hwnd = pygame.display.get_wm_info().get("window")
        except Exception:
            hwnd = None
        if not hwnd:
            return
        user32 = ctypes.windll.user32
        SW_RESTORE = 9
        user32.ShowWindow(hwnd, SW_RESTORE)
        user32.SetForegroundWindow(hwnd)
        user32.SetActiveWindow(hwnd)


    def present(self, frame: FrameBuffer) -> None:
        self._last_frame = frame
        if self._screen is None:
            return

        self._pump_events()
        rgba = bytes(frame.rgba or [])
        expected_size = frame.width * frame.height * 4
        if len(rgba) != expected_size or expected_size == 0:
            self._clear_screen()
            return

        if self._frame_surface is None or self._present_surface is None:
            self._recreate_surfaces()
        if self._frame_surface is None or self._present_surface is None:
            return

        source_surface = pygame.image.frombuffer(rgba, (frame.width, frame.height), "RGBA")
        self._frame_surface.blit(source_surface, (0, 0))
        pygame.transform.scale(self._frame_surface, self._screen.get_size(), self._present_surface)
        self._screen.blit(self._present_surface, (0, 0))
        pygame.display.flip()

    def close(self) -> None:
        if self._screen is not None:
            pygame.display.quit()
            self._screen = None
        self._frame_surface = None
        self._present_surface = None

    def reset_state(self) -> None:
        self.close()
        self._last_frame = None
        self._buttons = 0x0F
        self._directions = 0x0F
        self._pending_actions.clear()

    def poll_input(self) -> tuple[int, int]:
        if self._screen is None:
            return 0x0F, 0x0F

        self._pump_events()
        return self._buttons & 0x0F, self._directions & 0x0F

    def poll_actions(self) -> set[str]:
        if self._screen is not None:
            self._pump_events()
        actions = set(self._pending_actions)
        self._pending_actions.clear()
        return actions

    def _update_key_state(self, key: int, pressed: bool) -> None:
        mapping = self._BUTTON_KEYS.get(key)
        bank = None
        mask = 0
        if mapping is not None:
            bank, mask = mapping
        else:
            mapping = self._DIRECTION_KEYS.get(key)
            if mapping is not None:
                bank, mask = mapping

        if bank is None:
            return

        if bank == "buttons":
            if pressed:
                self._buttons &= ~mask
            else:
                self._buttons |= mask
        else:
            if pressed:
                self._directions &= ~mask
            else:
                self._directions |= mask

    def _pump_events(self) -> None:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                self.close()
            elif event.type == pygame.VIDEORESIZE and self._screen is not None:
                self._screen = self._create_screen(event.size)
                pygame.display.set_caption(self._title)
                if self._last_frame is not None:
                    self.present(self._last_frame)
                else:
                    self._clear_screen()
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    self._pending_actions.add("start_trace")
                elif event.key == pygame.K_e:
                    self._pending_actions.add("stop_trace")
                self._update_key_state(event.key, True)
            elif event.type == pygame.KEYUP:
                self._update_key_state(event.key, False)
            elif event.type == pygame.WINDOWFOCUSLOST:
                self._buttons = 0x0F
                self._directions = 0x0F
                self._pending_actions.clear()


def create_display_backend(system: SystemInfo, title: str) -> DisplayBackend:
    preferred = os.environ.get("PYEMU_DISPLAY_BACKEND", "pygame").strip().lower()

    if preferred == "qt":
        return QtDisplayWindow(system, title)
    if preferred == "pygame" and pygame is not None:
        return PygameDisplayBackend(system, title)
    return QtDisplayWindow(system, title)
