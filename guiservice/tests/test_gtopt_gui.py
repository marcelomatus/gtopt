"""Tests for gtopt_gui launcher with integrated webservice functionality."""

import socket
import os
import subprocess
import sys
from pathlib import Path

import pytest

# Import functions from gtopt_gui
# We need to handle the import carefully since it's a script
sys.path.insert(0, str(Path(__file__).parent.parent))
from gtopt_gui import (
    find_free_port,
    get_guiservice_dir,
    is_port_open,
    open_browser,
    resolve_python_executable,
)


def test_find_free_port():
    """Test that find_free_port returns a valid port number."""
    port = find_free_port()
    
    # Check it's a valid port number
    assert isinstance(port, int)
    assert 1024 <= port <= 65535
    
    # Verify the port is actually available
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", port))
        s.listen(1)
        # If we got here, port is available


def test_get_guiservice_dir():
    """Test that get_guiservice_dir finds the guiservice directory."""
    guiservice_dir = get_guiservice_dir()
    
    # Should return a Path object
    assert isinstance(guiservice_dir, Path)
    
    # Should contain app.py
    assert (guiservice_dir / "app.py").exists()
    
    # Should contain gtopt_gui.py
    assert (guiservice_dir / "gtopt_gui.py").exists()


def test_webservice_detection():
    """Test that webservice detection works without errors."""
    from gtopt_gui import get_webservice_dir
    
    # Should return None or Path, not raise error
    result = get_webservice_dir()
    assert result is None or isinstance(result, Path)
    
    # If found, should have package.json
    if result is not None:
        assert (result / "package.json").exists()


def test_gtopt_binary_detection():
    """Test that gtopt binary detection works without errors."""
    from gtopt_gui import find_gtopt_binary
    
    # Should return None or Path, not raise error
    result = find_gtopt_binary()
    assert result is None or isinstance(result, Path)


def test_open_browser_regular_mode_uses_webbrowser(monkeypatch):
    """Regular mode should always use default browser opener."""
    calls = []

    def fake_open(url, new=1):
        calls.append((url, new))
        return True

    monkeypatch.setattr("webbrowser.open", fake_open)
    open_browser("http://localhost:5001", app_mode=False)

    assert calls == [("http://localhost:5001", 1)]


def test_open_browser_app_mode_falls_back_to_webbrowser(monkeypatch):
    """App mode should fall back gracefully when app launchers are unavailable."""
    calls = []

    def fake_open(url, new=1):
        calls.append((url, new))
        return True

    def raise_file_not_found(*args, **kwargs):
        raise FileNotFoundError()

    monkeypatch.setattr("webbrowser.open", fake_open)
    monkeypatch.setattr("sys.platform", "linux")
    monkeypatch.setattr("subprocess.Popen", raise_file_not_found)

    open_browser("http://localhost:5001", app_mode=True)

    assert calls == [("http://localhost:5001", 1)]


def test_is_port_open_detects_listening_socket():
    """is_port_open should return True only while a socket is listening."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        assert is_port_open(port)

    assert not is_port_open(port)


def test_resolve_python_executable_prefers_cli_then_env(monkeypatch):
    """CLI override should win over environment variable."""
    monkeypatch.setenv("GTOPT_GUI_PYTHON", "/env/python")
    python_exe, source = resolve_python_executable("/cli/python")
    assert python_exe == "/cli/python"
    assert source == "cli"


def test_resolve_python_executable_uses_env(monkeypatch):
    """Environment override should be used when CLI is not provided."""
    monkeypatch.setenv("GTOPT_GUI_PYTHON", "/env/python")
    python_exe, source = resolve_python_executable()
    assert python_exe == "/env/python"
    assert source == "env"


def test_launcher_uses_temp_copy_for_non_writable_guiservice_dir(tmp_path):
    """Shell launcher should copy guiservice files to a writable temp dir when needed."""
    launcher_src = Path(__file__).parent.parent / "gtopt_gui.sh"
    install_bin = tmp_path / "install" / "bin"
    guiservice_dir = tmp_path / "install" / "share" / "gtopt" / "guiservice"
    install_bin.mkdir(parents=True)
    guiservice_dir.mkdir(parents=True)

    launcher = install_bin / "gtopt_gui.sh"
    launcher.write_text(launcher_src.read_text())
    launcher.chmod(0o755)

    (guiservice_dir / "gtopt_gui.py").write_text("print('ok')\n")
    guiservice_dir.chmod(0o555)

    fake_bin = tmp_path / "fakebin"
    fake_bin.mkdir()
    fake_python = fake_bin / "python3"
    fake_python.write_text("#!/bin/sh\nexit 0\n")
    fake_python.chmod(0o755)

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}:/usr/bin:/bin"
    env["GTOPT_GUI_DEBUG"] = "1"

    try:
        result = subprocess.run(
            [str(launcher)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    finally:
        guiservice_dir.chmod(0o755)

    assert result.returncode == 0
    marker = "Using temporary GUISERVICE_DIR="
    assert marker in result.stderr
    temp_dir = result.stderr.split(marker, 1)[1].splitlines()[0].strip()
    assert temp_dir.startswith("/tmp/")
    assert temp_dir != str(guiservice_dir)
