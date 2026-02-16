"""Tests for gtopt_gui launcher with integrated webservice functionality."""

import os
import signal
import socket
import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# Import functions from gtopt_gui
# We need to handle the import carefully since it's a script
sys.path.insert(0, str(Path(__file__).parent.parent))
from gtopt_gui import (
    check_webservice_api,
    find_free_port,
    get_guiservice_dir,
    is_gtopt_webservice,
    is_port_open,
    open_browser,
    query_webservice_ping,
    resolve_python_executable,
    wait_for_webservice,
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


def test_query_webservice_ping_returns_none_on_failure():
    """query_webservice_ping should return None when webservice is unreachable."""
    result = query_webservice_ping("http://127.0.0.1:19999", timeout=1)
    assert result is None


def test_start_webservice_uses_new_session():
    """start_webservice should create the subprocess with start_new_session=True."""
    from gtopt_gui import start_webservice

    with patch("subprocess.Popen") as mock_popen:
        mock_proc = MagicMock()
        mock_proc.poll.return_value = None
        mock_popen.return_value = mock_proc

        webservice_dir = Path("/fake/webservice")
        # Create a fake .next directory so the function doesn't bail out
        with patch.object(Path, "exists", return_value=True):
            start_webservice(webservice_dir, 3000)

        # Verify start_new_session was passed
        call_kwargs = mock_popen.call_args[1]
        assert call_kwargs.get("start_new_session") is True


def test_query_webservice_ping_returns_data_on_success():
    """query_webservice_ping should return parsed JSON on success."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    ping_data = {"status": "ok", "service": "gtopt-webservice"}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(ping_data).encode())

        def log_message(self, format, *args):
            pass  # suppress logging

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    result = query_webservice_ping(f"http://127.0.0.1:{port}", timeout=5)
    assert result is not None
    assert result["status"] == "ok"
    assert result["service"] == "gtopt-webservice"

    server.server_close()


def test_is_gtopt_webservice_returns_true_for_valid_service():
    """is_gtopt_webservice should return True for a valid gtopt webservice."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"status": "ok", "service": "gtopt-webservice"}).encode())

        def log_message(self, format, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    assert is_gtopt_webservice(f"http://127.0.0.1:{port}", timeout=5) is True
    server.server_close()


def test_is_gtopt_webservice_returns_false_for_wrong_service():
    """is_gtopt_webservice should return False for a non-gtopt service."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(404)
            self.end_headers()

        def log_message(self, format, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    assert is_gtopt_webservice(f"http://127.0.0.1:{port}", timeout=5) is False
    server.server_close()


def test_is_gtopt_webservice_returns_false_on_connection_failure():
    """is_gtopt_webservice should return False when the service is unreachable."""
    assert is_gtopt_webservice("http://127.0.0.1:19999", timeout=1) is False


def test_wait_for_webservice_returns_true_when_service_is_ready():
    """wait_for_webservice should return True when service responds immediately."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"status": "ok", "service": "gtopt-webservice"}).encode())

        def log_message(self, format, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    assert wait_for_webservice(f"http://127.0.0.1:{port}", timeout=5) is True
    server.shutdown()
    server.server_close()


def test_wait_for_webservice_returns_false_on_timeout():
    """wait_for_webservice should return False when service never becomes ready."""
    assert wait_for_webservice("http://127.0.0.1:19999", timeout=2) is False


def test_check_webservice_api_returns_true_on_success():
    """check_webservice_api should return True when /api responds with status ok."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"status": "ok", "service": "gtopt-webservice"}).encode())

        def log_message(self, format, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    assert check_webservice_api(f"http://127.0.0.1:{port}", timeout=5) is True
    server.server_close()


def test_check_webservice_api_returns_false_on_failure():
    """check_webservice_api should return False when the service is unreachable."""
    assert check_webservice_api("http://127.0.0.1:19999", timeout=1) is False


def test_check_webservice_api_returns_false_on_bad_status():
    """check_webservice_api should return False when status is not ok."""
    import json
    from http.server import HTTPServer, BaseHTTPRequestHandler
    import threading

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"status": "error"}).encode())

        def log_message(self, format, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    assert check_webservice_api(f"http://127.0.0.1:{port}", timeout=5) is False
    server.server_close()
