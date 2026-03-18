from __future__ import annotations

import json
from array import array
from datetime import datetime
import sys
import tempfile
import wave
from collections import deque
from pathlib import Path
from time import perf_counter

from PySide6.QtCore import QIODevice, QTimer, Qt
from PySide6.QtGui import QAction, QActionGroup, QColor, QKeyEvent, QKeySequence, QShortcut
from PySide6.QtMultimedia import QAudioDevice, QAudioFormat, QAudioSink, QMediaDevices
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QComboBox,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QKeySequenceEdit,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSplitter,
    QAbstractItemView,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QTextEdit,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from .controller_script import ControllerScriptHost, ScriptInput
from .input_devices import JoystickManager
from .runtime import AudioBuffer, Emulator, RunState
from .widgets import FrameBufferWidget, PALETTE_PRESETS


class AudioBufferDevice(QIODevice):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._buffer = bytearray()
        self._max_bytes = 0

    def set_limit(self, max_bytes: int) -> None:
        self._max_bytes = max(0, max_bytes)
        self._trim()

    def clear(self) -> None:
        self._buffer.clear()

    def push(self, payload: bytes) -> None:
        if not payload:
            return
        self._buffer.extend(payload)
        self._trim()

    def _trim(self) -> None:
        if self._max_bytes > 0 and len(self._buffer) > self._max_bytes:
            del self._buffer[: len(self._buffer) - self._max_bytes]

    def readData(self, maxlen: int) -> bytes:
        if not self.isOpen():
            return b""
        if maxlen <= 0 or not self._buffer:
            return bytes(max(0, maxlen))
        chunk = bytes(self._buffer[:maxlen])
        del self._buffer[:len(chunk)]
        return chunk

    def writeData(self, data: bytes) -> int:
        self.push(bytes(data))
        return len(data)

    def bytesAvailable(self) -> int:
        return len(self._buffer) + super().bytesAvailable()


