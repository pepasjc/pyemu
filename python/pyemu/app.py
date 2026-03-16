from __future__ import annotations

import json
from datetime import datetime
import sys
import tempfile
from collections import deque
from pathlib import Path
from time import perf_counter

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QColor, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
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

from .display import create_display_backend
from .runtime import Emulator, RunState


class EmulatorWindow(QMainWindow):
    def __init__(self, system: str = "gameboy") -> None:
        super().__init__()
        self.emulator = Emulator(system=system)
        self.setWindowTitle(f"pyemu | {self.emulator.system.display_name} debugger")
        self.resize(1360, 860)

        self.status_label = QLabel()
        self.hardware_label = QLabel()
        self.cartridge_label = QLabel()
        self.performance_label = QLabel()
        self._manual_buttons = 0x0F
        self._manual_directions = 0x0F
        self._frame_budget = 0.0
        self._last_run_tick = perf_counter()
        self._fps_window_start = self._last_run_tick
        self._fps_frame_counter = 0
        self._display_fps = 0.0
        self._speed_ratio = 0.0
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
        self._trace_dir: Path | None = None
        self._trace_log_path: Path | None = None
        self._trace_recording = False
        self._trace_frame_counter = 0
        self._trace_snapshot_interval_frames = 15
        self._run_timer.setInterval(5)
        self._run_timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._run_timer.timeout.connect(self._on_run_timer)
        self.display_window = create_display_backend(self.emulator.system, f"pyemu | {self.emulator.system.display_name} display")
        self._start_trace_shortcut = QShortcut(QKeySequence("Q"), self)
        self._start_trace_shortcut.activated.connect(self.start_trace_recording)
        self._stop_trace_shortcut = QShortcut(QKeySequence("E"), self)
        self._stop_trace_shortcut.activated.connect(self.stop_trace_recording)

        self.registers_table = self._create_table(6, 2, ["Register", "Value"])
        self.flags_table = self._create_table(4, 2, ["Flag", "State"])
        self.flags_table.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.disassembly_table = self._create_table(36, 3, ["Address", "Bytes", "Instruction"])
        self.ram_view = QTextEdit()
        self.ram_view.setReadOnly(True)
        self.ram_view.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        self.vram_view = QTextEdit()
        self.vram_view.setReadOnly(True)
        self.vram_view.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        self.watch_table = self._create_table(10, 2, ["Name", "Value"])
        self.breakpoints_table = self._create_table(4, 3, ["Enabled", "Address", "Condition"])
        self.call_stack_table = self._create_table(6, 2, ["Frame", "Address"])
        self.registers_table.setColumnWidth(0, 90)
        self.flags_table.setColumnWidth(0, 90)

        self._seed_placeholder_tables()
        self._fit_table_height(self.flags_table)
        self.registers_table.setMinimumHeight(self._table_content_height(self.registers_table))

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)
        layout.addWidget(self._build_controls())
        layout.addWidget(self._build_debugger_layout(), 1)

        self.setCentralWidget(central)
        self._autoload_default_media()
        self.refresh()
        self.display_window.show()

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

    def _seed_placeholder_tables(self) -> None:
        for row, name in enumerate(["PC", "SP", "AF", "BC", "DE", "HL"]):
            self.registers_table.setItem(row, 0, QTableWidgetItem(name))
        for row, name in enumerate(["Z", "N", "H", "C"]):
            self.flags_table.setItem(row, 0, QTableWidgetItem(name))
        watch_names = [
            "last opcode",
            "entrypoint",
            "joypad FF00",
            "buttons FF80",
            "directions FF81",
            "IE/IF",
            "FF82 handshake",
            "FF97 counter",
            "FF9B state",
            "A298 save byte",
        ]
        for row, name in enumerate(watch_names):
            self.watch_table.setItem(row, 0, QTableWidgetItem(name))
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
            ("Start Trace", self.start_trace_recording),
            ("Stop Trace", self.stop_trace_recording),
            ("Rewind", self.rewind_state),
            ("Reset", self._reset_emulator),
            ("Run", self.start_running),
            ("Pause", self.pause_running),
            ("Step", self._step_instruction),
            ("Frame", self._step_frame),
            ("Run 1K", lambda: self._run_instructions(1_000)),
            ("Run 10K", lambda: self._run_instructions(10_000)),
            ("Run 100K", lambda: self._run_instructions(100_000)),
            ("Show Display", self._show_display_window),
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
        mid_split = QSplitter(Qt.Orientation.Horizontal)

        top_split.addWidget(self._wrap_group("Disassembly", self.disassembly_table))
        top_split.addWidget(self._build_right_pane())
        top_split.setSizes([760, 520])

        bottom_tabs = QTabWidget()
        bottom_tabs.addTab(self._wrap_group("Watch", self.watch_table), "Watch")
        bottom_tabs.addTab(self._wrap_group("Breakpoints", self.breakpoints_table), "Breakpoints")
        bottom_tabs.addTab(self._wrap_group("Call Stack", self.call_stack_table), "Call Stack")

        memory_tabs = QTabWidget()
        memory_tabs.addTab(self._wrap_group("RAM", self.ram_view), "RAM")
        memory_tabs.addTab(self._wrap_group("VRAM", self.vram_view), "VRAM")

        mid_split.addWidget(bottom_tabs)
        mid_split.addWidget(self._wrap_group("Memory", memory_tabs))
        mid_split.setSizes([520, 760])

        vertical_split.addWidget(top_split)
        vertical_split.addWidget(mid_split)
        vertical_split.setSizes([520, 300])

        layout.addWidget(vertical_split)
        return container

    def _build_right_pane(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        top_grid = QGridLayout()
        top_grid.setContentsMargins(0, 0, 0, 0)
        top_grid.setHorizontalSpacing(8)
        top_grid.setVerticalSpacing(8)

        info_panel = QWidget()
        info_panel.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Preferred)
        info_panel.setMinimumWidth(320)
        info_panel.setMaximumWidth(420)
        info_layout = QVBoxLayout(info_panel)
        info_layout.setContentsMargins(0, 0, 0, 0)
        info_layout.setSpacing(8)
        info_layout.addWidget(self._wrap_group("Session", self.status_label))
        info_layout.addWidget(self._wrap_group("Hardware", self.hardware_label))
        info_layout.addWidget(self._wrap_group("Cartridge", self.cartridge_label))
        info_layout.addWidget(self._build_input_group(), 0, Qt.AlignmentFlag.AlignTop)
        info_layout.addStretch(1)

        state_panel = QWidget()
        state_panel.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        state_panel.setMinimumWidth(340)
        state_layout = QVBoxLayout(state_panel)
        state_layout.setContentsMargins(0, 0, 0, 0)
        state_layout.setSpacing(8)
        registers_group = self._wrap_group("Registers", self.registers_table)
        flags_group = self._wrap_group("Flags", self.flags_table)
        flags_group.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        state_layout.addWidget(registers_group, 1)
        state_layout.addWidget(flags_group, 0, Qt.AlignmentFlag.AlignTop)

        top_grid.addWidget(info_panel, 0, 0, alignment=Qt.AlignmentFlag.AlignTop)
        top_grid.addWidget(state_panel, 0, 1)
        top_grid.setColumnMinimumWidth(1, 340)
        top_grid.setColumnStretch(0, 0)
        top_grid.setColumnStretch(1, 1)

        layout.addLayout(top_grid, 1)
        return panel

    def _wrap_group(self, title: str, widget: QWidget) -> QGroupBox:
        group = QGroupBox(title)
        layout = QVBoxLayout(group)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(widget)
        return group

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

        specs = [
            ("Up", "directions", 0x04, 0, 1),
            ("Left", "directions", 0x02, 1, 0),
            ("Down", "directions", 0x08, 1, 1),
            ("Right", "directions", 0x01, 1, 2),
            ("A", "buttons", 0x01, 0, 3),
            ("B", "buttons", 0x02, 0, 4),
            ("Select", "buttons", 0x04, 1, 3),
            ("Start", "buttons", 0x08, 1, 4),
        ]

        for label, bank, mask, row, column in specs:
            button = QPushButton(label)
            button.setCheckable(True)
            button.setMinimumHeight(24)
            button.pressed.connect(lambda bank=bank, mask=mask: self._set_virtual_input(bank, mask, True))
            button.released.connect(lambda bank=bank, mask=mask: self._set_virtual_input(bank, mask, False))
            layout.addWidget(button, row, column)

        return group

    def _set_virtual_input(self, bank: str, mask: int, pressed: bool) -> None:
        if bank == "buttons":
            self._manual_buttons = self._apply_input_mask(self._manual_buttons, mask, pressed)
        else:
            self._manual_directions = self._apply_input_mask(self._manual_directions, mask, pressed)

        buttons, directions = self._current_input_state()
        self.emulator.set_gameboy_joypad_state(buttons, directions)
        if self.emulator.run_state != RunState.RUNNING:
            self.refresh()

    def _apply_input_mask(self, value: int, mask: int, pressed: bool) -> int:
        if pressed:
            return value & (~mask & 0x0F)
        return value | mask

    def _drain_display_actions(self) -> None:
        for action in self.display_window.poll_actions():
            if action == "start_trace":
                self.start_trace_recording()
            elif action == "stop_trace":
                self.stop_trace_recording()

    def _current_input_state(self) -> tuple[int, int]:
        self._drain_display_actions()
        display_buttons, display_directions = self.display_window.poll_input()
        return display_buttons & self._manual_buttons, display_directions & self._manual_directions

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
        self.display_window.focus()

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
            self.emulator.step_instruction()

    def _reset_emulator(self) -> None:
        self.pause_running()
        self.emulator.reset()
        self._clear_rewind_history()
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._append_trace_entry("reset", buttons, directions, force_snapshot=True)
        self.refresh()
        self._show_display_window()
        self.start_running()

    def _step_instruction(self) -> None:
        self.pause_running()
        self.emulator.step_instruction()
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._trace_frame_counter += 1
        self._append_trace_entry("step_instruction", buttons, directions, force_snapshot=False)

    def _step_frame(self) -> None:
        self.pause_running()
        self.emulator.step_frame()
        self._record_rewind_snapshot(force=True)
        buttons, directions = self._current_input_state()
        self._trace_frame_counter += 1
        self._append_trace_entry("step_frame", buttons, directions, force_snapshot=False)

    def start_running(self) -> None:
        if not self.emulator.media.loaded:
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
        self._run_timer.start()
        self._show_display_window()
        self._present_display()

    def pause_running(self) -> None:
        was_running = self.emulator.run_state == RunState.RUNNING
        self._run_timer.stop()
        self._frame_budget = 0.0
        self.emulator.pause()
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
            buttons, directions = self._current_input_state()
            self.emulator.set_gameboy_joypad_state(buttons, directions)
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
        self.display_window.present(self.emulator.frame_buffer)

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

    def _load_last_media_path(self) -> Path | None:
        if not self._session_path.exists():
            return None
        try:
            data = json.loads(self._session_path.read_text())
        except (OSError, json.JSONDecodeError):
            return None
        path = data.get("last_media_path")
        if not path:
            return None
        candidate = Path(path).expanduser()
        return candidate if candidate.exists() else None

    def _save_last_media_path(self, path: str) -> None:
        payload = {"last_media_path": str(Path(path).expanduser())}
        try:
            self._session_path.write_text(json.dumps(payload, indent=2))
        except OSError:
            pass

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
            if candidate.exists() and self.emulator.load_media(str(candidate)):
                self._save_last_media_path(str(candidate))
                self._clear_rewind_history()
                self._record_rewind_snapshot(force=True)
                self._trace_frame_counter = 0
                self.refresh()
                self._show_display_window()
                self.start_running()
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
        buttons, directions = self._current_input_state()
        self._append_trace_entry("media_loaded", buttons, directions, force_snapshot=True)
        self.refresh()
        self._show_display_window()
        self.start_running()

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
        buttons, directions = self._current_input_state()
        self._append_trace_entry("state_loaded", buttons, directions, force_snapshot=True)
        self.refresh()

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
                f"Run state: {self.emulator.run_state.name}",
                f"Target FPS: {self.emulator.system.frame_rate:.3f}",
                f"Display: {self.display_window.info.label}",
                f"CPU halted: {'yes' if state.halted else 'no'}",
                f"CPU faulted: {'yes' if self.emulator.faulted else 'no'}",
                f"Cycles: {self.emulator.cycle_count}",
                f"DIV/TIMA: {memory[0xFF04]:02X}/{memory[0xFF05]:02X}",
                f"IF/IE: {memory[0xFF0F]:02X}/{memory[0xFFFF]:02X}",
                f"JOY/BTN/DIR: {memory[0xFF00]:02X}/{buttons:02X}/{directions:02X}",
                f"LY: {memory[0xFF44]:02X}",
                f"Trace: {'recording' if self._trace_recording else 'off'} ({self._trace_frame_counter} frames)",
                f"Core key: {self.emulator.system_name}",
            ]
            if wait_note:
                status_lines.append(f"Wait: {wait_note}")
            self.status_label.setText("\n".join(status_lines))
            self.hardware_label.setText(self._format_hardware_access(access, media))
            self.cartridge_label.setText(self._format_cartridge_debug(cartridge, memory))
            self._update_registers(state)
            self._update_watch_and_stack(memory, state, buttons, directions)
            self._update_disassembly(memory, state.pc)
            self._update_memory(memory, state.pc)
            self.display_window.present(frame)
        finally:
            self._refresh_in_progress = False

    def _update_registers(self, state) -> None:
        register_rows = [
            ("PC", f"0x{state.pc:04X}"),
            ("SP", f"0x{state.sp:04X}"),
            ("AF", f"0x{state.a:02X}{state.f:02X}"),
            ("BC", f"0x{state.b:02X}{state.c:02X}"),
            ("DE", f"0x{state.d:02X}{state.e:02X}"),
            ("HL", f"0x{state.h:02X}{state.l:02X}"),
        ]
        for row, (_, value) in enumerate(register_rows):
            self.registers_table.setItem(row, 1, QTableWidgetItem(value))

        flags = [
            ("Z", bool(state.f & 0x80)),
            ("N", bool(state.f & 0x40)),
            ("H", bool(state.f & 0x20)),
            ("C", bool(state.f & 0x10)),
        ]
        for row, (_, enabled) in enumerate(flags):
            self.flags_table.setItem(row, 1, QTableWidgetItem("set" if enabled else "clear"))
        self._fit_table_height(self.flags_table)

    def _update_watch_and_stack(self, memory: list[int], state, buttons: int, directions: int) -> None:
        watch_values = [
            f"0x{memory[0xC000]:02X}" if len(memory) > 0xC000 else "n/a",
            f"0x{memory[0x0100]:02X}" if len(memory) > 0x0100 else "n/a",
            f"0x{memory[0xFF00]:02X}" if len(memory) > 0xFF00 else "n/a",
            f"0x{buttons:02X}",
            f"0x{directions:02X}",
            f"0x{memory[0xFFFF]:02X}/0x{memory[0xFF0F]:02X}" if len(memory) > 0xFFFF else "n/a",
            f"0x{memory[0xFF82]:02X}" if len(memory) > 0xFF82 else "n/a",
            f"0x{memory[0xFF97]:02X}" if len(memory) > 0xFF97 else "n/a",
            f"0x{memory[0xFF9B]:02X}" if len(memory) > 0xFF9B else "n/a",
            f"0x{memory[0xA298]:02X}" if len(memory) > 0xA298 else "n/a",
        ]
        for row, value in enumerate(watch_values):
            self.watch_table.setItem(row, 1, QTableWidgetItem(value))

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
        self.ram_view.setPlainText(self._format_memory_region(memory, 0xC000, 0xE000, pc))
        self.vram_view.setPlainText(self._format_memory_region(memory, 0x8000, 0xA000, pc))

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

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self.pause_running()
        self.display_window.close()
        super().closeEvent(event)


def main() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    window = EmulatorWindow()
    window.show()
    return app.exec()
