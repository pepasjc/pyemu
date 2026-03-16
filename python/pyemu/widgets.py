from __future__ import annotations

from PySide6.QtCore import Qt, QSize
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QLabel, QSizePolicy, QVBoxLayout, QWidget

from .runtime import FrameBuffer, SystemInfo


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

    def sizeHint(self) -> QSize:  # type: ignore[override]
        return QSize(self._system.screen_width * 3, self._system.screen_height * 3)

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
