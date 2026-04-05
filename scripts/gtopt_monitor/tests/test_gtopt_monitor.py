# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for gtopt_monitor."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_monitor.gtopt_monitor import (
    find_planning_json,
    get_option,
    load_status,
    main,
    print_status,
    run_gui,
    run_text,
)


# ---------------------------------------------------------------------------
# Sample data fixtures
# ---------------------------------------------------------------------------


@pytest.fixture()
def sample_status() -> dict:
    """Return a minimal SDDP status dictionary."""
    return {
        "status": "running",
        "iteration": 5,
        "gap": 0.012345,
        "lower_bound": 1000.0,
        "upper_bound": 1012.5,
        "elapsed_s": 42.3,
    }


@pytest.fixture()
def converged_status() -> dict:
    """Return a status dictionary indicating convergence."""
    return {
        "status": "converged",
        "iteration": 20,
        "gap": 0.0001,
        "lower_bound": 5000.0,
        "upper_bound": 5000.5,
        "elapsed_s": 120.7,
    }


@pytest.fixture()
def status_file(tmp_path: Path, sample_status: dict) -> Path:
    """Write sample_status to a temporary JSON file and return the path."""
    fpath = tmp_path / "solver_status.json"
    fpath.write_text(json.dumps(sample_status), encoding="utf-8")
    return fpath


# ---------------------------------------------------------------------------
# Tests for load_status
# ---------------------------------------------------------------------------


class TestLoadStatus:
    """Tests for the load_status helper."""

    def test_load_valid_json(self, status_file: Path, sample_status: dict) -> None:
        """Loading a valid JSON file returns the parsed dictionary."""
        result = load_status(status_file)
        assert result is not None
        assert result["iteration"] == sample_status["iteration"]
        assert result["status"] == "running"

    def test_load_missing_file(self, tmp_path: Path) -> None:
        """Loading a non-existent file returns None."""
        result = load_status(tmp_path / "does_not_exist.json")
        assert result is None

    def test_load_invalid_json(self, tmp_path: Path) -> None:
        """Loading a file with malformed JSON returns None."""
        bad_file = tmp_path / "bad.json"
        bad_file.write_text("{not valid json!!!", encoding="utf-8")
        result = load_status(bad_file)
        assert result is None

    def test_load_empty_file(self, tmp_path: Path) -> None:
        """Loading an empty file returns None (JSONDecodeError)."""
        empty_file = tmp_path / "empty.json"
        empty_file.write_text("", encoding="utf-8")
        result = load_status(empty_file)
        assert result is None

    def test_load_json_array(self, tmp_path: Path) -> None:
        """Loading a valid JSON array (not a dict) returns the parsed value."""
        arr_file = tmp_path / "array.json"
        arr_file.write_text("[1, 2, 3]", encoding="utf-8")
        result = load_status(arr_file)
        # The function does not enforce dict type; it returns whatever json.loads yields.
        assert result == [1, 2, 3]

    def test_load_directory_path(self, tmp_path: Path) -> None:
        """Passing a directory path returns None (OSError)."""
        result = load_status(tmp_path)
        assert result is None

    def test_load_complete_status(self, tmp_path: Path) -> None:
        """Loading a status file with all fields round-trips correctly."""
        full_data = {
            "status": "converged",
            "iteration": 100,
            "gap": 1e-6,
            "lower_bound": 9999.9,
            "upper_bound": 10000.0,
            "elapsed_s": 3600.0,
            "realtime": {
                "timestamps": [0.0, 1.0, 2.0],
                "cpu_loads": [50.0, 75.0, 60.0],
                "active_workers": [4, 4, 3],
            },
            "history": [
                {
                    "iteration": 1,
                    "gap": 0.5,
                    "scene_upper_bounds": [200.0],
                    "scene_lower_bounds": [100.0],
                },
            ],
        }
        fpath = tmp_path / "full_status.json"
        fpath.write_text(json.dumps(full_data), encoding="utf-8")
        result = load_status(fpath)
        assert result is not None
        assert result["realtime"]["cpu_loads"] == [50.0, 75.0, 60.0]
        assert len(result["history"]) == 1