class AudioPlayer:
    def __init__(self, parent=None) -> None:
        self._sink = None
        self._device = None
        self._sample_rate = 0
        self._channels = 0
        self._parent = parent
        self._output_device: QAudioDevice | None = None
        self._format: QAudioFormat | None = None
        self._last_nonzero = False

    def _build_format(self, audio: AudioBuffer) -> QAudioFormat | None:
        output = QMediaDevices.defaultAudioOutput()
        if output.isNull():
            return None

        preferred = output.preferredFormat()
        if preferred.sampleRate() == audio.sample_rate and preferred.channelCount() == audio.channels:
            self._output_device = output
            return preferred

        fmt = QAudioFormat()
        fmt.setSampleRate(audio.sample_rate)
        fmt.setChannelCount(audio.channels)
        fmt.setSampleFormat(QAudioFormat.SampleFormat.Int16)
        if output.isFormatSupported(fmt):
            self._output_device = output
            return fmt
        return None

    def _bytes_per_second(self, fmt: QAudioFormat) -> int:
        bytes_per_sample = 4 if fmt.sampleFormat() == QAudioFormat.SampleFormat.Float else 2
        return fmt.sampleRate() * fmt.channelCount() * bytes_per_sample

    def _convert_payload(self, payload: bytes) -> bytes:
        if self._format is None or self._format.sampleFormat() == QAudioFormat.SampleFormat.Int16:
            return payload
        if self._format.sampleFormat() == QAudioFormat.SampleFormat.Float:
            samples = array('h')
            samples.frombytes(payload)
            converted = array('f', (sample / 32768.0 for sample in samples))
            return converted.tobytes()
        return payload

    def _ensure_sink(self, audio: AudioBuffer) -> None:
        if audio.sample_rate <= 0 or audio.channels <= 0:
            return
        if self._sink is not None and self._sample_rate == audio.sample_rate and self._channels == audio.channels:
            return
        self.stop()
        fmt = self._build_format(audio)
        if fmt is None or self._output_device is None:
            return
        self._format = fmt
        self._sink = QAudioSink(self._output_device, fmt, self._parent)
        self._sink.setBufferSize(max(self._bytes_per_second(fmt) // 3, len(self._convert_payload(audio.pcm16le)) * 8 if audio.pcm16le else 0))
        self._device = AudioBufferDevice(self._parent)
        self._device.set_limit(max(self._bytes_per_second(fmt), len(self._convert_payload(audio.pcm16le)) * 12 if audio.pcm16le else 0))
        self._device.open(QIODevice.OpenModeFlag.ReadOnly)
        self._sink.setVolume(1.0)
        self._sink.start(self._device)
        self._sample_rate = audio.sample_rate
        self._channels = audio.channels

    def push(self, audio: AudioBuffer) -> None:
        self._last_nonzero = bool(audio.pcm16le) and any(audio.pcm16le)
        if not audio.pcm16le:
            return
        self._ensure_sink(audio)
        if self._device is None:
            return
        self._device.push(self._convert_payload(audio.pcm16le))

    def reset(self) -> None:
        self._last_nonzero = False
        if self._device is not None:
            self._device.clear()
        if self._sink is not None:
            self._sink.reset()
            self._sink.start(self._device)

    def stop(self) -> None:
        sink = self._sink
        device = self._device
        self._last_nonzero = False
        self._sink = None
        self._device = None
        self._sample_rate = 0
        self._channels = 0
        self._output_device = None
        self._format = None
        if sink is not None:
            sink.stop()
            sink.reset()
            sink.deleteLater()
        if device is not None:
            device.clear()
            device.deleteLater()

    def status_lines(self) -> list[str]:
        device_name = self._output_device.description() if self._output_device is not None else 'none'
        if self._sink is None or self._format is None:
            return [
                f"Audio device: {device_name}",
                "Audio sink: inactive",
                f"Audio signal: {'yes' if self._last_nonzero else 'no'}",
            ]
        state = self._sink.state().name if hasattr(self._sink.state(), 'name') else str(int(self._sink.state()))
        error = self._sink.error().name if hasattr(self._sink.error(), 'name') else str(int(self._sink.error()))
        fmt = f"{self._format.sampleRate()} Hz / {self._format.channelCount()} ch / {self._format.sampleFormat().name}"
        queued = self._device.bytesAvailable() if self._device is not None else 0
        return [
            f"Audio device: {device_name}",
            f"Audio format: {fmt}",
            f"Audio sink: {state} / {error}",
            f"Audio queued: {queued} bytes",
            f"Audio signal: {'yes' if self._last_nonzero else 'no'}",
        ]


class ControlsDialog(QDialog):
    def __init__(self, system, key_bindings: dict[str, str], parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Configure Controls")
        self._edits: dict[str, QKeySequenceEdit] = {}

        layout = QVBoxLayout(self)
        form = QFormLayout()
        for action in system.input_actions:
            edit = QKeySequenceEdit(QKeySequence(key_bindings.get(action.key, "")), self)
            self._edits[action.key] = edit
            form.addRow(action.label, edit)
        layout.addLayout(form)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel, self)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def key_bindings(self) -> dict[str, str]:
        result: dict[str, str] = {}
        for action_key, edit in self._edits.items():
            result[action_key] = edit.keySequence().toString(QKeySequence.SequenceFormat.PortableText)
        return result


class EmulatorWindow(QMainWindow):
    def __init__(self, system: str = "gameboy") -> None:
        super().__init__()
        self.emulator = Emulator(system=system)
        self.setWindowTitle(f"pyemu | {self.emulator.system.display_name}")
        self.resize(720, 620)

        self.status_label = QLabel()
        self.hardware_label = QLabel()
        self.cartridge_label = QLabel()
        self.apu_label = QLabel()
        self.performance_label = QLabel()
        self._manual_action_states = {action.key: False for action in self.emulator.system.input_actions}
        self._joystick_action_states = {action.key: False for action in self.emulator.system.input_actions}
        self._joystick_manager = JoystickManager()
        self._frame_budget = 0.0
        self._last_run_tick = perf_counter()
        self._fps_window_start = self._last_run_tick
        self._fps_frame_counter = 0
        self._display_fps = 0.0
        self._speed_ratio = 0.0
        self._zoom_factor = 4
        self._palette_key = "gray"
        self._key_bindings = dict(self.emulator.system.default_key_bindings)
        self._key_binding_codes: dict[int, str] = {}
        self._run_timer = QTimer(self)
        self._pause_refresh_pending = False
        self._refresh_in_progress = False
        self._rewind_dir = Path(tempfile.mkdtemp(prefix="pyemu-rewind-"))
        self._rewind_history: deque[Path] = deque()
        self._rewind_generation = 0
        self._rewind_frame_counter = 0
        self._rewind_interval_frames = 30
        self._rewind_limit = 20
        self._session_path = Path.cwd() / ".pyemu-session.json"
        self._recent_media_menu: QMenu | None = None
        self._recent_media_actions: list[QAction] = []
        self._trace_dir: Path | None = None
        self._trace_log_path: Path | None = None
        self._trace_recording = False
        self._trace_frame_counter = 0
        self._trace_snapshot_interval_frames = 15
        self._controller_script: ControllerScriptHost | None = None
        self._script_input = ScriptInput()
        self._run_timer.setInterval(5)
        self._run_timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._run_timer.timeout.connect(self._on_run_timer)
        self.display_widget = FrameBufferWidget(self.emulator.system)
        self.display_widget.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.audio_player = AudioPlayer(self)
        self._start_trace_shortcut = QShortcut(QKeySequence("Q"), self)
        self._start_trace_shortcut.activated.connect(self.start_trace_recording)
        self._stop_trace_shortcut = QShortcut(QKeySequence("E"), self)
        self._stop_trace_shortcut.activated.connect(self.stop_trace_recording)

        self.registers_table = self._create_table(len(self.emulator.system.debug_registers), 2, ["Register", "Value"])
        self.flags_table = self._create_table(len(self.emulator.system.debug_flags), 2, ["Flag", "State"])
        self.flags_table.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.disassembly_table = self._create_table(36, 3, ["Address", "Bytes", "Instruction"])
        self.memory_views: dict[str, QTextEdit] = {}
        for region in self.emulator.system.debug_memory_regions:
            view = QTextEdit()
            view.setReadOnly(True)
            view.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
            self.memory_views[region.key] = view
        self.watch_table = self._create_table(len(self.emulator.system.debug_watch_entries), 2, ["Name", "Value"])
        self.breakpoints_table = self._create_table(4, 3, ["Enabled", "Address", "Condition"])
        self.call_stack_table = self._create_table(6, 2, ["Frame", "Address"])
        self.freeze_table = self._create_table(0, 3, ["Address", "Value", "Mode"])
        self.freeze_table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.freeze_table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self._freeze_entries: dict[int, int] = {}
        self.memory_region_combo = QComboBox()
        self.memory_region_combo.addItems([region.label for region in self.emulator.system.debug_memory_regions if region.writable])
        self.memory_region_combo.currentTextChanged.connect(self._on_memory_region_changed)
        self.memory_address_input = QLineEdit()
        self.memory_address_input.setPlaceholderText("0xC000 or +0010")
        self.memory_value_input = QLineEdit()
        self.memory_value_input.setPlaceholderText("0x00")
        self.memory_tool_status = QLabel("Write and freeze values from the current memory map.")
        self.registers_table.setColumnWidth(0, 90)
        self.flags_table.setColumnWidth(0, 90)

        self._seed_placeholder_tables()
        self._sync_freeze_table()
        self._on_memory_region_changed(self.memory_region_combo.currentText())
        self._fit_table_height(self.flags_table)
        self.registers_table.setMinimumHeight(self._table_content_height(self.registers_table))

        self._build_menu_bar()
        self.debugger_window = self._build_debugger_window()

        self.setCentralWidget(self.display_widget)
        self._zoom_factor = self._load_session_zoom()
        self._palette_key = self._load_session_palette()
        self._key_bindings = self._load_session_key_bindings()
        self._rebuild_key_binding_codes()
        self.display_widget.set_palette(self._palette_key)
        self._apply_zoom(self._zoom_factor, persist=False)
        self._autoload_default_media()
        self.refresh()

    def _create_table(self, rows: int, columns: int, headers: list[str]) -> QTableWidget:
        table = QTableWidget(rows, columns)
        table.setHorizontalHeaderLabels(headers)
        table.verticalHeader().setVisible(False)
        table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        table.setAlternatingRowColors(True)
        table.horizontalHeader().setStretchLastSection(True)
        table.horizontalHeader().setSectionResizeMode(0, table.horizontalHeader().sectionResizeMode(0))
        return table

    def _table_content_height(self, table: QTableWidget) -> int:
        table.resizeRowsToContents()
        height = table.horizontalHeader().height() + (table.frameWidth() * 2)
        for row in range(table.rowCount()):
            height += table.rowHeight(row)
        return height + 2

    def _fit_table_height(self, table: QTableWidget) -> None:
        table.setFixedHeight(self._table_content_height(table))

    def _build_menu_bar(self) -> None:
        self._file_menu = self.menuBar().addMenu("&File")
        file_menu = self._file_menu
        self._add_menu_action(file_menu, f"Load {self.emulator.system.media_label}", self.load_media, "Ctrl+O")
        self._recent_media_menu = file_menu.addMenu("Load Recent")
        self._rebuild_recent_media_menu()
        file_menu.addSeparator()
        self._add_menu_action(file_menu, "Save State", self.save_state, "Ctrl+S")
        self._add_menu_action(file_menu, "Load State", self.load_state, "Ctrl+L")
        self._add_menu_action(file_menu, "Rewind", self.rewind_state, "Ctrl+R")
        file_menu.addSeparator()
        self._add_menu_action(file_menu, "Reset", self._reset_emulator, "Ctrl+Shift+R")
        file_menu.addSeparator()
        self._add_menu_action(file_menu, "Exit", self.close, "Alt+F4")

        emu_menu = self.menuBar().addMenu("&Emulation")
        self._add_menu_action(emu_menu, "Run", self.start_running, "F5")
        self._add_menu_action(emu_menu, "Pause", self.pause_running, "F6")
        self._add_menu_action(emu_menu, "Step Instruction", self._step_instruction, "F10")
        self._add_menu_action(emu_menu, "Step Frame", self._step_frame, "F11")
        emu_menu.addSeparator()
        self._add_menu_action(emu_menu, "Configure Controls", self.configure_controls)

        view_menu = self.menuBar().addMenu("&View")
        zoom_group = QActionGroup(self)
        zoom_group.setExclusive(True)
        for scale in (1, 2, 4):
            action = QAction(f"{scale}x", self)
            action.setCheckable(True)
            action.setChecked(scale == self._zoom_factor)
            action.triggered.connect(lambda checked, scale=scale: self._apply_zoom(scale) if checked else None)
            zoom_group.addAction(action)
            view_menu.addAction(action)

        palette_menu = view_menu.addMenu("Palette")
        palette_group = QActionGroup(self)
        palette_group.setExclusive(True)
        for palette_key, (label, _colors) in PALETTE_PRESETS.items():
            action = QAction(label, self)
            action.setCheckable(True)
            action.setChecked(palette_key == self._palette_key)
            action.triggered.connect(lambda checked, palette_key=palette_key: self._apply_palette(palette_key) if checked else None)
            palette_group.addAction(action)
            palette_menu.addAction(action)

        debug_menu = self.menuBar().addMenu("&Debug")
        self._add_menu_action(debug_menu, "Open Debugger", self._show_debugger_window, "Ctrl+D")
        debug_menu.addSeparator()
        self._add_menu_action(debug_menu, "Load Controller Script", self.load_controller_script)
        self._add_menu_action(debug_menu, "Unload Controller Script", self.unload_controller_script)
        debug_menu.addSeparator()
        self._add_menu_action(debug_menu, "Dump Audio", self.dump_audio_capture)
        debug_menu.addSeparator()
        self._add_menu_action(debug_menu, "Start Trace", self.start_trace_recording, "Q")
        self._add_menu_action(debug_menu, "Stop Trace", self.stop_trace_recording, "E")

    def _add_menu_action(self, menu, label: str, callback, shortcut: str | None = None) -> QAction:
        action = QAction(label, self)
        if shortcut:
            action.setShortcut(QKeySequence(shortcut))
        action.triggered.connect(callback)
        menu.addAction(action)
        return action

    def _rebuild_recent_media_menu(self) -> None:
        if self._recent_media_menu is None:
            return
        self._recent_media_menu.clear()
        self._recent_media_actions.clear()

        recent_items = self._load_recent_media_paths()
        self._recent_media_menu.setEnabled(bool(recent_items))
        if not recent_items:
            empty_action = QAction("(empty)", self)
            empty_action.setEnabled(False)
            self._recent_media_menu.addAction(empty_action)
            self._recent_media_actions.append(empty_action)
            return

        for recent_path in recent_items:
            label = Path(recent_path).name
            action = QAction(label, self)
            action.setToolTip(recent_path)
            action.triggered.connect(lambda checked=False, path=recent_path: self._load_media_path(path))
            self._recent_media_menu.addAction(action)
            self._recent_media_actions.append(action)

    def _load_recent_media_paths(self) -> list[str]:
        entries = self._load_session().get("recent_media", [])
        if not isinstance(entries, list):
            return []
        results: list[str] = []
        for entry in entries:
            if not isinstance(entry, str):
                continue
            candidate = Path(entry).expanduser()
            if candidate.exists():
                results.append(str(candidate))
        return results[:8]


    def _apply_zoom(self, scale: int, persist: bool = True) -> None:
        self._zoom_factor = scale
        if persist:
            self._save_session(zoom_factor=scale)
        width = self.emulator.system.screen_width * scale
        height = self.emulator.system.screen_height * scale
        self.display_widget.set_scale(scale)
        self.display_widget.setFixedSize(width, height)
        self.adjustSize()
        self.setFixedSize(self.sizeHint())

    def _apply_palette(self, palette_key: str, persist: bool = True) -> None:
        if palette_key not in PALETTE_PRESETS:
            palette_key = "gray"
        self._palette_key = palette_key
        self.display_widget.set_palette(palette_key)
        if persist:
            self._save_session(palette_key=palette_key)

    def _build_debugger_window(self) -> QMainWindow:
        window = QMainWindow(self)
        window.setWindowTitle(f"pyemu | {self.emulator.system.display_name} debugger")
        window.resize(1360, 860)
        window.setCentralWidget(self._build_debugger_layout())
        return window

    def _show_debugger_window(self) -> None:
        self.debugger_window.show()
        self.debugger_window.raise_()
        self.debugger_window.activateWindow()
        self.refresh()

    def _seed_placeholder_tables(self) -> None:
        for row, register in enumerate(self.emulator.system.debug_registers):
            self.registers_table.setItem(row, 0, QTableWidgetItem(register.label))
        for row, flag in enumerate(self.emulator.system.debug_flags):
            self.flags_table.setItem(row, 0, QTableWidgetItem(flag.label))
        for row, watch in enumerate(self.emulator.system.debug_watch_entries):
            self.watch_table.setItem(row, 0, QTableWidgetItem(watch.label))
        self.breakpoints_table.setItem(0, 1, QTableWidgetItem("0x0100"))
        self.breakpoints_table.setItem(0, 2, QTableWidgetItem("ROM entry"))
        self.call_stack_table.setItem(0, 0, QTableWidgetItem("current"))

    def _build_controls(self) -> QGroupBox:
        group = QGroupBox("Execution")
        row = QHBoxLayout(group)

        buttons = [
            (f"Load {self.emulator.system.media_label}", self.load_media),
            ("Save State", self.save_state),
            ("Load State", self.load_state),
            ("Rewind", self.rewind_state),
            ("Reset", self._reset_emulator),
            ("Run", self.start_running),
            ("Pause", self.pause_running),
            ("Step", self._step_instruction),
            ("Frame", self._step_frame),
            ("Open Debugger", self._show_debugger_window),
        ]
        for label, action in buttons:
            button = QPushButton(label)
            button.clicked.connect(self._make_action(action))
            row.addWidget(button)

        row.addStretch(1)
        self.performance_label.setMinimumWidth(210)
        row.addWidget(self.performance_label)
        return group

    def _build_debugger_layout(self) -> QWidget:
        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)

        vertical_split = QSplitter(Qt.Orientation.Vertical)
        top_split = QSplitter(Qt.Orientation.Horizontal)
        bottom_split = QSplitter(Qt.Orientation.Horizontal)

        top_split.addWidget(self._wrap_group("Disassembly", self.disassembly_table))
        top_split.addWidget(self._build_right_pane())
        top_split.setSizes([860, 500])

        inspector_tabs = QTabWidget()
        inspector_tabs.addTab(self._wrap_group("Watch", self.watch_table), "Watch")
        inspector_tabs.addTab(self._wrap_group("Breakpoints", self.breakpoints_table), "Breakpoints")
        inspector_tabs.addTab(self._wrap_group("Call Stack", self.call_stack_table), "Call Stack")
        inspector_tabs.addTab(self._wrap_group("Freezes", self.freeze_table), "Cheats")

        memory_tabs = QTabWidget()
        for region in self.emulator.system.debug_memory_regions:
            memory_tabs.addTab(self._wrap_group(region.label, self.memory_views[region.key]), region.label)

        memory_panel = QWidget()
        memory_layout = QVBoxLayout(memory_panel)
        memory_layout.setContentsMargins(0, 0, 0, 0)
        memory_layout.setSpacing(8)
        memory_layout.addWidget(self._wrap_group("Memory", memory_tabs), 1)
        memory_layout.addWidget(self._build_memory_tools_group(), 0)

        bottom_split.addWidget(inspector_tabs)
        bottom_split.addWidget(memory_panel)
        bottom_split.setSizes([480, 880])

        vertical_split.addWidget(top_split)
        vertical_split.addWidget(bottom_split)
        vertical_split.setSizes([520, 320])

        layout.addWidget(vertical_split)
        return container

    def _build_right_pane(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        top_split = QSplitter(Qt.Orientation.Horizontal)

        details_tabs = QTabWidget()
        details_tabs.addTab(self._wrap_group("Session", self.status_label), "Session")
        details_tabs.addTab(self._wrap_group("Hardware", self.hardware_label), "Hardware")
        details_tabs.addTab(self._wrap_group("Cartridge", self.cartridge_label), "Cartridge")
        details_tabs.addTab(self._wrap_group("APU", self.apu_label), "APU")
        details_tabs.addTab(self._build_input_group(), "Input")

        state_panel = QWidget()
        state_layout = QVBoxLayout(state_panel)
        state_layout.setContentsMargins(0, 0, 0, 0)
        state_layout.setSpacing(8)
        state_layout.addWidget(self._wrap_group("Registers", self.registers_table), 1)
        flags_group = self._wrap_group("Flags", self.flags_table)
        flags_group.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        state_layout.addWidget(flags_group, 0)

        top_split.addWidget(details_tabs)
        top_split.addWidget(state_panel)
        top_split.setSizes([330, 240])

        layout.addWidget(top_split, 1)
        return panel

    def _wrap_group(self, title: str, widget: QWidget) -> QGroupBox:
        group = QGroupBox(title)
        layout = QVBoxLayout(group)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(widget)
        return group

    def _build_memory_tools_group(self) -> QGroupBox:
        group = QGroupBox("Memory Tools")
        layout = QVBoxLayout(group)
        controls = QGridLayout()
        controls.addWidget(QLabel("Region"), 0, 0)
        controls.addWidget(self.memory_region_combo, 0, 1)
        controls.addWidget(QLabel("Address"), 1, 0)
        controls.addWidget(self.memory_address_input, 1, 1)
        controls.addWidget(QLabel("Value"), 2, 0)
        controls.addWidget(self.memory_value_input, 2, 1)
        layout.addLayout(controls)

        row = QHBoxLayout()
        write_button = QPushButton("Write")
        write_button.clicked.connect(self.write_memory_value)
        freeze_button = QPushButton("Freeze")
        freeze_button.clicked.connect(self.freeze_memory_value)
        remove_button = QPushButton("Remove Selected")
        remove_button.clicked.connect(self.remove_selected_freeze)
        row.addWidget(write_button)
        row.addWidget(freeze_button)
        row.addWidget(remove_button)
        row.addStretch(1)
        layout.addLayout(row)
        layout.addWidget(self.memory_tool_status)
        return group

    def _region_base_address(self, region_name: str) -> int:
        region = self.emulator.system.debug_memory_region_map.get(region_name)
        return region.start if region is not None else 0x0000

    def _on_memory_region_changed(self, region_name: str) -> None:
        entry = self.memory_address_input.text().strip()
        current_value = None
        if entry.lower().startswith("0x"):
            with_value = entry[2:]
        else:
            with_value = entry
        try:
            current_value = int(with_value, 16) if with_value else None
        except ValueError:
            current_value = None
        if not entry or current_value == self._region_base_address(region_name):
            self.memory_address_input.setText(f"0x{self._region_base_address(region_name):04X}")

    def _parse_memory_address(self) -> int:
        raw = self.memory_address_input.text().strip()
        base = self._region_base_address(self.memory_region_combo.currentText())
        if not raw:
            return base
        if raw.startswith("+"):
            return (base + int(raw[1:], 16)) & 0xFFFF
        if raw.lower().startswith("0x"):
            return int(raw, 16) & 0xFFFF
        return int(raw, 16) & 0xFFFF

    def _parse_memory_value(self) -> int:
        raw = self.memory_value_input.text().strip()
        if not raw:
            raise ValueError("Enter a byte value before writing.")
        return int(raw, 16) & 0xFF

    def _sync_freeze_table(self) -> None:
        entries = sorted(self._freeze_entries.items())
        self.freeze_table.setRowCount(len(entries))
        for row, (address, value) in enumerate(entries):
            self.freeze_table.setItem(row, 0, QTableWidgetItem(f"0x{address:04X}"))
            self.freeze_table.setItem(row, 1, QTableWidgetItem(f"0x{value:02X}"))
            self.freeze_table.setItem(row, 2, QTableWidgetItem("freeze"))

    def _apply_freezes(self) -> None:
        for address, value in self._freeze_entries.items():
            self.emulator.poke_memory(address, value)

    def write_memory_value(self) -> None:
        try:
            address = self._parse_memory_address()
            value = self._parse_memory_value()
        except ValueError as exc:
            QMessageBox.warning(self, "Memory Tools", str(exc))
            return
        self.emulator.poke_memory(address, value)
        self.memory_tool_status.setText(f"Wrote 0x{value:02X} to 0x{address:04X}.")
        self.refresh()

    def freeze_memory_value(self) -> None:
        try:
            address = self._parse_memory_address()
            value = self._parse_memory_value()
        except ValueError as exc:
            QMessageBox.warning(self, "Memory Tools", str(exc))
            return
        self._freeze_entries[address] = value
        self.emulator.poke_memory(address, value)
        self._sync_freeze_table()
        self.memory_tool_status.setText(f"Frozen 0x{address:04X} at 0x{value:02X}.")
        self.refresh()

    def remove_selected_freeze(self) -> None:
        row = self.freeze_table.currentRow()
        if row < 0:
            return
        item = self.freeze_table.item(row, 0)
        if item is None:
            return
        try:
            address = int(item.text().replace("0x", ""), 16)
        except ValueError:
            return
        self._freeze_entries.pop(address, None)
        self._sync_freeze_table()
        self.memory_tool_status.setText(f"Removed freeze for 0x{address:04X}.")

    def _build_input_group(self) -> QGroupBox:
        group = QGroupBox("Input")
        group.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        group.setMinimumWidth(320)
        group.setMaximumWidth(420)
        group.setMaximumHeight(112)
        layout = QGridLayout(group)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setHorizontalSpacing(10)
        layout.setVerticalSpacing(6)

        for action in self.emulator.system.input_actions:
            button = QPushButton(action.label)
            button.setCheckable(True)
            button.setMinimumHeight(24)
            button.pressed.connect(lambda action_key=action.key: self._set_virtual_input(action_key, True))
            button.released.connect(lambda action_key=action.key: self._set_virtual_input(action_key, False))
            layout.addWidget(button, action.ui_row, action.ui_col)

        return group

    def _set_virtual_input(self, action_key: str, pressed: bool) -> None:
        if action_key in self._manual_action_states:
            self._manual_action_states[action_key] = pressed

        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        if self.emulator.run_state != RunState.RUNNING:
            self.refresh()

    def _refresh_joystick_input(self) -> None:
        self._joystick_action_states = self._joystick_manager.poll(
            self._manual_action_states.keys(),
            self.emulator.system.default_joystick_bindings,
        )

    def _drain_display_actions(self) -> None:
        return

    def _current_input_state(self) -> tuple[int, int]:
        self._refresh_joystick_input()
        action_map = self.emulator.system.input_action_map
        active_actions = {
            key for key, value in self._manual_action_states.items() if value
        }
        active_actions.update({key for key, value in self._joystick_action_states.items() if value})
        active_actions.update(name.lower() for name in getattr(self._script_input, "actions", ()) if isinstance(name, str))

        buttons = self._script_input.buttons & 0x0F
        directions = self._script_input.directions & 0x0F
        for action_key in active_actions:
            action = action_map.get(action_key)
            if action is None:
                continue
            if action.group == "buttons":
                buttons &= (~action.bitmask & 0x0F)
            elif action.group == "directions":
                directions &= (~action.bitmask & 0x0F)
        return buttons & 0x0F, directions & 0x0F

    def _clear_rewind_history(self) -> None:
        while self._rewind_history:
            snapshot = self._rewind_history.popleft()
            snapshot.unlink(missing_ok=True)
        self._rewind_generation += 1
        self._rewind_frame_counter = 0

    def _record_rewind_snapshot(self, *, force: bool = False) -> None:
        if not self.emulator.media.loaded:
            return
        if self.emulator.faulted and not force:
            return
        if not force:
            self._rewind_frame_counter += 1
            if self._rewind_frame_counter < self._rewind_interval_frames:
                return
        self._rewind_frame_counter = 0
        snapshot_path = self._rewind_dir / f"rewind-{self._rewind_generation:04d}-{len(self._rewind_history):02d}-{self.emulator.cycle_count}.pystate"
        if not self.emulator.save_state(str(snapshot_path)):
            return
        self._rewind_history.append(snapshot_path)
        while len(self._rewind_history) > self._rewind_limit:
            expired = self._rewind_history.popleft()
            expired.unlink(missing_ok=True)

    def _append_trace_entry(self, note: str, buttons: int, directions: int, *, force_snapshot: bool = False) -> None:
        if not self._trace_recording or self._trace_dir is None or self._trace_log_path is None or not self.emulator.media.loaded:
            return

        state = self.emulator.cpu_state
        memory = self.emulator.memory_snapshot()
        snapshot_name = None
        if force_snapshot or (self._trace_frame_counter % self._trace_snapshot_interval_frames == 0):
            snapshot_name = f"state-{self._trace_frame_counter:06d}-{self.emulator.cycle_count}.pystate"
            snapshot_path = self._trace_dir / snapshot_name
            if not self.emulator.save_state(str(snapshot_path)):
                snapshot_name = None

        def mem(address: int) -> int:
            return int(memory[address]) if len(memory) > address else 0

        cartridge = self.emulator.cartridge_debug
        entry = {
            "frame": self._trace_frame_counter,
            "cycle_count": int(self.emulator.cycle_count),
            "pc": int(state.pc),
            "sp": int(state.sp),
            "halted": bool(state.halted),
            "faulted": bool(self.emulator.faulted),
            "buttons": int(buttons),
            "directions": int(directions),
            "note": note,
            "io": {
                "ff00": mem(0xFF00),
                "ff0f": mem(0xFF0F),
                "ffff": mem(0xFFFF),
                "ff41": mem(0xFF41),
                "ff42": mem(0xFF42),
                "ff43": mem(0xFF43),
                "ff44": mem(0xFF44),
                "ff45": mem(0xFF45),
                "ff47": mem(0xFF47),
                "ff82": mem(0xFF82),
                "ff97": mem(0xFF97),
                "ff9b": mem(0xFF9B),
            },
            "cartridge": {
                "type": int(cartridge.cartridge_type),
                "rom_bank": int(cartridge.rom_bank),
                "ram_bank": int(cartridge.ram_bank),
                "banking_mode": int(cartridge.banking_mode),
                "ram_enabled": bool(cartridge.ram_enabled),
            },
            "watch": {
                "a233": mem(0xA233),
                "a298": mem(0xA298),
                "d85f": mem(0xD85F),
                "d860": mem(0xD860),
                "df83": mem(0xDF83),
                "df84": mem(0xDF84),
                "df85": mem(0xDF85),
                "df86": mem(0xDF86),
                "df87": mem(0xDF87),
                "df89": mem(0xDF89),
            },
            "sprite0": {
                "y": mem(0xFE00),
                "x": mem(0xFE01),
                "tile": mem(0xFE02),
                "attr": mem(0xFE03),
            },
        }
        if snapshot_name is not None:
            entry["snapshot"] = snapshot_name

        try:
            with self._trace_log_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry) + "\n")
        except OSError:
            pass

    def start_trace_recording(self) -> None:
        if not self.emulator.media.loaded:
            QMessageBox.information(self, "Trace", "Load a ROM before starting a trace.")
            return
        if self._trace_recording:
            return

        trace_root = Path.cwd() / "traces"
        trace_root.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        safe_title = (Path(self.emulator.media.path).stem or self.emulator.media.title or "trace").replace(" ", "_")
        self._trace_dir = trace_root / f"{stamp}-{safe_title}"
        self._trace_dir.mkdir(parents=True, exist_ok=True)
        self._trace_log_path = self._trace_dir / "trace.jsonl"
        self._trace_recording = True
        self._trace_frame_counter = 0

        metadata = {
            "system": self.emulator.system_name,
            "rom_path": self.emulator.media.path,
            "title": self.emulator.media.title,
            "frame_rate": self.emulator.system.frame_rate,
        }
        try:
            (self._trace_dir / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
        except OSError:
            pass

        buttons, directions = self._current_input_state()
        self._append_trace_entry("trace_started", buttons, directions, force_snapshot=True)
        self.refresh()

    def stop_trace_recording(self) -> None:
        if not self._trace_recording:
            return
        buttons, directions = self._current_input_state()
        self._append_trace_entry("trace_stopped", buttons, directions, force_snapshot=True)
        trace_dir = self._trace_dir
        self._trace_recording = False
        self._trace_dir = None
        self._trace_log_path = None
        self.refresh()
        if trace_dir is not None:
            QMessageBox.information(self, "Trace saved", f"Trace saved to:\n{trace_dir}")

    def _show_display_window(self) -> None:
        self.show()
        self.raise_()
        self.activateWindow()
        self.display_widget.setFocus()

    def _reset_display_and_input_state(self) -> None:
        self._manual_action_states = {action.key: False for action in self.emulator.system.input_actions}
        self._joystick_action_states = {action.key: False for action in self.emulator.system.input_actions}
        self._script_input = ScriptInput()
        self.display_widget.update_frame(self.emulator.frame_buffer)
        self.audio_player.reset()
        self.emulator.set_gameboy_joypad_state(0x0F, 0x0F)

    def _make_action(self, action):
        def callback() -> None:
            action()
            if self.emulator.run_state == RunState.RUNNING:
                self._present_display()
            else:
                self.refresh()

        return callback

    def _run_instructions(self, steps: int) -> None:
        self.pause_running()
        for _ in range(steps):
            if self.emulator.faulted:
                break
            self._refresh_script_input()
            buttons, directions = self._current_input_state()
            self.emulator.set_gameboy_joypad_state(buttons, directions)
            self.emulator.step_instruction()

    def _reset_emulator(self) -> None:
        self.pause_running()
        self._reset_display_and_input_state()
        self.emulator.reset()
        self._clear_rewind_history()
        self._record_rewind_snapshot(force=True)
        if self._controller_script is not None:
            self._controller_script.reset(self.emulator)
        buttons, directions = self._current_input_state()
        self._append_trace_entry("reset", buttons, directions, force_snapshot=True)
        self.refresh()

    def _step_instruction(self) -> None:
        self.pause_running()
        self._refresh_script_input()
        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        self._apply_freezes()
        self.emulator.step_instruction()
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._trace_frame_counter += 1
        self._append_trace_entry("step_instruction", buttons, directions, force_snapshot=False)

    def _step_frame(self) -> None:
        self.pause_running()
        self._refresh_script_input()
        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        self._apply_freezes()
        self.emulator.step_frame()
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._trace_frame_counter += 1
        self._append_trace_entry("step_frame", buttons, directions, force_snapshot=False)

    def start_running(self) -> None:
        if not self.emulator.media.loaded:
            return
        if self.emulator.run_state == RunState.RUNNING:
            return
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._append_trace_entry("run_started", buttons, directions, force_snapshot=True)
        self._last_run_tick = perf_counter()
        self._fps_window_start = self._last_run_tick
        self._fps_frame_counter = 0
        self._frame_budget = 0.0
        self.emulator.set_bus_tracking(False)
        self.emulator.run()
        self.refresh()
        self._run_timer.start()
        self._show_display_window()
        self._present_display()

    def pause_running(self) -> None:
        was_running = self.emulator.run_state == RunState.RUNNING
        self._run_timer.stop()
        self._frame_budget = 0.0
        self.emulator.pause()
        self.audio_player.reset()
        self.emulator.set_bus_tracking(True)
        self._update_performance_label()
        if was_running:
            self._pause_refresh_pending = True
        if self._pause_refresh_pending and not self._refresh_in_progress:
            self._pause_refresh_pending = False
            self.refresh()

    def _on_run_timer(self) -> None:
        if self.emulator.run_state != RunState.RUNNING:
            self._run_timer.stop()
            return

        now = perf_counter()
        elapsed = now - self._last_run_tick
        self._last_run_tick = now
        frame_time = 1.0 / self.emulator.system.frame_rate
        self._frame_budget += elapsed

        frames_to_run = min(6, int(self._frame_budget / frame_time))
        if frames_to_run <= 0:
            return

        executed_frames = 0
        for _ in range(frames_to_run):
            if self.emulator.faulted:
                break
            self._refresh_script_input()
            buttons, directions = self._current_input_state()
            self.emulator.set_gameboy_joypad_state(buttons, directions)
            self._apply_freezes()
            self.emulator.step_frame()
            executed_frames += 1
            self._trace_frame_counter += 1
            self._append_trace_entry("run_frame", buttons, directions, force_snapshot=self.emulator.faulted)
            self.emulator.run()
            self._frame_budget -= frame_time

        if executed_frames:
            self._record_frame_stats(executed_frames, now)
            self._record_rewind_snapshot()
            self._present_display()

        if self.emulator.faulted:
            self.pause_running()

    def _present_display(self) -> None:
        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        self.display_widget.update_frame(self.emulator.frame_buffer)
        self.audio_player.push(self.emulator.audio_buffer)
        self.display_widget.setFocus()

    def _record_frame_stats(self, frames: int, now: float) -> None:
        self._fps_frame_counter += frames
        elapsed = now - self._fps_window_start
        if elapsed >= 0.5:
            self._display_fps = self._fps_frame_counter / elapsed
            self._speed_ratio = self._display_fps / self.emulator.system.frame_rate if self.emulator.system.frame_rate else 0.0
            self._fps_window_start = now
            self._fps_frame_counter = 0
            self._update_performance_label()

    def _update_performance_label(self) -> None:
        self.performance_label.setText(
            f"FPS: {self._display_fps:5.1f} | Speed: {self._speed_ratio * 100:5.1f}%"
        )

    def _load_session(self) -> dict:
        if not self._session_path.exists():
            return {}
        try:
            data = json.loads(self._session_path.read_text())
        except (OSError, json.JSONDecodeError):
            return {}
        return data if isinstance(data, dict) else {}

    def _save_session(self, **updates) -> None:
        payload = self._load_session()
        payload.update(updates)
        try:
            self._session_path.write_text(json.dumps(payload, indent=2))
        except OSError:
            pass

    def _load_last_media_path(self) -> Path | None:
        path = self._load_session().get("last_media_path")
        if not path:
            return None
        candidate = Path(path).expanduser()
        return candidate if candidate.exists() else None

    def _load_session_zoom(self) -> int:
        zoom = self._load_session().get("zoom_factor", self._zoom_factor)
        return zoom if zoom in (1, 2, 4) else self._zoom_factor

    def _load_session_palette(self) -> str:
        palette_key = self._load_session().get("palette_key", self._palette_key)
        return palette_key if palette_key in PALETTE_PRESETS else self._palette_key

    def _load_session_key_bindings(self) -> dict[str, str]:
        saved = self._load_session().get("key_bindings", {})
        bindings = dict(self.emulator.system.default_key_bindings)
        if isinstance(saved, dict):
            for action in self.emulator.system.input_actions:
                value = saved.get(action.key)
                if isinstance(value, str):
                    bindings[action.key] = value
        return bindings

    def _rebuild_key_binding_codes(self) -> None:
        self._key_binding_codes = {}
        for action in self.emulator.system.input_actions:
            sequence = self._key_bindings.get(action.key, "")
            key_sequence = QKeySequence(sequence)
            if key_sequence.count() <= 0:
                continue
            combination = key_sequence[0]
            key_value = combination.key() if hasattr(combination, "key") else int(combination)
            self._key_binding_codes[int(key_value)] = action.key

    def configure_controls(self) -> None:
        dialog = ControlsDialog(self.emulator.system, self._key_bindings, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        self._key_bindings = dialog.key_bindings()
        self._rebuild_key_binding_codes()
        self._save_session(key_bindings=self._key_bindings)

    def _save_last_media_path(self, path: str) -> None:
        normalized = str(Path(path).expanduser())
        recent = [item for item in self._load_recent_media_paths() if item != normalized]
        recent.insert(0, normalized)
        self._save_session(last_media_path=normalized, recent_media=recent[:8])
        self._rebuild_recent_media_menu()

    def _write_wav(self, path: Path, audio_buffer: AudioBuffer) -> None:
        if audio_buffer.channels <= 0 or audio_buffer.sample_rate <= 0:
            return
        with wave.open(str(path), 'wb') as wav_file:
            wav_file.setnchannels(audio_buffer.channels)
            wav_file.setsampwidth(2)
            wav_file.setframerate(audio_buffer.sample_rate)
            wav_file.writeframes(audio_buffer.pcm16le)

    def dump_audio_capture(self) -> None:
        if not self.emulator.media.loaded:
            QMessageBox.information(self, 'Dump Audio', 'Load a ROM before dumping audio.')
            return
        self.pause_running()
        base_name = f"{Path(self.emulator.media.path).stem or 'pyemu'}-audio.wav"
        path, _ = QFileDialog.getSaveFileName(self, 'Save Audio Capture', str(Path.cwd() / base_name), 'Wave files (*.wav)')
        if not path:
            return
        target = Path(path)
        temp_state = Path(tempfile.gettempdir()) / f"pyemu-audio-dump-{id(self)}.pystate"
        if not self.emulator.save_state(str(temp_state)):
            QMessageBox.warning(self, 'Dump Audio', 'Could not snapshot the current emulator state.')
            return
        try:
            mixed = bytearray()
            channels = [bytearray() for _ in range(4)]
            for _ in range(180):
                self.emulator.step_frame()
                mixed.extend(self.emulator.audio_buffer.pcm16le)
                for channel_index in range(4):
                    channels[channel_index].extend(self.emulator.gameboy_audio_channel_buffer(channel_index + 1).pcm16le)
            mixed_buffer = AudioBuffer(sample_rate=self.emulator.audio_buffer.sample_rate, channels=self.emulator.audio_buffer.channels, pcm16le=bytes(mixed))
            self._write_wav(target, mixed_buffer)
            stem = target.with_suffix('')
            names = ['pulse1', 'pulse2', 'wave', 'noise']
            for idx, payload in enumerate(channels):
                channel_buffer = AudioBuffer(sample_rate=mixed_buffer.sample_rate, channels=mixed_buffer.channels, pcm16le=bytes(payload))
                self._write_wav(stem.parent / f"{stem.name}-{names[idx]}.wav", channel_buffer)
        finally:
            self.emulator.load_state(str(temp_state))
            temp_state.unlink(missing_ok=True)
            self.refresh()
        QMessageBox.information(self, 'Dump Audio', f'WAV files saved next to:\n{target}')

    def _describe_wait_state(self, memory, state, media) -> str | None:
        if self.emulator.faulted:
            return f"Faulted at PC=0x{state.pc:04X}: execution stopped on an unsupported path"
        if not state.halted:
            return None

        pending = memory[0xFF0F] & memory[0xFFFF] & 0x1F
        if pending:
            return f"HALT wake pending: IF={memory[0xFF0F]:02X} IE={memory[0xFFFF]:02X}"

        if media.title.upper().startswith("SUPER MARIOLAND") and state.pc == 0x0297:
            return "Mario transition loop: waiting for the next LCD/VBlank tick"

        if memory[0xFFFF] & 0x01:
            return f"HALT wait: next VBlank/LCD interrupt (LY={memory[0xFF44]:02X}, STAT={memory[0xFF41]:02X})"

        return "HALT wait: no enabled interrupt is pending yet"

    def _autoload_default_media(self) -> None:
        last_media = self._load_last_media_path()
        candidates = []
        if last_media is not None:
            candidates.append(last_media)
        candidates.extend([
            Path.cwd() / "tetris.gb",
            Path.cwd() / "TETRIS.GB",
            Path.cwd() / "tetris.gbc",
            Path.cwd() / "tetris.zip",
            Path.cwd() / "TETRIS.ZIP",
        ])
        for candidate in candidates:
            if candidate.exists():
                self._reset_display_and_input_state()
                if self.emulator.load_media(str(candidate)):
                    self._save_last_media_path(str(candidate))
                    self._clear_rewind_history()
                    self._record_rewind_snapshot(force=True)
                    self._trace_frame_counter = 0
                    if self._controller_script is not None:
                        self._controller_script.reset(self.emulator)
                    self.refresh()
                    break

    def load_media(self) -> None:
        self.pause_running()
        path, _ = QFileDialog.getOpenFileName(
            self,
            f"Select {self.emulator.system.media_label}",
            str(Path.cwd()),
            self.emulator.system.file_dialog_filter,
        )
        if not path:
            return
        self._load_media_path(path)

    def _load_media_path(self, path: str) -> None:
        self.pause_running()
        self._reset_display_and_input_state()
        try:
            loaded = self.emulator.load_media(path)
        except ValueError as exc:
            QMessageBox.warning(self, "Load failed", str(exc))
            return
        if not loaded:
            QMessageBox.warning(self, "Load failed", f"The selected {self.emulator.system.media_label.lower()} could not be loaded.")
            return
        self._save_last_media_path(path)
        self._clear_rewind_history()
        self._record_rewind_snapshot(force=True)
        self._trace_frame_counter = 0
        if self._controller_script is not None:
            self._controller_script.reset(self.emulator)
        buttons, directions = self._current_input_state()
        self._append_trace_entry("media_loaded", buttons, directions, force_snapshot=True)
        self.refresh()

    def save_state(self) -> None:
        if not self.emulator.media.loaded:
            QMessageBox.information(self, "Save state", "Load a ROM before saving state.")
            return
        default_name = f"{Path(self.emulator.media.path).stem or self.emulator.media.title or 'pyemu'}.pystate"
        path, _ = QFileDialog.getSaveFileName(
            self,
            "Save State",
            str(Path.cwd() / default_name),
            "pyemu state (*.pystate);;All files (*.*)",
        )
        if not path:
            return
        self.pause_running()
        if not self.emulator.save_state(path):
            QMessageBox.warning(self, "Save state failed", "The current emulator state could not be saved.")
            return
        self._record_rewind_snapshot(force=True)
        self.refresh()

    def rewind_state(self) -> None:
        if not self._rewind_history:
            QMessageBox.information(self, "Rewind", "No rewind history is available yet.")
            return
        self.pause_running()
        if len(self._rewind_history) > 1:
            latest = self._rewind_history.pop()
            latest.unlink(missing_ok=True)
        target = self._rewind_history[-1]
        if not self.emulator.load_state(str(target)):
            QMessageBox.warning(self, "Rewind failed", "The previous rewind state could not be restored.")
            return
        self.refresh()

    def load_state(self) -> None:
        if not self.emulator.media.loaded:
            QMessageBox.information(self, "Load state", "Load the matching ROM before loading a save state.")
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Load State",
            str(Path.cwd()),
            "pyemu state (*.pystate);;All files (*.*)",
        )
        if not path:
            return
        self.pause_running()
        if not self.emulator.load_state(path):
            QMessageBox.warning(self, "Load state failed", "The selected state could not be loaded for the current ROM.")
            return
        self._clear_rewind_history()
        self._record_rewind_snapshot(force=True)
        self._trace_frame_counter = 0
        if self._controller_script is not None:
            self._controller_script.reset(self.emulator)
        buttons, directions = self._current_input_state()
        self._append_trace_entry("state_loaded", buttons, directions, force_snapshot=True)
        self.refresh()

    def load_controller_script(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Load Controller Script",
            str(Path.cwd()),
            "Python script (*.py);;All files (*.*)",
        )
        if not path:
            return
        try:
            host = ControllerScriptHost(path)
            host.reset(self.emulator)
            self._controller_script = host
            self._script_input = ScriptInput()
        except Exception as exc:
            QMessageBox.warning(self, "Controller Script", f"Could not load script:\n{exc}")
            return
        self.refresh()

    def unload_controller_script(self) -> None:
        if self._controller_script is not None:
            try:
                self._controller_script.unload(self.emulator)
            except Exception:
                pass
        self._controller_script = None
        self._script_input = ScriptInput()
        self.refresh()

    def _refresh_script_input(self) -> None:
        if self._controller_script is None:
            self._script_input = ScriptInput()
            return
        try:
            self._script_input = self._controller_script.next_input(self.emulator)
        except Exception as exc:
            QMessageBox.warning(self, "Controller Script", f"Script execution failed:\n{exc}")
            self.unload_controller_script()
            self._script_input = ScriptInput()


    def refresh(self) -> None:
        if self._refresh_in_progress:
            return

        self._refresh_in_progress = True
        try:
            state = self.emulator.cpu_state
            media = self.emulator.media
            frame = self.emulator.frame_buffer
            memory = self.emulator.memory_snapshot()
            buttons, directions = self._current_input_state()
            access = self.emulator.last_bus_access
            cartridge = self.emulator.cartridge_debug

            self._update_performance_label()
            wait_note = self._describe_wait_state(memory, state, media)
            status_lines = [
                f"System: {self.emulator.system.display_name}",
                f"Native backend: {'yes' if self.emulator.native_available else 'no'}",
                f"Native DLL: {Path(self.emulator.native_library_path).name if self.emulator.native_library_path else 'fallback'}",
                f"Run state: {self.emulator.run_state.name}",
                f"Target FPS: {self.emulator.system.frame_rate:.3f}",
                f"CPU halted: {'yes' if state.halted else 'no'}",
                f"CPU faulted: {'yes' if self.emulator.faulted else 'no'}",
                f"Cycles: {self.emulator.cycle_count}",
                f"DIV/TIMA: {memory[0xFF04]:02X}/{memory[0xFF05]:02X}",
                f"IF/IE: {memory[0xFF0F]:02X}/{memory[0xFFFF]:02X}",
                f"JOY/BTN/DIR: {memory[0xFF00]:02X}/{buttons:02X}/{directions:02X}",
                f"LY: {memory[0xFF44]:02X}",
                f"Trace: {'recording' if self._trace_recording else 'off'} ({self._trace_frame_counter} frames)",
                f"Core key: {self.emulator.system_name}",
                f"Controller script: {self._controller_script.display_name if self._controller_script is not None else 'off'}",
            ]
            status_lines.extend(self.audio_player.status_lines())
            if wait_note:
                status_lines.append(f"Wait: {wait_note}")
            self.status_label.setText("\n".join(status_lines))
            self.hardware_label.setText(self._format_hardware_access(access, media))
            self.cartridge_label.setText(self._format_cartridge_debug(cartridge, memory))
            self.apu_label.setText(self._format_apu_debug(memory))
            self._update_shell_status(media, state)
            self._present_display()
            self._update_registers(state)
            self._update_watch_and_stack(memory, state, buttons, directions)
            self._update_disassembly(memory, state.pc)
            self._update_memory(memory, state.pc)
            self._sync_freeze_table()
        finally:
            self._refresh_in_progress = False

    def _update_shell_status(self, media, state) -> None:
        rom_name = Path(media.path).name if media.path else "(none)"
        title = media.title or self.emulator.system.display_name
        self.setWindowTitle(f"pyemu | {title} | {rom_name} | {self.emulator.run_state.name}")

    def _format_apu_debug(self, memory) -> str:
        info = self.emulator.gameboy_audio_debug
        lines = [
            f"NR50/51/52: {info.nr50:02X}/{info.nr51:02X}/{info.nr52:02X}",
            f"CH1: {'on' if info.ch1_enabled else 'off'} duty={info.ch1_duty} vol={info.ch1_volume} freq={info.ch1_frequency_raw}",
            f"CH2: {'on' if info.ch2_enabled else 'off'} duty={info.ch2_duty} vol={info.ch2_volume} freq={info.ch2_frequency_raw}",
            f"CH3: {'on' if info.ch3_enabled else 'off'} volcode={info.ch3_volume_code} freq={info.ch3_frequency_raw}",
            f"CH4: {'on' if info.ch4_enabled else 'off'} vol={info.ch4_volume} div={info.ch4_divisor_code} shift={info.ch4_clock_shift} width={info.ch4_width_mode}",
            f"LFSR: {info.ch4_lfsr:04X}",
            f"Wave RAM: {' '.join(f'{value:02X}' for value in memory[0xFF30:0xFF40])}",
        ]
        return "\n".join(lines)

    def _update_registers(self, state) -> None:
        for row, register in enumerate(self.emulator.system.debug_registers):
            if register.source == "u16_attr" and register.attr is not None:
                value = getattr(state, register.attr, 0)
                formatted = f"0x{int(value) & 0xFFFF:04X}"
            elif register.source == "u8_pair" and register.hi_attr is not None and register.lo_attr is not None:
                hi = getattr(state, register.hi_attr, 0)
                lo = getattr(state, register.lo_attr, 0)
                formatted = f"0x{int(hi) & 0xFF:02X}{int(lo) & 0xFF:02X}"
            else:
                formatted = "n/a"
            self.registers_table.setItem(row, 1, QTableWidgetItem(formatted))

        for row, flag in enumerate(self.emulator.system.debug_flags):
            enabled = bool(state.f & flag.mask)
            self.flags_table.setItem(row, 1, QTableWidgetItem("set" if enabled else "clear"))
        self._fit_table_height(self.flags_table)

    def _format_watch_value(self, watch, memory: list[int], buttons: int, directions: int) -> str:
        if watch.source == "memory8" and watch.address is not None:
            return f"0x{memory[watch.address]:02X}" if len(memory) > watch.address else "n/a"
        if watch.source == "memory8_pair" and watch.address is not None and watch.address2 is not None:
            if len(memory) > max(watch.address, watch.address2):
                return f"0x{memory[watch.address]:02X}/0x{memory[watch.address2]:02X}"
            return "n/a"
        if watch.source == "input_buttons":
            return f"0x{buttons:02X}"
        if watch.source == "input_directions":
            return f"0x{directions:02X}"
        return "n/a"

    def _update_watch_and_stack(self, memory: list[int], state, buttons: int, directions: int) -> None:
        for row, watch in enumerate(self.emulator.system.debug_watch_entries):
            self.watch_table.setItem(row, 1, QTableWidgetItem(self._format_watch_value(watch, memory, buttons, directions)))

        self.breakpoints_table.setItem(0, 0, QTableWidgetItem("yes"))
        self.call_stack_table.setItem(0, 1, QTableWidgetItem(f"0x{state.pc:04X}"))
        if len(memory) > state.sp + 1 and state.sp < 0xFFFE:
            ret = memory[state.sp] | (memory[state.sp + 1] << 8)
            self.call_stack_table.setItem(1, 0, QTableWidgetItem("stack[sp]"))
            self.call_stack_table.setItem(1, 1, QTableWidgetItem(f"0x{ret:04X}"))

    def _update_disassembly(self, memory: list[int], pc: int) -> None:
        start = max(0, pc - 24)
        address = start
        row = 0
        self.disassembly_table.clearContents()
        current_row = -1
        while row < self.disassembly_table.rowCount() and address < min(len(memory), 0x10000):
            size, text = self._decode_instruction(memory, address)
            bytes_text = " ".join(f"{byte:02X}" for byte in memory[address : address + size])
            addr_item = QTableWidgetItem(f"0x{address:04X}")
            bytes_item = QTableWidgetItem(bytes_text)
            text_item = QTableWidgetItem(text)
            if address == pc:
                current_row = row
                for item in (addr_item, bytes_item, text_item):
                    item.setBackground(QColor("#264f78"))
                    item.setForeground(QColor("#ffffff"))
            self.disassembly_table.setItem(row, 0, addr_item)
            self.disassembly_table.setItem(row, 1, bytes_item)
            self.disassembly_table.setItem(row, 2, text_item)
            address += max(size, 1)
            row += 1

        if current_row >= 0:
            self.disassembly_table.setCurrentCell(current_row, 0)
            self.disassembly_table.scrollToItem(
                self.disassembly_table.item(current_row, 0),
                QAbstractItemView.ScrollHint.PositionAtCenter,
            )

    def _update_memory(self, memory: list[int], pc: int) -> None:
        for region in self.emulator.system.debug_memory_regions:
            view = self.memory_views.get(region.key)
            if view is None:
                continue
            view.setPlainText(self._format_memory_region(memory, region.start, region.end, pc))

    def _format_memory_region(self, memory: list[int], start: int, end: int, pc: int | None = None) -> str:
        lines = []
        for offset in range(start, min(end, len(memory)), 16):
            chunk = memory[offset : offset + 16]
            hex_bytes = " ".join(f"{value:02X}" for value in chunk)
            marker = "<" if pc is not None and offset <= pc < offset + 16 else " "
            lines.append(f"{marker} {offset:04X}: {hex_bytes}")
        return "\n".join(lines)

    def _format_hardware_access(self, access, media) -> str:
        if not access.valid:
            return "No tracked hardware access yet."

        names = {
            0xFF00: "Joypad",
            0xFF40: "LCDC",
            0xFF41: "STAT",
            0xFF42: "SCY",
            0xFF43: "SCX",
            0xFF44: "LY",
            0xFF46: "DMA",
            0xFF47: "BGP",
            0xFF48: "OBP0",
            0xFF49: "OBP1",
            0xFF4A: "WY",
            0xFF4B: "WX",
            0xFFFF: "IE",
        }
        if access.address <= 0x1FFF:
            meaning = "Cartridge RAM enable"
        elif access.address <= 0x3FFF:
            meaning = "Cartridge ROM bank select"
        elif access.address <= 0x5FFF:
            meaning = "Cartridge RAM bank / upper ROM bits"
        elif access.address <= 0x7FFF:
            meaning = "Cartridge banking mode select"
        elif 0x8000 <= access.address <= 0x97FF:
            meaning = "Tile data (VRAM)"
        elif 0x9800 <= access.address <= 0x9FFF:
            meaning = "Tile map (BG/Window)"
        elif 0xFE00 <= access.address <= 0xFE9F:
            meaning = "Sprite attribute table (OAM)"
        else:
            meaning = names.get(access.address, "Memory-mapped IO")

        action = "Wrote" if access.is_write else "Read"
        return "\n".join([
            f"{action} 0x{access.value:02X} {'to' if access.is_write else 'from'} 0x{access.address:04X}",
            meaning,
            f"ROM: {media.title or '(none)'}",
        ])

    def _format_cartridge_debug(self, cartridge, memory: list[int]) -> str:
        eram_preview = " ".join(f"{value:02X}" for value in memory[0xA000:0xA008]) if len(memory) >= 0xA008 else "n/a"
        if cartridge.last_mapper_valid:
            if cartridge.last_mapper_address <= 0x1FFF:
                mapper_meaning = "RAM enable"
            elif cartridge.last_mapper_address <= 0x3FFF:
                mapper_meaning = "ROM bank select"
            elif cartridge.last_mapper_address <= 0x5FFF:
                mapper_meaning = "RAM bank / upper bits"
            else:
                mapper_meaning = "Banking mode"
            mapper_line = f"Last mapper: {cartridge.last_mapper_address:04X}={cartridge.last_mapper_value:02X} ({mapper_meaning})"
        else:
            mapper_line = "Last mapper: (none)"
        return "\n".join([
            f"Type: 0x{cartridge.cartridge_type:02X}",
            f"ROM/RAM codes: {cartridge.rom_size_code:02X}/{cartridge.ram_size_code:02X}",
            f"ROM bank: {cartridge.rom_bank}/{cartridge.rom_bank_count}",
            f"RAM bank: {cartridge.ram_bank}/{cartridge.ram_bank_count}",
            f"RAM enabled: {'yes' if cartridge.ram_enabled else 'no'}",
            f"MBC1 mode: {cartridge.banking_mode}",
            f"Battery/save: {'yes' if cartridge.has_battery else 'no'}/{'yes' if cartridge.save_file_present else 'no'}",
            mapper_line,
            f"A000: {eram_preview}",
        ])

    def _decode_instruction(self, memory: list[int], pc: int) -> tuple[int, str]:
        opcode = memory[pc]
        nxt = memory[pc + 1] if pc + 1 < len(memory) else 0
        nxt2 = memory[pc + 2] if pc + 2 < len(memory) else 0
        imm16 = nxt | (nxt2 << 8)
        decoders = {
            0x00: (1, "NOP"),
            0x01: (3, f"LD BC, ${imm16:04X}"),
            0x03: (1, "INC BC"),
            0x04: (1, "INC B"),
            0x05: (1, "DEC B"),
            0x06: (2, f"LD B, ${nxt:02X}"),
            0x08: (3, f"LD (${imm16:04X}), SP"),
            0x0B: (1, "DEC BC"),
            0x0C: (1, "INC C"),
            0x0D: (1, "DEC C"),
            0x0E: (2, f"LD C, ${nxt:02X}"),
            0x11: (3, f"LD DE, ${imm16:04X}"),
            0x13: (1, "INC DE"),
            0x18: (2, f"JR {self._format_rel(pc, nxt)}"),
            0x20: (2, f"JR NZ, {self._format_rel(pc, nxt)}"),
            0x21: (3, f"LD HL, ${imm16:04X}"),
            0x22: (1, "LD (HL+), A"),
            0x23: (1, "INC HL"),
            0x27: (1, "DAA"),
            0x28: (2, f"JR Z, {self._format_rel(pc, nxt)}"),
            0x29: (1, "ADD HL, HL"),
            0x2A: (1, "LD A, (HL+)"),
            0x2B: (1, "DEC HL"),
            0x31: (3, f"LD SP, ${imm16:04X}"),
            0x32: (1, "LD (HL-), A"),
            0x33: (1, "INC SP"),
            0x36: (2, f"LD (HL), ${nxt:02X}"),
            0x3E: (2, f"LD A, ${nxt:02X}"),
            0x46: (1, "LD B, (HL)"),
            0x47: (1, "LD B, A"),
            0x4F: (1, "LD C, A"),
            0x57: (1, "LD D, A"),
            0x5F: (1, "LD E, A"),
            0x67: (1, "LD H, A"),
            0x6F: (1, "LD L, A"),
            0x77: (1, "LD (HL), A"),
            0x78: (1, "LD A, B"),
            0x79: (1, "LD A, C"),
            0x7A: (1, "LD A, D"),
            0x7B: (1, "LD A, E"),
            0x7C: (1, "LD A, H"),
            0x7D: (1, "LD A, L"),
            0x7E: (1, "LD A, (HL)"),
            0x86: (1, "ADD A, (HL)"),
            0x8E: (1, "ADC A, (HL)"),
            0xA0: (1, "AND B"),
            0xA7: (1, "AND A"),
            0xAF: (1, "XOR A"),
            0xC1: (1, "POP BC"),
            0xC3: (3, f"JP ${imm16:04X}"),
            0xC5: (1, "PUSH BC"),
            0xC9: (1, "RET"),
            0xCD: (3, f"CALL ${imm16:04X}"),
            0xD0: (1, "RET NC"),
            0xD1: (1, "POP DE"),
            0xD5: (1, "PUSH DE"),
            0xE0: (2, f"LDH (${nxt:02X}), A"),
            0xE1: (1, "POP HL"),
            0xE5: (1, "PUSH HL"),
            0xE6: (2, f"AND ${nxt:02X}"),
            0xF0: (2, f"LDH A, (${nxt:02X})"),
            0xF1: (1, "POP AF"),
            0xF3: (1, "DI"),
            0xF5: (1, "PUSH AF"),
            0xFB: (1, "EI"),
            0xFE: (2, f"CP ${nxt:02X}"),
        }
        return decoders.get(opcode, (1, f"DB ${opcode:02X}"))

    def _format_rel(self, pc: int, value: int) -> str:
        offset = value if value < 0x80 else value - 0x100
        target = (pc + 2 + offset) & 0xFFFF
        return f"${target:04X}"

    def _apply_keyboard_input(self, key: int, pressed: bool) -> bool:
        action_key = self._key_binding_codes.get(int(key))
        if action_key is None and key == Qt.Key.Key_Enter:
            action_key = self._key_binding_codes.get(int(Qt.Key.Key_Return))
        if action_key is None:
            return False
        if action_key in self._manual_action_states:
            self._manual_action_states[action_key] = pressed
        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        return True

    def keyPressEvent(self, event: QKeyEvent) -> None:  # type: ignore[override]
        if not event.isAutoRepeat() and self._apply_keyboard_input(event.key(), True):
            event.accept()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event: QKeyEvent) -> None:  # type: ignore[override]
        if not event.isAutoRepeat() and self._apply_keyboard_input(event.key(), False):
            event.accept()
            return
        super().keyReleaseEvent(event)

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self.pause_running()
        self.audio_player.stop()
        self.debugger_window.close()
        super().closeEvent(event)


def main() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    window = EmulatorWindow()
    window.show()
    return app.exec()
