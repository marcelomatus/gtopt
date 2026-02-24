"""Tests for guiservice/gtopt_guisrv.py."""

import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# Ensure the guiservice package root is importable
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

import guiservice.gtopt_guisrv as guisrv


# ---------------------------------------------------------------------------
# get_guiservice_dir()
# ---------------------------------------------------------------------------


def test_get_guiservice_dir_found(tmp_path):
    """Returns the first location where app.py exists."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    # Patch __file__ so script_dir resolves to a parent that has app.py via
    # the "script_dir" candidate (index 1: script_dir itself).
    with patch("guiservice.gtopt_guisrv.__file__", str(tmp_path / "gtopt_guisrv.py")):
        result = guisrv.get_guiservice_dir()

    assert result == tmp_path


def test_get_guiservice_dir_not_found():
    """Calls sys.exit(1) when app.py is not found anywhere."""
    non_existent = Path("/nonexistent_dir_for_test_xyz")
    with patch("guiservice.gtopt_guisrv.__file__", str(non_existent / "gtopt_guisrv.py")):
        with patch("sys.prefix", "/nonexistent_prefix_xyz"):
            with pytest.raises(SystemExit) as exc_info:
                guisrv.get_guiservice_dir()
    assert exc_info.value.code == 1


# ---------------------------------------------------------------------------
# check_dependencies()
# ---------------------------------------------------------------------------


def test_check_dependencies_all_installed():
    """Returns True when all required packages are found."""
    mock_spec = MagicMock()
    with patch("importlib.util.find_spec", return_value=mock_spec):
        assert guisrv.check_dependencies() is True


def test_check_dependencies_one_missing():
    """Returns False when any required package is not found."""

    def _find_spec(name):
        return None if name == "pyarrow" else MagicMock()

    with patch("importlib.util.find_spec", side_effect=_find_spec):
        assert guisrv.check_dependencies() is False


def test_check_dependencies_all_missing():
    """Returns False when no packages are found."""
    with patch("importlib.util.find_spec", return_value=None):
        assert guisrv.check_dependencies() is False


# ---------------------------------------------------------------------------
# show_help()
# ---------------------------------------------------------------------------


def test_show_help(capsys):
    """Prints the module docstring."""
    guisrv.show_help()
    captured = capsys.readouterr()
    assert "gtopt_guisrv" in captured.out


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def test_main_help_exits():
    """--help causes SystemExit(0)."""
    with patch("sys.argv", ["gtopt_guisrv", "--help"]):
        with pytest.raises(SystemExit) as exc_info:
            guisrv.main()
    assert exc_info.value.code == 0


def test_main_default_port_and_host(tmp_path):
    """main() uses default port 5001 and host 0.0.0.0."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    mock_app_module = MagicMock()
    mock_app_module.app = MagicMock()
    mock_app_module.app.run = MagicMock()

    with patch("sys.argv", ["gtopt_guisrv"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=True):
                with patch.dict("sys.modules", {"app": mock_app_module}):
                    guisrv.main()

    mock_app_module.app.run.assert_called_once_with(
        host="0.0.0.0", port=5001, debug=False
    )


def test_main_custom_port(tmp_path):
    """main() accepts --port argument."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    mock_app_module = MagicMock()
    mock_app_module.app = MagicMock()
    mock_app_module.app.run = MagicMock()

    with patch("sys.argv", ["gtopt_guisrv", "--port", "8080"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=True):
                with patch.dict("sys.modules", {"app": mock_app_module}):
                    guisrv.main()

    mock_app_module.app.run.assert_called_once_with(
        host="0.0.0.0", port=8080, debug=False
    )


def test_main_debug_flag(tmp_path):
    """main() passes debug=True when --debug is set."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    mock_app_module = MagicMock()
    mock_app_module.app = MagicMock()
    mock_app_module.app.run = MagicMock()

    with patch("sys.argv", ["gtopt_guisrv", "--debug"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=True):
                with patch.dict("sys.modules", {"app": mock_app_module}):
                    guisrv.main()

    mock_app_module.app.run.assert_called_once_with(
        host="0.0.0.0", port=5001, debug=True
    )


def test_main_missing_dependencies_exits(tmp_path, capsys):
    """main() exits with code 1 when dependencies are missing."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    with patch("sys.argv", ["gtopt_guisrv"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=False):
                with pytest.raises(SystemExit) as exc_info:
                    guisrv.main()

    assert exc_info.value.code == 1


def test_main_keyboard_interrupt_exits(tmp_path):
    """main() exits cleanly on KeyboardInterrupt."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    mock_app_module = MagicMock()
    mock_app_module.app = MagicMock()
    mock_app_module.app.run = MagicMock(side_effect=KeyboardInterrupt)

    with patch("sys.argv", ["gtopt_guisrv"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=True):
                with patch.dict("sys.modules", {"app": mock_app_module}):
                    with pytest.raises(SystemExit) as exc_info:
                        guisrv.main()

    assert exc_info.value.code == 0


def test_main_exception_exits(tmp_path, capsys):
    """main() exits with code 1 on unexpected exception."""
    app_py = tmp_path / "app.py"
    app_py.touch()

    mock_app_module = MagicMock()
    mock_app_module.app = MagicMock()
    mock_app_module.app.run = MagicMock(side_effect=RuntimeError("boom"))

    with patch("sys.argv", ["gtopt_guisrv"]):
        with patch("guiservice.gtopt_guisrv.get_guiservice_dir", return_value=tmp_path):
            with patch("guiservice.gtopt_guisrv.check_dependencies", return_value=True):
                with patch.dict("sys.modules", {"app": mock_app_module}):
                    with pytest.raises(SystemExit) as exc_info:
                        guisrv.main()

    assert exc_info.value.code == 1
    captured = capsys.readouterr()
    assert "boom" in captured.err
