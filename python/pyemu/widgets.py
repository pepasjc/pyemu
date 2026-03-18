from __future__ import annotations

from PySide6.QtCore import Qt, QSize
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QLabel, QSizePolicy, QVBoxLayout, QWidget

from .runtime import FrameBuffer, SystemInfo

PALETTE_PRESETS: dict[str, tuple[str, tuple[tuple[int, int, int, int], ...]]] = {
    "gray": ("DMG Gray", ((255, 255, 255, 255), (170, 170, 170, 255), (85, 85, 85, 255), (0, 0, 0, 255))),
    "green": ("DMG Green", ((155, 188, 15, 255), (139, 172, 15, 255), (48, 98, 48, 255), (15, 56, 15, 255))),
    "pocket": ("Pocket", ((224, 248, 208, 255), (136, 192, 112, 255), (52, 104, 86, 255), (8, 24, 32, 255))),
    "blue": ("Cool Blue", ((234, 244, 255, 255), (146, 184, 255, 255), (74, 106, 176, 255), (24, 32, 72, 255))),
    "amber": ("Amber", ((255, 245, 224, 255), (255, 196, 128, 255), (176, 96, 32, 255), (56, 24, 0, 255))),
}


class FrameBufferWidget(QWidget):
    def __init__(self, system: SystemInfo, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._system = system
        self._image = QImage()
        self._last_frame: FrameBuffer | None = None
        self._scale = 4
        self._palette_key = "gray"
        self._label = QLabel("No frame available")
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._label)

    def update_frame(self, frame: FrameBuffer) -> None:
        self._last_frame = frame
        rgba = bytes(frame.rgba or [])
        expected_size = frame.width * frame.height * 4
        if expected_size == 0 or len(rgba) != expected_size:
            self._image = QImage()
            self._label.setText("No frame available")
            self._label.setPixmap(QPixmap())
            return

        mapped = self._map_palette(rgba)
        image = QImage(mapped, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGBA8888)
        self._image = image.copy()
        self._refresh_pixmap()

    def resizeEvent(self, event) -> None:  # type: ignore[override]
        super().resizeEvent(event)
        self._refresh_pixmap()

    def set_scale(self, scale: int) -> None:
        self._scale = max(1, scale)
        self.updateGeometry()

    def set_palette(self, palette_key: str) -> None:
        if palette_key not in PALETTE_PRESETS:
            palette_key = "gray"
        if palette_key == self._palette_key:
            return
        self._palette_key = palette_key
        if self._last_frame is not None:
            self.update_frame(self._last_frame)

    def palette_key(self) -> str:
        return self._palette_key

    def sizeHint(self) -> QSize:  # type: ignore[override]
        return QSize(self._system.screen_width * self._scale, self._system.screen_height * self._scale)

    def _map_palette(self, rgba: bytes) -> bytes:
        colors = PALETTE_PRESETS.get(self._palette_key, PALETTE_PRESETS["gray"])[1]
        if self._palette_key == "gray":
            return rgba
        mapped = bytearray(rgba)
        for index in range(0, len(mapped), 4):
            shade = mapped[index]
            if shade >= 213:
                palette_index = 0
            elif shade >= 128:
                palette_index = 1
            elif shade >= 43:
                palette_index = 2
            else:
                palette_index = 3
            r, g, b, a = colors[palette_index]
            mapped[index] = r
            mapped[index + 1] = g
            mapped[index + 2] = b
            mapped[index + 3] = a
        return bytes(mapped)

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
