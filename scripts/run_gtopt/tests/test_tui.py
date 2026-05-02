# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the Rich terminal UI module."""

import json
import threading
import time
from pathlib import Path
from unittest.mock import patch

from run_gtopt._tui import (
    SDDPGridTracker,
    SolverDisplay,
    SolverPhaseTracker,
    _build_command_bar,
    _build_header,
    _build_help_overlay,
    _build_history,
    _build_log,
    _build_plan_panel,
    _build_progress,
    _build_sddp_grid,
    _build_stats,
    _build_stats_overlay,
    _build_system,
    _enter_cbreak,
    _exit_cbreak,
    _find_planning_json,
    _find_status_file,
    _format_elapsed,
    _format_number,
    _GRID_APERTURE,
    _GRID_BACKWARD,
    _GRID_ELASTIC,
    _GRID_FORWARD,
    _GRID_IDLE,
    _GRID_INFEASIBLE,
    _load_status,
    _load_system_stats,
    _poll_key,
    _sparkline,
    is_interactive,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_SAMPLE_STATUS = {
    "version": 1,
    "timestamp": 1711234567.0,
    "elapsed_s": 42.5,
    "status": "running",
    "iteration": 5,
    "lower_bound": 1234567.89,
    "upper_bound": 1345678.90,
    "gap": 0.0826,
    "converged": False,
    "max_iterations": 100,
    "min_iterations": 3,
    "current_pass": 1,
    "scenes_done": 3,
    "history": [
        {
            "iteration": i,
            "lower_bound": 1000000 + i * 50000,
            "upper_bound": 1500000 - i * 30000,
            "gap": 0.3 - i * 0.05,
            "converged": False,
            "cuts_added": 4 + i,
            "infeasible_cuts_added": 0,
            "forward_pass_s": 2.1 + i * 0.1,
            "backward_pass_s": 3.5 + i * 0.2,
            "iteration_s": 5.6 + i * 0.3,
            "scene_upper_bounds": [750000 - i * 15000],
            "scene_lower_bounds": [500000 + i * 25000],
        }
        for i in range(1, 6)
    ],
    "realtime": {
        "timestamps": [0.5 * i for i in range(10)],
        "cpu_loads": [30.0 + i * 5 for i in range(10)],
        "active_workers": [4] * 10,
    },
}


def _make_planning_json(tmp_path: Path) -> Path:
    """Write a minimal planning JSON and return its path."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "generator_array": [
                {"uid": 1, "name": "g1"},
                {"uid": 2, "name": "g2"},
            ],
            "demand_array": [{"uid": 1, "name": "d1"}],
            "line_array": [{"uid": 1, "name": "l1"}],
        },
        "simulation": {
            "scenario_array": [{"uid": 0}],
            "stage_array": [{"uid": 0}, {"uid": 1}],
            "block_array": [{"uid": 0}, {"uid": 1}, {"uid": 2}],
        },
        "options": {
            "method": "sddp",
            "scale_objective": 1000,
            "use_kirchhoff": True,
            "use_single_bus": False,
        },
    }
    case_dir = tmp_path / "test_case"
    case_dir.mkdir()
    json_path = case_dir / "test_case.json"
    json_path.write_text(json.dumps(planning), encoding="utf-8")
    return json_path


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------


def test_sparkline_empty():
    assert _sparkline([]) == ""


def test_sparkline_single():
    result = _sparkline([5.0])
    assert len(result) == 1


def test_sparkline_rising():
    result = _sparkline([0.0, 0.5, 1.0])
    assert len(result) == 3
    # First char should be smallest block, last should be tallest
    assert result[0] <= result[-1]


def test_sparkline_truncates():
    values = list(range(100))
    result = _sparkline(values, width=10)
    assert len(result) == 10


def test_format_elapsed_short():
    assert _format_elapsed(65) == "01:05"


def test_format_elapsed_hours():
    assert _format_elapsed(3661) == "1:01:01"


def test_format_elapsed_zero():
    assert _format_elapsed(0) == "00:00"


def test_format_number_large():
    result = _format_number(1234567890.0)
    assert "," in result  # thousands separators


def test_format_number_medium():
    result = _format_number(1234567.0)
    assert "." in result


def test_format_number_small():
    result = _format_number(0.123)
    assert result == "0.123000"


# ---------------------------------------------------------------------------
# Status file loading
# ---------------------------------------------------------------------------


def test_load_status_missing():
    assert _load_status(Path("/no/such/file.json")) == {}


def test_load_status_none():
    assert _load_status(None) == {}


def test_load_status_valid(tmp_path: Path):
    f = tmp_path / "status.json"
    f.write_text(json.dumps({"status": "running", "iteration": 3}))
    result = _load_status(f)
    assert result["status"] == "running"
    assert result["iteration"] == 3


def test_load_status_corrupt(tmp_path: Path):
    f = tmp_path / "status.json"
    f.write_text("{invalid json")
    assert _load_status(f) == {}


# ---------------------------------------------------------------------------
# Status file discovery
# ---------------------------------------------------------------------------


def test_find_status_file_solver(tmp_path: Path):
    output = tmp_path / "output"
    output.mkdir()
    status = output / "solver_status.json"
    status.write_text("{}")
    result = _find_status_file(tmp_path)
    assert result == status


def test_find_status_file_legacy_sddp(tmp_path: Path):
    """Falls back to legacy sddp_status.json for older builds."""
    output = tmp_path / "output"
    output.mkdir()
    sddp = output / "sddp_status.json"
    sddp.write_text("{}")
    result = _find_status_file(tmp_path)
    assert result == sddp


def test_find_status_file_legacy_monolithic(tmp_path: Path):
    """Falls back to legacy monolithic_status.json for older builds."""
    output = tmp_path / "output"
    output.mkdir()
    mono = output / "monolithic_status.json"
    mono.write_text("{}")
    result = _find_status_file(tmp_path)
    assert result == mono


def test_find_status_file_json_input(tmp_path: Path):
    """When case_dir is a JSON file, look in its parent's output/."""
    output = tmp_path / "output"
    output.mkdir()
    status = output / "solver_status.json"
    status.write_text("{}")
    json_file = tmp_path / "case.json"
    json_file.write_text("{}")
    result = _find_status_file(json_file)
    assert result == status


def test_find_status_file_fallback(tmp_path: Path):
    """When no status files exist, returns solver_status.json as default."""
    result = _find_status_file(tmp_path)
    assert result.name == "solver_status.json"


# ---------------------------------------------------------------------------
# Planning JSON discovery and stats loading
# ---------------------------------------------------------------------------


def test_find_planning_json_directory(tmp_path: Path):
    json_path = _make_planning_json(tmp_path)
    case_dir = json_path.parent
    result = _find_planning_json(case_dir)
    assert result == json_path


def test_find_planning_json_file(tmp_path: Path):
    json_path = _make_planning_json(tmp_path)
    result = _find_planning_json(json_path)
    assert result == json_path


def test_find_planning_json_missing(tmp_path: Path):
    result = _find_planning_json(tmp_path / "nonexistent")
    assert result is None


def test_load_system_stats(tmp_path: Path):
    json_path = _make_planning_json(tmp_path)
    stats = _load_system_stats(json_path)
    assert stats["elements"]["Bus"] == 1
    assert stats["elements"]["Generator"] == 2
    assert stats["elements"]["Demand"] == 1
    assert stats["elements"]["Line"] == 1
    assert stats["scenarios"] == 1
    assert stats["stages"] == 2
    assert stats["blocks"] == 3
    assert stats["method"] == "sddp"
    assert stats["scale_objective"] == 1000


def test_load_system_stats_missing():
    assert not _load_system_stats(None)


def test_load_system_stats_corrupt(tmp_path: Path):
    f = tmp_path / "bad.json"
    f.write_text("not json")
    assert not _load_system_stats(f)


# ---------------------------------------------------------------------------
# is_interactive
# ---------------------------------------------------------------------------


def test_is_interactive_in_tests():
    """In pytest, stdout is typically not a TTY."""
    result = is_interactive()
    assert isinstance(result, bool)


def test_is_interactive_true():
    with patch("run_gtopt._tui.sys") as mock_sys:
        mock_sys.stdout.isatty.return_value = True
        import run_gtopt._tui as tui_mod

        original = tui_mod.sys
        tui_mod.sys = mock_sys
        try:
            assert tui_mod.is_interactive() is True
        finally:
            tui_mod.sys = original


# ---------------------------------------------------------------------------
# Keyboard helpers
# ---------------------------------------------------------------------------


def test_enter_exit_cbreak_no_tty():
    """In tests (no real TTY), cbreak setup returns None."""
    # This may or may not work depending on the test runner's stdin
    old = _enter_cbreak()
    _exit_cbreak(old)
    # Just ensure no crash


def test_poll_key_inactive():
    """When cbreak is not active, _poll_key returns None."""
    assert _poll_key(False) is None


# ---------------------------------------------------------------------------
# Panel builders (smoke tests — ensure they produce Rich renderables)
# ---------------------------------------------------------------------------


def test_build_header_running():
    panel = _build_header("test_case", _SAMPLE_STATUS, 42.5)
    assert panel is not None
    assert panel.title is not None


def test_build_header_empty():
    panel = _build_header("empty", {}, 0.0)
    assert panel is not None


def test_build_header_converged():
    data = {**_SAMPLE_STATUS, "status": "converged", "converged": True}
    panel = _build_header("case", data, 100.0)
    assert panel is not None


def test_build_progress_sddp():
    panel = _build_progress(_SAMPLE_STATUS)
    assert panel is not None


def test_build_progress_monolithic():
    panel = _build_progress({"current_pass": 0})
    assert panel is not None


def test_build_progress_with_scenes():
    data = {**_SAMPLE_STATUS, "scenes_done": 5}
    panel = _build_progress(data)
    assert panel is not None


def test_build_stats():
    panel = _build_stats(_SAMPLE_STATUS)
    assert panel is not None


def test_build_stats_empty():
    panel = _build_stats({"gap": 0.0, "converged": False})
    assert panel is not None


def test_build_stats_converged():
    data = {
        **_SAMPLE_STATUS,
        "gap": 0.001,
        "converged": True,
    }
    panel = _build_stats(data)
    assert panel is not None


def test_build_history_with_data():
    panel = _build_history(_SAMPLE_STATUS)
    assert panel is not None


def test_build_history_empty():
    panel = _build_history({})
    assert panel is not None


def test_build_system_with_data():
    panel = _build_system(_SAMPLE_STATUS)
    assert panel is not None


def test_build_system_empty():
    panel = _build_system({})
    assert panel is not None


def test_build_log_empty():
    panel = _build_log([])
    assert panel is not None


def test_build_log_with_lines():
    lines = [
        "SDDP: === iteration 1 / 100 ===",
        "warning: something",
        "error: bad thing",
        "converged at iteration 50",
        "normal log line",
    ]
    panel = _build_log(lines)
    assert panel is not None


def test_build_command_bar():
    panel = _build_command_bar(stop_sent=False)
    assert panel is not None


def test_build_command_bar_stop_sent():
    panel = _build_command_bar(stop_sent=True)
    assert panel is not None


def test_build_help_overlay():
    panel = _build_help_overlay()
    assert panel is not None


def test_build_stats_overlay_with_data(tmp_path: Path):
    json_path = _make_planning_json(tmp_path)
    stats = _load_system_stats(json_path)
    panel = _build_stats_overlay(stats)
    assert panel is not None


def test_build_stats_overlay_empty():
    panel = _build_stats_overlay({})
    assert panel is not None


# ---------------------------------------------------------------------------
# SolverDisplay
# ---------------------------------------------------------------------------


def test_solver_display_construction(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    assert display.case_name == "test"
    assert not display.quit_requested.is_set()


def test_solver_display_add_log_line(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display.add_log_line("hello world\n")
    display.add_log_line("second line")
    assert len(display._log_lines) == 2
    assert display._log_lines[0] == "hello world"
    assert display._log_lines[1] == "second line"


def test_solver_display_log_line_cap(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    for i in range(50):
        display.add_log_line(f"line {i}")
    assert len(display._log_lines) <= 16


def test_solver_display_start_stop(tmp_path: Path):
    """Display thread starts and stops without crashing."""
    display = SolverDisplay(
        case_name="test",
        case_dir=tmp_path,
        poll_interval=0.1,
    )
    display.start()
    assert display._thread is not None
    assert display._thread.is_alive()
    time.sleep(0.3)
    display.stop()
    assert not display._thread.is_alive()


def test_solver_display_reads_status(tmp_path: Path):
    """Display picks up the status file when it appears."""
    output = tmp_path / "output"
    output.mkdir()

    display = SolverDisplay(
        case_name="test",
        case_dir=tmp_path,
        poll_interval=0.1,
    )
    display.start()

    # Write status file after start
    status_file = output / "solver_status.json"
    status_file.write_text(json.dumps({"status": "running", "iteration": 3}))

    time.sleep(0.5)
    display.stop()

    assert display._status.get("status") == "running"


def test_solver_display_print_final_success(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._status = {"converged": True, "iteration": 50, "gap": 0.001}
    display.print_final(0)


def test_solver_display_print_final_failure(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display.print_final(1)


def test_solver_display_print_final_stopped(tmp_path: Path):
    """Print final shows 'Stopped' when graceful stop was sent."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._stop_sent = True
    display.print_final(1)


def test_solver_display_thread_safety(tmp_path: Path):
    """Concurrent add_log_line calls don't crash."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)

    def writer(prefix: str):
        for i in range(100):
            display.add_log_line(f"{prefix}_{i}")

    threads = [threading.Thread(target=writer, args=(f"t{t}",)) for t in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert len(display._log_lines) == 16  # capped by deque maxlen


# ---------------------------------------------------------------------------
# Interactive commands
# ---------------------------------------------------------------------------


def test_handle_key_help_toggle(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    assert not display._show_help
    display._handle_key("h")
    assert display._show_help
    display._handle_key("h")
    assert not display._show_help


def test_handle_key_stats_toggle(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    assert not display._show_stats
    display._handle_key("i")
    assert display._show_stats
    assert not display._show_help  # mutually exclusive
    display._handle_key("i")
    assert not display._show_stats


def test_handle_key_help_dismisses_stats(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._handle_key("i")
    assert display._show_stats
    display._handle_key("h")
    assert display._show_help
    assert not display._show_stats


def test_handle_key_stats_dismisses_help(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._handle_key("h")
    assert display._show_help
    display._handle_key("i")
    assert display._show_stats
    assert not display._show_help


def test_handle_key_quit(tmp_path: Path):
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    assert not display.quit_requested.is_set()
    display._handle_key("q")
    assert display.quit_requested.is_set()


def test_handle_key_uppercase(tmp_path: Path):
    """Commands work with uppercase keys too."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._handle_key("H")
    assert display._show_help
    display._handle_key("I")
    assert display._show_stats
    display._handle_key("Q")
    assert display.quit_requested.is_set()


def test_handle_key_question_mark(tmp_path: Path):
    """? is an alias for help."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._handle_key("?")
    assert display._show_help


def test_cmd_stop_creates_file(tmp_path: Path):
    """Graceful stop creates the stop-request JSON file."""
    output = tmp_path / "output"
    output.mkdir()

    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    assert not display._stop_sent

    display._cmd_stop()

    assert display._stop_sent
    stop_file = output / "sddp_stop_request.json"
    assert stop_file.is_file()
    data = json.loads(stop_file.read_text(encoding="utf-8"))
    assert "requested_at" in data
    assert data["source"] == "run_gtopt_tui"


def test_cmd_stop_idempotent(tmp_path: Path):
    """Calling stop twice does not error or overwrite."""
    output = tmp_path / "output"
    output.mkdir()

    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._cmd_stop()
    first_content = (output / "sddp_stop_request.json").read_text()
    display._cmd_stop()  # second call is a no-op
    assert display._stop_sent
    second_content = (output / "sddp_stop_request.json").read_text()
    assert first_content == second_content


def test_cmd_stop_adds_log_line(tmp_path: Path):
    """Stop command appends a message to the log buffer."""
    (tmp_path / "output").mkdir()
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._cmd_stop()
    assert any("stop" in line.lower() for line in display._log_lines)


def test_cmd_stop_creates_output_dir(tmp_path: Path):
    """Stop command creates the output directory if needed."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._cmd_stop()
    assert (tmp_path / "output" / "sddp_stop_request.json").is_file()


def test_cmd_stop_json_file_input(tmp_path: Path):
    """Stop works when case_dir is a JSON file (uses parent)."""
    case_dir = tmp_path / "case"
    case_dir.mkdir()
    json_file = case_dir / "case.json"
    json_file.write_text("{}")

    display = SolverDisplay(case_name="test", case_dir=json_file)
    display._cmd_stop()

    assert (case_dir / "output" / "sddp_stop_request.json").is_file()


def test_handle_key_stop_via_s(tmp_path: Path):
    """'s' key triggers the stop command."""
    (tmp_path / "output").mkdir()
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display._handle_key("s")
    assert display._stop_sent
    assert (tmp_path / "output" / "sddp_stop_request.json").is_file()


def test_system_stats_loaded_on_start(tmp_path: Path):
    """System stats are eagerly loaded on start() so method is displayed from the first frame."""
    json_path = _make_planning_json(tmp_path)
    case_dir = json_path.parent

    display = SolverDisplay(
        case_name="test",
        case_dir=case_dir,
        poll_interval=0.1,
    )
    display.start()
    time.sleep(0.2)
    display.stop()

    # Stats are loaded eagerly at startup for immediate display
    assert display._system_stats.get("elements") is not None
    assert display._system_stats.get("method") == "sddp"


def test_system_stats_lazy_loaded_on_i_key(tmp_path: Path):
    """System stats are lazy-loaded from planning JSON on 'i' key press."""
    json_path = _make_planning_json(tmp_path)
    case_dir = json_path.parent

    display = SolverDisplay(
        case_name="test",
        case_dir=case_dir,
        poll_interval=0.1,
    )
    display._handle_key("i")

    assert display._show_stats is True
    assert display._system_stats.get("elements", {}).get("Generator") == 2


# ---------------------------------------------------------------------------
# SolverPhaseTracker
# ---------------------------------------------------------------------------


def test_phase_tracker_initial_state():
    tracker = SolverPhaseTracker()
    assert all(st.status == "pending" for st in tracker.states.values())
    assert len(tracker.order) == 7


def test_phase_tracker_sddp_sequence():
    """Full SDDP phase flow: parse → validate → build → optimize → sim → solution → write."""
    tracker = SolverPhaseTracker()
    lines = [
        "[00:00:01] Parsing input file case.json",
        "[00:00:02] Parse all input files time 1.000s",
        "[00:00:02] Planning validation passed",
        "[00:00:02] === Building LP model ===",
        "[00:00:10] Build lp time 8.000s",
        "[00:00:10] === System optimization ===",
        "[00:00:10] SDDPMethod: starting 1 scene(s)",
        "[00:01:00] SDDP: === iteration 3 / 99 ===",
        "[00:02:00] SDDP: === simulation pass (iter 4) ===",
        "[00:02:30] SDDP: simulation pass done in 30.000s",
        "[00:02:30] === Solution statistics ===",
        "[00:02:30] === Output writing ===",
        "[00:02:35] Write output time 5.000s",
    ]
    for line in lines:
        tracker.process_line(line)

    assert tracker.states["parse"].status == "done"
    assert tracker.states["validate"].status == "done"
    assert tracker.states["build_lp"].status == "done"
    assert tracker.states["optimize"].status == "done"
    assert tracker.states["sim_pass"].status == "done"
    assert tracker.states["solution"].status == "done"
    assert tracker.states["write"].status == "done"


def test_phase_tracker_monolithic_skips_sim_pass():
    """Monolithic method auto-skips the simulation pass phase."""
    tracker = SolverPhaseTracker()
    tracker.process_line("[00:00:10] === System optimization ===")
    tracker.process_line("[00:00:10] MonolithicMethod: starting 4 scene(s)")
    assert tracker.states["sim_pass"].status == "skipped"


def test_phase_tracker_detail_sddp_iteration():
    """SDDP iteration detail string is updated on the optimize phase."""
    tracker = SolverPhaseTracker()
    tracker.process_line("=== System optimization ===")
    assert tracker.states["optimize"].status == "active"
    tracker.process_line("SDDP: === iteration 5 / 99 ===")
    assert tracker.states["optimize"].detail == "iter 5/99"


def test_phase_tracker_detail_monolithic_scene():
    """Monolithic scene progress updates the detail string."""
    tracker = SolverPhaseTracker()
    tracker.process_line("=== System optimization ===")
    tracker.process_line("MonolithicMethod: scene 2 done in 3.000s (2/4)")
    assert tracker.states["optimize"].detail == "scene 2/4"


def test_phase_tracker_finish_all():
    """finish_all() marks the active phase as done."""
    tracker = SolverPhaseTracker()
    tracker.process_line("=== Building LP model ===")
    assert tracker.states["build_lp"].status == "active"
    tracker.finish_all()
    assert tracker.states["build_lp"].status == "done"
    assert tracker.states["build_lp"].elapsed >= 0.0  # near-instant


def test_phase_tracker_instant_validate():
    """Validate goes directly from pending to done (no explicit start)."""
    tracker = SolverPhaseTracker()
    assert tracker.states["validate"].status == "pending"
    tracker.process_line("Planning validation passed")
    assert tracker.states["validate"].status == "done"


def test_phase_tracker_tick():
    tracker = SolverPhaseTracker()
    assert tracker.frame == 0
    tracker.tick()
    tracker.tick()
    assert tracker.frame == 2


def test_build_plan_panel_smoke():
    """Plan panel renders without error for mixed states."""
    tracker = SolverPhaseTracker()
    tracker.process_line("Parsing input file case.json")
    tracker.process_line("Parse all input files time 0.1s")
    tracker.process_line("=== Building LP model ===")
    panel = _build_plan_panel(tracker)
    assert panel is not None


def test_add_log_line_feeds_tracker(tmp_path: Path):
    """SolverDisplay.add_log_line() updates the phase tracker."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display.add_log_line("=== Building LP model ===\n")
    assert display._phase_tracker.states["build_lp"].status == "active"


# ---------------------------------------------------------------------------
# SDDPGridTracker tests
# ---------------------------------------------------------------------------


def test_grid_tracker_empty():
    """Empty tracker has no data."""
    tracker = SDDPGridTracker()
    assert not tracker.has_data
    assert tracker.scenes == []
    assert tracker.max_phase == 0
    assert tracker.max_iter == 0


def test_grid_tracker_forward_pass():
    """Forward pass lines populate the grid correctly."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i0 s0 p0]: solving phase")
    tracker.process_line("SDDP Forward [i0 s0 p1]: solving phase")
    tracker.process_line("SDDP Forward [i0 s0 p2]: solving phase")

    assert tracker.has_data
    assert tracker.scenes == [0]
    assert tracker.max_phase == 2
    assert tracker.max_iter == 0
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 1) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 2) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 3) == _GRID_IDLE  # not visited


def test_grid_tracker_forward_pass_with_phase_total():
    """Forward INFO lines emit ``p<idx>/<total>`` (e.g. ``p3/51``); the
    grid tracker must accept the suffix without dropping the line.
    Pinned after the suffix was added in sddp_forward_pass.cpp and the
    regex initially rejected it, freezing the grid mid-run."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i0 s0 p0/3]: opex=1.23M")
    tracker.process_line("SDDP Forward [i0 s0 p1/3]: opex=2.34M")
    tracker.process_line("SDDP Forward [i0 s0 p2/3]: opex=3.45M")

    assert tracker.has_data
    assert tracker.scenes == [0]
    assert tracker.max_phase == 2
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 1) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 2) == _GRID_FORWARD


def test_grid_tracker_backward_pass():
    """Backward pass lines create backward cells."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i1 s0 p0]: ok")
    tracker.process_line("SDDP Forward [i1 s0 p1]: ok")
    tracker.process_line("SDDP Backward [i1 s0 p1]: adding cut")
    tracker.process_line("SDDP Backward [i1 s0 p0]: adding cut")

    assert tracker.get_cell(0, 1, 0) == _GRID_BACKWARD  # overwritten by backward
    assert tracker.get_cell(0, 1, 1) == _GRID_BACKWARD


def test_grid_tracker_elastic():
    """Elastic keyword in forward line triggers elastic state."""
    tracker = SDDPGridTracker()
    tracker.process_line(
        "SDDP Forward [i2 s0 p5]: non-optimal (status 2), trying elastic solve"
    )

    assert tracker.get_cell(0, 2, 5) == _GRID_ELASTIC
    assert tracker.elastic_count == 1


def test_grid_tracker_aperture():
    """Aperture tag creates aperture cells."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Aperture [i3 s1 p2 a0]: solving aperture")
    tracker.process_line("SDDP Aperture [i3 s1 p2 a1]: solving aperture")

    assert tracker.get_cell(1, 3, 2) == _GRID_APERTURE
    assert tracker.aperture_count == 2


def test_grid_tracker_backward_with_aperture():
    """Backward with aperture uid is treated as aperture."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Backward [i3 s1 p2 a0]: cut added")

    assert tracker.get_cell(1, 3, 2) == _GRID_APERTURE
    assert tracker.aperture_count == 1


def test_grid_tracker_kappa_warning():
    """Kappa warnings increment counter but don't change grid."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Kappa [i0 s0 p3]: condition number 1.2e+10")

    assert tracker.get_cell(0, 0, 3) == _GRID_IDLE  # kappa doesn't set cells
    assert tracker.kappa_warnings == 1


def test_grid_tracker_infeasible():
    """Lines containing 'infeasib' set the infeasible state."""
    tracker = SDDPGridTracker()
    tracker.process_line(
        "SDDP Forward [i0 s0 p0]: scene 0 is infeasible (backpropagated)"
    )

    assert tracker.get_cell(0, 0, 0) == _GRID_INFEASIBLE
    assert tracker.infeasible_count == 1


def test_grid_tracker_multiple_scenes():
    """Multiple scenes are tracked independently."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i0 s0 p0]: ok")
    tracker.process_line("SDDP Forward [i0 s1 p0]: ok")
    tracker.process_line("SDDP Forward [i0 s2 p0]: ok")

    assert tracker.scenes == [0, 1, 2]
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(1, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(2, 0, 0) == _GRID_FORWARD


def test_grid_tracker_priority():
    """Higher-priority states overwrite lower ones."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i0 s0 p5]: ok")
    assert tracker.get_cell(0, 0, 5) == _GRID_FORWARD

    tracker.process_line("SDDP Backward [i0 s0 p5]: cut")
    assert tracker.get_cell(0, 0, 5) == _GRID_BACKWARD

    tracker.process_line("SDDP Forward [i0 s0 p5]: elastic")  # elastic > backward
    assert tracker.get_cell(0, 0, 5) == _GRID_ELASTIC


def test_grid_tracker_active_cell():
    """Active cell tracks the last processed (scene, iter, phase)."""
    tracker = SDDPGridTracker()
    assert tracker.active_cell is None

    tracker.process_line("SDDP Forward [i2 s1 p7]: ok")
    assert tracker.active_cell == (1, 2, 7)


def test_grid_tracker_ignores_non_sddp():
    """Non-SDDP lines are ignored."""
    tracker = SDDPGridTracker()
    tracker.process_line("MonolithicMethod: solving scene 0")
    tracker.process_line("Parse all input files time 0.5s")
    assert not tracker.has_data


def test_grid_tracker_ignores_iter_tag():
    """Iter/Sim/Init tags without phase info are ignored."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Iter [i5 s0 p0]: === 5/100 ===")
    # Iter tag returns early — no grid data
    assert not tracker.has_data


def test_build_sddp_grid_panel():
    """Grid panel renders without error."""
    tracker = SDDPGridTracker()
    tracker.process_line("SDDP Forward [i0 s0 p0]: ok")
    tracker.process_line("SDDP Forward [i0 s0 p1]: ok")
    tracker.process_line("SDDP Backward [i0 s0 p1]: cut")
    tracker.process_line("SDDP Forward [i1 s0 p0]: ok")
    tracker.process_line("SDDP Forward [i1 s0 p1]: ok")
    panel = _build_sddp_grid(tracker)
    assert panel is not None


def test_grid_tracker_display_integration(tmp_path: Path):
    """SolverDisplay.add_log_line feeds the grid tracker."""
    display = SolverDisplay(case_name="test", case_dir=tmp_path)
    display.add_log_line("SDDP Forward [i0 s0 p3]: solving")
    assert display._grid_tracker.has_data
    assert display._grid_tracker.get_cell(0, 0, 3) == _GRID_FORWARD


# ---------------------------------------------------------------------------
# SDDPGridTracker.load_from_status tests (remote monitoring)
# ---------------------------------------------------------------------------


def test_grid_load_from_status_basic():
    """Grid can be populated from status JSON phase_grid data."""
    tracker = SDDPGridTracker()
    status = {
        "phase_grid": {
            "rows": [
                {"i": 0, "s": 0, "cells": "FF.FEB"},
                {"i": 1, "s": 0, "cells": "FFFFFF"},
            ],
        },
    }
    tracker.load_from_status(status)

    assert tracker.has_data
    assert tracker.scenes == [0]
    assert tracker.max_iter == 1
    assert tracker.max_phase == 5
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 1) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 2) == _GRID_IDLE  # '.' = idle
    assert tracker.get_cell(0, 0, 3) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 4) == _GRID_ELASTIC
    assert tracker.get_cell(0, 0, 5) == _GRID_BACKWARD
    assert tracker.get_cell(0, 1, 0) == _GRID_FORWARD


def test_grid_load_from_status_multi_scene():
    """Multiple scenes from status JSON."""
    tracker = SDDPGridTracker()
    status = {
        "phase_grid": {
            "rows": [
                {"i": 0, "s": 0, "cells": "FB"},
                {"i": 0, "s": 1, "cells": "FA"},
            ],
        },
    }
    tracker.load_from_status(status)

    assert tracker.scenes == [0, 1]
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(0, 0, 1) == _GRID_BACKWARD
    assert tracker.get_cell(1, 0, 0) == _GRID_FORWARD
    assert tracker.get_cell(1, 0, 1) == _GRID_APERTURE


def test_grid_load_from_status_merges_with_log():
    """Status JSON data merges with log-parsed data (higher priority wins)."""
    tracker = SDDPGridTracker()
    # Log line sets forward
    tracker.process_line("SDDP Forward [i0 s0 p0]: ok")
    assert tracker.get_cell(0, 0, 0) == _GRID_FORWARD

    # Status JSON has backward (higher priority) for same cell
    status = {"phase_grid": {"rows": [{"i": 0, "s": 0, "cells": "B"}]}}
    tracker.load_from_status(status)
    assert tracker.get_cell(0, 0, 0) == _GRID_BACKWARD


def test_grid_load_from_status_empty():
    """Empty or missing phase_grid is a no-op."""
    tracker = SDDPGridTracker()
    tracker.load_from_status({})
    assert not tracker.has_data
    tracker.load_from_status({"phase_grid": {}})
    assert not tracker.has_data
    tracker.load_from_status({"phase_grid": {"rows": []}})
    assert not tracker.has_data


def test_grid_load_infeasible_from_status():
    """Infeasible cells from status JSON."""
    tracker = SDDPGridTracker()
    status = {"phase_grid": {"rows": [{"i": 2, "s": 1, "cells": "FXF"}]}}
    tracker.load_from_status(status)
    assert tracker.get_cell(1, 2, 1) == _GRID_INFEASIBLE
    assert tracker.get_cell(1, 2, 0) == _GRID_FORWARD