# ---------------------------------------------------------------------------
# Tests for print_status
# ---------------------------------------------------------------------------


class TestPrintStatus:
    """Tests for the print_status helper."""

    def test_print_basic_status(
        self, capsys: pytest.CaptureFixture, sample_status: dict
    ) -> None:
        """print_status outputs iteration, bounds, gap, and status tag."""
        print_status(sample_status)
        captured = capsys.readouterr().out
        assert "iter=   5" in captured
        assert "LB=" in captured
        assert "UB=" in captured
        assert "gap=0.012345" in captured
        assert "[running]" in captured

    def test_print_converged_status(
        self, capsys: pytest.CaptureFixture, converged_status: dict
    ) -> None:
        """print_status displays the converged tag."""
        print_status(converged_status)
        captured = capsys.readouterr().out
        assert "[converged]" in captured
        assert "iter=  20" in captured

    def test_print_missing_fields(self, capsys: pytest.CaptureFixture) -> None:
        """print_status handles missing keys gracefully using defaults."""
        print_status({})
        captured = capsys.readouterr().out
        assert "[unknown]" in captured
        assert "iter=   0" in captured

    def test_print_elapsed_formatting(
        self, capsys: pytest.CaptureFixture, sample_status: dict
    ) -> None:
        """Elapsed time is formatted with one decimal place."""
        print_status(sample_status)
        captured = capsys.readouterr().out
        assert "42.3s" in captured

    def test_print_large_values(self, capsys: pytest.CaptureFixture) -> None:
        """Large bound values are formatted correctly."""
        data = {
            "status": "running",
            "iteration": 999,
            "gap": 0.0,
            "lower_bound": 1234567.8901,
            "upper_bound": 1234567.8901,
            "elapsed_s": 99999.9,
        }
        print_status(data)
        captured = capsys.readouterr().out
        assert "iter= 999" in captured
        assert "1234567.8901" in captured


# ---------------------------------------------------------------------------
# Tests for run_text
# ---------------------------------------------------------------------------


