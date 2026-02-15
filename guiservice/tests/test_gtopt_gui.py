"""Tests for gtopt_gui launcher with integrated webservice functionality."""

import socket
import sys
from pathlib import Path

import pytest

# Import functions from gtopt_gui
# We need to handle the import carefully since it's a script
sys.path.insert(0, str(Path(__file__).parent.parent))
from gtopt_gui import (
    find_free_port,
    get_guiservice_dir,
    open_browser,
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