class TestRunText:
    """Tests for the text-mode polling loop."""

    def test_converged_exits_loop(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """run_text exits when the status reports 'converged'."""
        converged = {
            "status": "converged",
            "iteration": 10,
            "gap": 0.0001,
            "lower_bound": 500.0,
            "upper_bound": 500.05,
            "elapsed_s": 30.0,
        }
        fpath = tmp_path / "solver_status.json"
        fpath.write_text(json.dumps(converged), encoding="utf-8")

        with patch("gtopt_monitor.gtopt_monitor.time.sleep"):
            run_text(fpath, poll_interval=0.1)

        captured = capsys.readouterr().out
        assert "Solver converged" in captured

    def test_keyboard_interrupt_stops(
        self, capsys: pytest.CaptureFixture, status_file: Path
    ) -> None:
        """run_text prints 'Monitoring stopped.' on KeyboardInterrupt."""
        call_count = 0

        def mock_sleep(_seconds: float) -> None:
            nonlocal call_count
            call_count += 1
            if call_count >= 2:
                raise KeyboardInterrupt

        with patch("gtopt_monitor.gtopt_monitor.time.sleep", side_effect=mock_sleep):
            run_text(status_file, poll_interval=0.1)

        captured = capsys.readouterr().out
        assert "Monitoring stopped." in captured

    def test_missing_file_keeps_polling(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """run_text keeps polling when the status file does not exist yet."""
        missing = tmp_path / "nonexistent.json"
        call_count = 0

        def mock_sleep(_seconds: float) -> None:
            nonlocal call_count
            call_count += 1
            if call_count >= 3:
                raise KeyboardInterrupt

        with patch("gtopt_monitor.gtopt_monitor.time.sleep", side_effect=mock_sleep):
            run_text(missing, poll_interval=0.1)

        # Should not have printed any status line (only header + interrupt msg)
        captured = capsys.readouterr().out
        assert "Monitoring stopped." in captured
        assert "iter=" not in captured

    def test_prints_header(self, capsys: pytest.CaptureFixture, tmp_path: Path) -> None:
        """run_text prints the monitoring header with file path and poll interval."""
        fpath = tmp_path / "solver_status.json"
        fpath.write_text(
            json.dumps(
                {
                    "status": "converged",
                    "iteration": 1,
                    "gap": 0.0,
                    "lower_bound": 0.0,
                    "upper_bound": 0.0,
                    "elapsed_s": 0.0,
                }
            ),
            encoding="utf-8",
        )
        with patch("gtopt_monitor.gtopt_monitor.time.sleep"):
            run_text(fpath, poll_interval=2.5)

        captured = capsys.readouterr().out
        assert "poll every 2.5s" in captured
        assert "Press Ctrl-C to stop" in captured

    def test_multiple_polls_before_converge(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """run_text polls multiple times before convergence."""
        fpath = tmp_path / "solver_status.json"
        poll_count = 0

        running_data = json.dumps(
            {
                "status": "running",
                "iteration": 1,
                "gap": 0.1,
                "lower_bound": 100.0,
                "upper_bound": 110.0,
                "elapsed_s": 1.0,
            }
        )
        converged_data = json.dumps(
            {
                "status": "converged",
                "iteration": 3,
                "gap": 0.001,
                "lower_bound": 100.0,
                "upper_bound": 100.1,
                "elapsed_s": 3.0,
            }
        )

        # Start with running status
        fpath.write_text(running_data, encoding="utf-8")

        def mock_sleep(_seconds: float) -> None:
            nonlocal poll_count
            poll_count += 1
            if poll_count >= 2:
                # Switch to converged after two polls
                fpath.write_text(converged_data, encoding="utf-8")

        with patch("gtopt_monitor.gtopt_monitor.time.sleep", side_effect=mock_sleep):
            run_text(fpath, poll_interval=0.1)

        captured = capsys.readouterr().out
        assert "Solver converged" in captured
        assert poll_count >= 2


# ---------------------------------------------------------------------------
# Tests for run_gui
# ---------------------------------------------------------------------------


def _make_mock_plt() -> tuple[MagicMock, MagicMock, MagicMock, MagicMock]:
    """Build mock plt, ticker, fig1, fig2 objects for GUI tests.

    Returns (mock_plt, mock_ticker, mock_fig1, mock_fig2).
    """
    mock_plt = MagicMock()
    mock_ticker = MagicMock()
    mock_fig1 = MagicMock()
    mock_fig2 = MagicMock()
    mock_fig1.number = 1
    mock_fig2.number = 2

    mock_line = MagicMock()
    mock_ax = MagicMock()
    mock_ax.plot.return_value = (mock_line,)

    mock_plt.subplots.side_effect = [
        (mock_fig1, (mock_ax, mock_ax)),
        (mock_fig2, (mock_ax, mock_ax, mock_ax)),
    ]
    mock_plt.rcParams = {"axes.prop_cycle": MagicMock()}
    mock_plt.rcParams["axes.prop_cycle"].by_key.return_value = {
        "color": ["tab:blue", "tab:orange", "tab:green"],
    }
    return mock_plt, mock_ticker, mock_fig1, mock_fig2


def _patch_matplotlib(mock_plt: MagicMock, mock_ticker: MagicMock):
    """Context manager that replaces matplotlib modules in sys.modules.

    Saves and restores any previously-loaded real matplotlib modules so
    the local ``import matplotlib.pyplot as plt`` inside run_gui resolves
    to our mock objects.
    """
    # Keys that run_gui imports from
    keys = [k for k in sys.modules if k == "matplotlib" or k.startswith("matplotlib.")]
    saved = {k: sys.modules[k] for k in keys}

    # Remove all real matplotlib modules
    for k in keys:
        del sys.modules[k]

    # Inject mocks — the parent "matplotlib" module's attributes must
    # point to the sub-module mocks so that ``import matplotlib.pyplot``
    # resolves correctly through attribute access on the parent.
    mock_mpl = MagicMock()
    mock_mpl.pyplot = mock_plt
    mock_mpl.ticker = mock_ticker
    sys.modules["matplotlib"] = mock_mpl
    sys.modules["matplotlib.pyplot"] = mock_plt
    sys.modules["matplotlib.ticker"] = mock_ticker

    class _Ctx:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            # Remove injected mocks
            for k in list(sys.modules):
                if k == "matplotlib" or k.startswith("matplotlib."):
                    del sys.modules[k]
            # Restore originals
            sys.modules.update(saved)

    return _Ctx()


class TestRunGui:
    """Tests for the GUI mode (matplotlib mocked)."""

    def test_import_error_exits(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """run_gui prints an error and exits when matplotlib is missing."""
        fpath = tmp_path / "solver_status.json"

        # Save and remove real matplotlib modules
        keys = [
            k for k in sys.modules if k == "matplotlib" or k.startswith("matplotlib.")
        ]
        saved = {k: sys.modules[k] for k in keys}
        for k in keys:
            del sys.modules[k]
        # Inject None to simulate ImportError
        sys.modules["matplotlib"] = None  # type: ignore[assignment]
        sys.modules["matplotlib.pyplot"] = None  # type: ignore[assignment]
        sys.modules["matplotlib.ticker"] = None  # type: ignore[assignment]

        try:
            with pytest.raises(SystemExit) as exc_info:
                run_gui(fpath, poll_interval=1.0)
            assert exc_info.value.code == 1
        finally:
            # Restore real modules
            for k in list(sys.modules):
                if k == "matplotlib" or k.startswith("matplotlib."):
                    del sys.modules[k]
            sys.modules.update(saved)

        captured = capsys.readouterr().err
        assert "matplotlib is not installed" in captured

    def test_gui_dispatches_with_matplotlib(self, tmp_path: Path) -> None:
        """run_gui calls plt.ion, creates figures, and enters the event loop."""
        converged_data = {
            "status": "converged",
            "iteration": 5,
            "gap": 0.001,
            "lower_bound": 100.0,
            "upper_bound": 100.1,
            "elapsed_s": 10.0,
            "realtime": {
                "timestamps": [0.0, 1.0],
                "cpu_loads": [50.0, 60.0],
                "active_workers": [2, 3],
            },
            "history": [
                {
                    "iteration": 1,
                    "gap": 0.5,
                    "scene_upper_bounds": [200.0],
                    "scene_lower_bounds": [100.0],
                },
                {
                    "iteration": 5,
                    "gap": 0.001,
                    "scene_upper_bounds": [100.1],
                    "scene_lower_bounds": [100.0],
                },
            ],
        }
        fpath = tmp_path / "solver_status.json"
        fpath.write_text(json.dumps(converged_data), encoding="utf-8")

        mock_plt, mock_ticker, _mock_fig1, _mock_fig2 = _make_mock_plt()
        # fignum_exists returns True first, then False to break loop
        mock_plt.fignum_exists.side_effect = [True, True, False]

        with _patch_matplotlib(mock_plt, mock_ticker):
            run_gui(fpath, poll_interval=0.01)

        mock_plt.ion.assert_called_once()
        mock_plt.ioff.assert_called_once()

    def test_gui_window_close_exits(self, tmp_path: Path) -> None:
        """run_gui exits when a figure window is closed."""
        fpath = tmp_path / "solver_status.json"
        fpath.write_text(json.dumps({"status": "running"}), encoding="utf-8")

        mock_plt, mock_ticker, _mock_fig1, _mock_fig2 = _make_mock_plt()
        # First window check: fig1 exists but fig2 does not
        mock_plt.fignum_exists.side_effect = [True, False]

        with _patch_matplotlib(mock_plt, mock_ticker):
            run_gui(fpath, poll_interval=0.01)

        mock_plt.ioff.assert_called_once()

    def test_gui_keyboard_interrupt(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """run_gui handles KeyboardInterrupt gracefully."""
        fpath = tmp_path / "solver_status.json"
        fpath.write_text(json.dumps({"status": "running"}), encoding="utf-8")

        mock_plt, mock_ticker, _mock_fig1, _mock_fig2 = _make_mock_plt()
        # fignum_exists always true, but pause raises KeyboardInterrupt
        mock_plt.fignum_exists.return_value = True
        mock_plt.pause.side_effect = KeyboardInterrupt

        with _patch_matplotlib(mock_plt, mock_ticker):
            run_gui(fpath, poll_interval=0.01)

        captured = capsys.readouterr().out
        assert "Monitoring stopped." in captured
        mock_plt.ioff.assert_called_once()

    def test_gui_null_status_keeps_polling(self, tmp_path: Path) -> None:
        """run_gui continues polling when load_status returns None."""
        fpath = tmp_path / "nonexistent_status.json"

        mock_plt, mock_ticker, _mock_fig1, _mock_fig2 = _make_mock_plt()
        # Allow two iterations: first finds no file, second window closes
        mock_plt.fignum_exists.side_effect = [True, True, True, False]
        mock_plt.pause.return_value = None

        with _patch_matplotlib(mock_plt, mock_ticker):
            run_gui(fpath, poll_interval=0.01)

        # pause was called (polling continued despite missing file)
        assert mock_plt.pause.call_count >= 1
        mock_plt.ioff.assert_called_once()


# ---------------------------------------------------------------------------
# Tests for main (CLI entry point)
# ---------------------------------------------------------------------------


class TestMain:
    """Tests for the CLI entry point."""

    def test_default_args_calls_run_gui(self) -> None:
        """With no arguments, main calls run_gui with defaults."""
        with patch("gtopt_monitor.gtopt_monitor.run_gui") as mock_gui, patch(
            "sys.argv", ["gtopt_monitor"]
        ):
            main()
        mock_gui.assert_called_once()
        call_args = mock_gui.call_args
        assert call_args[0][0] == Path("output/solver_status.json")
        assert call_args[0][1] == 1.0

    def test_no_gui_flag_calls_run_text(self) -> None:
        """--no-gui flag dispatches to run_text."""
        with patch("gtopt_monitor.gtopt_monitor.run_text") as mock_text, patch(
            "sys.argv", ["gtopt_monitor", "--no-gui"]
        ):
            main()
        mock_text.assert_called_once()
        call_args = mock_text.call_args
        assert call_args[0][0] == Path("output/solver_status.json")
        assert call_args[0][1] == 1.0

    def test_custom_status_file(self) -> None:
        """--status-file sets the path passed to run_gui."""
        with patch("gtopt_monitor.gtopt_monitor.run_gui") as mock_gui, patch(
            "sys.argv", ["gtopt_monitor", "--status-file", "/tmp/my_status.json"]
        ):
            main()
        call_args = mock_gui.call_args
        assert call_args[0][0] == Path("/tmp/my_status.json")

    def test_custom_poll_interval(self) -> None:
        """--poll sets the polling interval passed to run_text."""
        with patch("gtopt_monitor.gtopt_monitor.run_text") as mock_text, patch(
            "sys.argv", ["gtopt_monitor", "--no-gui", "--poll", "0.5"]
        ):
            main()
        call_args = mock_text.call_args
        assert call_args[0][1] == 0.5

    def test_all_custom_args(self) -> None:
        """All CLI arguments are forwarded correctly."""
        with patch("gtopt_monitor.gtopt_monitor.run_text") as mock_text, patch(
            "sys.argv",
            [
                "gtopt_monitor",
                "--no-gui",
                "--status-file",
                "/custom/path.json",
                "--poll",
                "3.0",
            ],
        ):
            main()
        mock_text.assert_called_once_with(Path("/custom/path.json"), 3.0)

    def test_get_option_prints_value(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """--get prints the requested option value and exits."""
        planning = {
            "options": {
                "method": "sddp",
                "sddp_options": {"max_iterations": 50},
            }
        }
        case_dir = tmp_path / "mycase"
        case_dir.mkdir()
        (case_dir / "mycase.json").write_text(json.dumps(planning), encoding="utf-8")
        main(["--get", "method", "--case-dir", str(case_dir)])
        assert capsys.readouterr().out.strip() == "sddp"

    def test_get_nested_option(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """--get navigates dotted paths into nested dicts."""
        planning = {
            "options": {
                "sddp_options": {"max_iterations": 42},
            }
        }
        case_dir = tmp_path / "nested"
        case_dir.mkdir()
        (case_dir / "nested.json").write_text(json.dumps(planning), encoding="utf-8")
        main(["--get", "sddp_options.max_iterations", "--case-dir", str(case_dir)])
        assert capsys.readouterr().out.strip() == "42"

    def test_get_missing_option_exits(self, tmp_path: Path) -> None:
        """--get exits with error when the option path does not exist."""
        planning = {"options": {"method": "sddp"}}
        case_dir = tmp_path / "miss"
        case_dir.mkdir()
        (case_dir / "miss.json").write_text(json.dumps(planning), encoding="utf-8")
        with pytest.raises(SystemExit) as exc_info:
            main(["--get", "no_such_option", "--case-dir", str(case_dir)])
        assert exc_info.value.code == 1

    def test_get_dict_option_prints_json(
        self, capsys: pytest.CaptureFixture, tmp_path: Path
    ) -> None:
        """--get prints dict values as formatted JSON."""
        planning = {
            "options": {
                "sddp_options": {"max_iterations": 10, "convergence_tol": 0.01},
            }
        }
        case_dir = tmp_path / "dictcase"
        case_dir.mkdir()
        (case_dir / "dictcase.json").write_text(json.dumps(planning), encoding="utf-8")
        main(["--get", "sddp_options", "--case-dir", str(case_dir)])
        output = json.loads(capsys.readouterr().out)
        assert output["max_iterations"] == 10


# ---------------------------------------------------------------------------
# Tests for get_option
# ---------------------------------------------------------------------------


class TestGetOption:
    """Tests for the get_option helper."""

    def test_simple_key(self, tmp_path: Path) -> None:
        planning = {"options": {"method": "sddp"}}
        f = tmp_path / "p.json"
        f.write_text(json.dumps(planning), encoding="utf-8")
        assert get_option(f, "method") == "sddp"

    def test_nested_key(self, tmp_path: Path) -> None:
        planning = {"options": {"sddp_options": {"max_iterations": 5}}}
        f = tmp_path / "p.json"
        f.write_text(json.dumps(planning), encoding="utf-8")
        assert get_option(f, "sddp_options.max_iterations") == 5

    def test_missing_key_raises(self, tmp_path: Path) -> None:
        planning = {"options": {"method": "sddp"}}
        f = tmp_path / "p.json"
        f.write_text(json.dumps(planning), encoding="utf-8")
        with pytest.raises(KeyError, match="no_such"):
            get_option(f, "no_such")

    def test_missing_nested_key_raises(self, tmp_path: Path) -> None:
        planning = {"options": {"sddp_options": {}}}
        f = tmp_path / "p.json"
        f.write_text(json.dumps(planning), encoding="utf-8")
        with pytest.raises(KeyError, match="missing key 'threads'"):
            get_option(f, "sddp_options.threads")

    def test_no_options_key(self, tmp_path: Path) -> None:
        f = tmp_path / "p.json"
        f.write_text("{}", encoding="utf-8")
        with pytest.raises(KeyError):
            get_option(f, "method")


# ---------------------------------------------------------------------------
# Tests for find_planning_json
# ---------------------------------------------------------------------------


class TestFindPlanningJson:
    """Tests for the find_planning_json helper."""

    def test_directory_with_matching_name(self, tmp_path: Path) -> None:
        case_dir = tmp_path / "mycase"
        case_dir.mkdir()
        expected = case_dir / "mycase.json"
        expected.write_text("{}", encoding="utf-8")
        assert find_planning_json(case_dir) == expected

    def test_json_file_directly(self, tmp_path: Path) -> None:
        f = tmp_path / "plan.json"
        f.write_text("{}", encoding="utf-8")
        assert find_planning_json(f) == f

    def test_fallback_to_any_json(self, tmp_path: Path) -> None:
        case_dir = tmp_path / "other"
        case_dir.mkdir()
        f = case_dir / "something.json"
        f.write_text("{}", encoding="utf-8")
        assert find_planning_json(case_dir) == f

    def test_empty_directory_returns_none(self, tmp_path: Path) -> None:
        case_dir = tmp_path / "empty"
        case_dir.mkdir()
        assert find_planning_json(case_dir) is None
