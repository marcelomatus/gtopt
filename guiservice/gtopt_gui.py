#!/usr/bin/env python3
"""
gtopt_gui - Standalone launcher for the gtopt GUI service.

This script starts the Flask guiservice and opens a web browser to interact
with gtopt configuration files in a kiosk-like experience.

Usage:
    gtopt_gui [config_file.json]

Example:
    gtopt_gui system_c0.json
"""

import argparse
import atexit
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import webbrowser
from pathlib import Path


def find_free_port():
    """Find an available TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
    return port


def get_guiservice_dir():
    """Get the directory containing the guiservice app.py."""
    # This script is in the guiservice directory
    script_dir = Path(__file__).parent.resolve()
    app_py = script_dir / "app.py"
    if app_py.exists():
        return script_dir
    
    # If installed, look for app.py in known locations
    possible_locations = [
        Path(__file__).parent,
        Path(sys.prefix) / "share" / "gtopt" / "guiservice",
        Path("/usr/local/share/gtopt/guiservice"),
        Path("/opt/gtopt/guiservice"),
    ]
    
    for location in possible_locations:
        app_py = location / "app.py"
        if app_py.exists():
            return location
    
    print("Error: Cannot find guiservice app.py", file=sys.stderr)
    sys.exit(1)


def get_webservice_dir():
    """Get the directory containing the webservice installation.
    
    Returns:
        Path to webservice directory, or None if not found
    """
    # Check if gtopt_websrv launcher exists (indicates webservice is installed)
    # When installed, guiservice is in share/gtopt/guiservice and bin is at ../../bin
    guiservice_dir = Path(__file__).resolve().parent
    install_prefix = guiservice_dir.parent.parent.parent  # go up from share/gtopt/guiservice
    
    possible_bin_locations = [
        install_prefix / "bin",  # Same install prefix
        Path(sys.prefix) / "bin",
        Path("/usr/local/bin"),
        Path("/usr/bin"),
        Path.home() / ".local" / "bin",
    ]
    
    for bin_dir in possible_bin_locations:
        websrv_script = bin_dir / "gtopt_websrv"
        if websrv_script.exists():
            # Found launcher, now find webservice directory
            possible_web_locations = [
                bin_dir.parent / "share" / "gtopt" / "webservice",
                Path("/usr/local/share/gtopt/webservice"),
                Path("/usr/share/gtopt/webservice"),
            ]
            for web_dir in possible_web_locations:
                if (web_dir / "package.json").exists():
                    return web_dir
    
    return None


def find_gtopt_binary():
    """Find the gtopt binary in common locations.
    
    Returns:
        Path to gtopt binary, or None if not found
    """
    # Try PATH first
    import shutil
    gtopt_path = shutil.which("gtopt")
    if gtopt_path:
        return Path(gtopt_path)
    
    # Try common installation locations
    # When installed, guiservice is in share/gtopt/guiservice and bin is at ../../bin
    guiservice_dir = Path(__file__).resolve().parent
    install_prefix = guiservice_dir.parent.parent.parent  # go up from share/gtopt/guiservice
    
    possible_locations = [
        install_prefix / "bin" / "gtopt",  # Same install prefix
        Path(sys.prefix) / "bin" / "gtopt",
        Path("/usr/local/bin/gtopt"),
        Path("/usr/bin/gtopt"),
        Path.home() / ".local" / "bin" / "gtopt",
    ]
    
    for location in possible_locations:
        if location.exists() and location.is_file():
            return location
    
    return None


def wait_for_service(port, timeout=30):
    """Wait for the Flask service to be ready."""
    import urllib.request
    import urllib.error
    
    url = f"http://localhost:{port}/api/schemas"
    start_time = time.time()
    
    while time.time() - start_time < timeout:
        try:
            with urllib.request.urlopen(url, timeout=1) as response:
                if response.status == 200:
                    return True
        except (urllib.error.URLError, ConnectionRefusedError, OSError):
            time.sleep(0.5)
    
    return False


def is_port_open(port, host="127.0.0.1", timeout=0.5):
    """Check whether a TCP port is accepting connections."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        return sock.connect_ex((host, port)) == 0


def query_webservice_ping(webservice_url, timeout=5):
    """Query the webservice /api/ping endpoint for version and status info.

    Returns:
        dict with ping response, or None on failure.
    """
    import urllib.request
    import urllib.error

    url = f"{webservice_url}/api/ping"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            if response.status == 200:
                return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, ConnectionRefusedError, OSError, json.JSONDecodeError):
        pass
    return None


def is_gtopt_webservice(webservice_url, timeout=3):
    """Check whether the given URL hosts a gtopt webservice.

    Sends a ping and verifies the response contains the expected
    ``service`` field so that unrelated services on the same port are
    not mistaken for the gtopt webservice.

    Returns:
        True if the service identifies itself as ``gtopt-webservice``.
    """
    info = query_webservice_ping(webservice_url, timeout=timeout)
    return info is not None and info.get("service") == "gtopt-webservice"


def wait_for_webservice(webservice_url, timeout=30):
    """Wait until the webservice responds to a ping request.

    Polls the ``/api/ping`` endpoint every second until a valid response
    is received or *timeout* seconds elapse.

    Returns:
        True if the webservice became ready within the timeout.
    """
    start_time = time.time()
    while time.time() - start_time < timeout:
        if is_gtopt_webservice(webservice_url, timeout=2):
            return True
        time.sleep(1)
    return False


def resolve_python_executable(cli_python=None):
    """Resolve Python executable used to launch the Flask guiservice process."""
    env_python = os.environ.get("GTOPT_GUI_PYTHON")
    if cli_python:
        return cli_python, "cli"
    if env_python:
        return env_python, "env"
    return sys.executable, "current"


def open_browser(url, app_mode=False):
    """Open a web browser with the specified URL.
    
    Args:
        url: The URL to open
        app_mode: If True, try to open in app/kiosk mode (platform-dependent)
    """
    if app_mode:
        # Try platform-specific app modes
        if sys.platform == "darwin":  # macOS
            # Use open with --new --app for kiosk-like experience
            try:
                subprocess.Popen([
                    "open", "-n", "-a", "Google Chrome",
                    "--args", f"--app={url}", "--disable-extensions"
                ])
                return
            except FileNotFoundError:
                pass  # Fall back to default browser
        
        elif sys.platform.startswith("linux"):
            # Try Chrome/Chromium app mode on Linux
            for browser_cmd in ["google-chrome", "chromium-browser", "chromium"]:
                try:
                    subprocess.Popen([
                        browser_cmd, f"--app={url}",
                        "--disable-extensions",
                        "--no-first-run"
                    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    return
                except FileNotFoundError:
                    continue
    
    # Fallback to default browser
    webbrowser.open(url, new=1)


def start_webservice(webservice_dir, port, gtopt_bin=None, data_dir=None, log_file=None):
    """Start the webservice in a subprocess.
    
    Args:
        webservice_dir: Path to webservice installation directory
        port: Port to run webservice on
        gtopt_bin: Path to gtopt binary (optional)
        data_dir: Path to data directory for job storage (optional)
    
    Returns:
        subprocess.Popen object for the webservice process
    """
    env = os.environ.copy()
    env["PORT"] = str(port)
    
    if gtopt_bin:
        env["GTOPT_BIN"] = str(gtopt_bin)
    
    if data_dir:
        env["GTOPT_DATA_DIR"] = str(data_dir)
    
    if log_file:
        env["GTOPT_LOG_DIR"] = str(Path(log_file).parent)
    
    # Use Node.js to start the webservice
    # The webservice should already be built (npm run build was executed during install)
    node_exe = "node"
    
    # Check if .next directory exists (production build)
    next_dir = webservice_dir / ".next"
    if not next_dir.exists():
        print(f"Warning: Webservice not built. Run 'npm run build' in {webservice_dir}", file=sys.stderr)
        return None
    
    # Start with 'npm start' which runs the production server
    log_handle = None
    try:
        if log_file:
            log_handle = open(log_file, "a")
        stdout_target = log_handle if log_handle else subprocess.PIPE
        process = subprocess.Popen(
            ["npm", "start"],
            cwd=str(webservice_dir),
            env=env,
            stdout=stdout_target,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        if log_handle:
            process._gtopt_log_handle = log_handle
        return process
    except FileNotFoundError:
        print("Error: npm not found. Please install Node.js and npm.", file=sys.stderr)
        if log_handle:
            log_handle.close()
        return None


def create_gui_url(port):
    """Create the URL for the GUI interface.
    
    Args:
        port: The port number where the Flask service is running
    
    Returns:
        The base URL for the GUI interface (e.g., http://localhost:5001/)
    """
    return f"http://localhost:{port}/"


def main():
    """Main entry point for gtopt_gui."""
    parser = argparse.ArgumentParser(
        description="Launch the gtopt GUI service and open a web browser.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  gtopt_gui                    # Start GUI without a specific config file
  gtopt_gui system_c0.json     # Start GUI (config file can be uploaded via UI)
  gtopt_gui --port 5001        # Use a specific port
  gtopt_gui --app-mode         # Open in Chrome/Chromium app window mode
  gtopt_gui --no-webservice    # Don't auto-start webservice

The GUI will open in your default web browser. To upload your configuration
file, use the "Upload Case" button in the interface.

By default, the browser opens in a regular window for best compatibility.
Use --app-mode to request Chrome/Chromium app/kiosk mode.

The GUI automatically starts a local webservice instance for running
optimizations. Use --no-webservice to disable auto-start if you want to use
an external webservice.
        """,
    )
    
    parser.add_argument(
        "config_file",
        nargs="?",
        help="Path to gtopt configuration JSON file (optional)",
    )
    
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        help="Port for the GUI service (default: auto-select)",
    )
    
    parser.add_argument(
        "--webservice-port",
        type=int,
        default=3000,
        help="Port for the webservice (default: 3000)",
    )
    
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Don't open a web browser automatically",
    )
    
    parser.add_argument(
        "--no-app-mode",
        action="store_true",
        help="Deprecated alias for regular browser mode",
    )

    parser.add_argument(
        "--app-mode",
        action="store_true",
        help="Try to open browser in app/kiosk mode (Chrome/Chromium only)",
    )
    
    parser.add_argument(
        "--no-webservice",
        action="store_true",
        help="Don't auto-start the webservice",
    )
    
    parser.add_argument(
        "--webservice-url",
        type=str,
        default=None,
        help="Use external webservice at this URL (implies --no-webservice)",
    )
    
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Run Flask in debug mode with auto-reload",
    )

    parser.add_argument(
        "--python",
        type=str,
        default=None,
        help="Python executable for launching guiservice app.py (default: current interpreter or GTOPT_GUI_PYTHON)",
    )
    
    args = parser.parse_args()
    
    # Validate config file if provided
    config_path = None
    if args.config_file:
        config_path = Path(args.config_file).resolve()
        if not config_path.exists():
            print(f"Error: Configuration file not found: {config_path}", file=sys.stderr)
            sys.exit(1)
        
        # Try to validate it's a JSON file
        try:
            with open(config_path) as f:
                json.load(f)
        except json.JSONDecodeError as e:
            print(f"Warning: Configuration file may not be valid JSON: {e}", file=sys.stderr)
    
    # Find guiservice directory
    guiservice_dir = get_guiservice_dir()
    print(f"Using guiservice from: {guiservice_dir}")
    
    # Start webservice if requested
    webservice_process = None
    webservice_url = args.webservice_url
    
    log_dir = tempfile.mkdtemp(prefix="gtopt_gui_logs_")
    guiservice_log_file = Path(log_dir) / "guiservice.log"
    webservice_log_file = Path(log_dir) / "webservice.log"
    print(f"Logs directory: {log_dir}")

    if not args.no_webservice and not webservice_url:
        if is_port_open(args.webservice_port):
            candidate_url = f"http://localhost:{args.webservice_port}"
            if is_gtopt_webservice(candidate_url):
                webservice_url = candidate_url
                print(
                    f"Detected existing gtopt webservice on port {args.webservice_port}; reusing at {webservice_url}"
                )
            else:
                print(
                    f"Port {args.webservice_port} is in use but does not appear to be a gtopt webservice."
                )
                print("Use --webservice-port to choose a different port.")
        else:
            # Try to start webservice automatically
            webservice_dir = get_webservice_dir()
            
            if webservice_dir:
                print(f"Found webservice at: {webservice_dir}")
                
                # Find gtopt binary
                gtopt_bin = find_gtopt_binary()
                if gtopt_bin:
                    print(f"Found gtopt binary at: {gtopt_bin}")
                else:
                    print("Warning: gtopt binary not found. Webservice will start but solving may fail.")
                    print("Install gtopt or set GTOPT_BIN environment variable.")
                
                # Create temporary data directory for webservice jobs
                data_dir = tempfile.mkdtemp(prefix="gtopt_gui_jobs_")
                
                # Start webservice
                print(f"Starting webservice on port {args.webservice_port}...")
                webservice_process = start_webservice(
                    webservice_dir,
                    args.webservice_port,
                    gtopt_bin=gtopt_bin,
                    data_dir=data_dir,
                    log_file=str(webservice_log_file),
                )
                
                if webservice_process:
                    webservice_url = f"http://localhost:{args.webservice_port}"
                    print(f"Webservice starting at: {webservice_url}")
                    # Wait for the webservice to become ready
                    print("Waiting for webservice to be ready...")
                    if wait_for_webservice(webservice_url, timeout=30):
                        print("Webservice is ready.")
                    else:
                        print("Warning: Webservice did not become ready within 30 seconds.", file=sys.stderr)
                    # If our child exited but the port is open, another instance is already serving.
                    if webservice_process.poll() is not None and is_port_open(args.webservice_port):
                        print(
                            f"Detected existing process on port {args.webservice_port}; using {webservice_url}"
                        )
                        webservice_process = None
                else:
                    print("Warning: Failed to start webservice. Solve functionality will not be available.")
            else:
                print("Warning: Webservice not found. Solve functionality will not be available.")
                print("Install webservice with: cmake -S webservice -B build-web && sudo cmake --install build-web")
    
    # Determine port for GUI
    port = args.port if args.port else find_free_port()
    
    # Start Flask server
    print(f"Starting gtopt GUI service on port {port}...")
    
    env = os.environ.copy()
    env["FLASK_DEBUG"] = "1" if args.debug else "0"
    env["GTOPT_GUI_PORT"] = str(port)
    env["GTOPT_GUI_LOG_FILE"] = str(guiservice_log_file)
    
    # Set webservice URL if we have one
    if webservice_url:
        env["GTOPT_WEBSERVICE_URL"] = webservice_url
        print(f"Configured to use webservice at: {webservice_url}")
    
    # Use configured python executable for the Flask process
    python_exe, python_source = resolve_python_executable(args.python)
    print(f"Using Python executable ({python_source}): {python_exe}")
    
    flask_log_handle = open(guiservice_log_file, "a")
    flask_process = subprocess.Popen(
        [python_exe, "-u", "app.py"],
        cwd=str(guiservice_dir),
        env=env,
        stdout=flask_log_handle,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    flask_process._gtopt_log_handle = flask_log_handle
    
    # Register cleanup handler
    def _kill_process_group(proc, label):
        """Terminate a process and its entire process group."""
        if proc.poll() is not None:
            return
        print(f"Shutting down {label}...")
        try:
            pgid = os.getpgid(proc.pid)
            os.killpg(pgid, signal.SIGTERM)
        except (OSError, ProcessLookupError):
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                pgid = os.getpgid(proc.pid)
                os.killpg(pgid, signal.SIGKILL)
            except (OSError, ProcessLookupError):
                proc.kill()
            proc.wait(timeout=3)

    def cleanup():
        """Terminate Flask server and webservice on exit."""
        _kill_process_group(flask_process, "gtopt GUI service")
        if webservice_process:
            _kill_process_group(webservice_process, "webservice")
        if hasattr(flask_process, "_gtopt_log_handle"):
            flask_process._gtopt_log_handle.close()
        if webservice_process and hasattr(webservice_process, "_gtopt_log_handle"):
            webservice_process._gtopt_log_handle.close()
    
    atexit.register(cleanup)
    
    # Handle Ctrl+C gracefully
    def signal_handler(sig, frame):
        print("\nReceived interrupt signal, shutting down...")
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # Wait for GUI service to be ready
    print("Waiting for GUI service to start...")
    if not wait_for_service(port):
        print("Error: GUI service failed to start within timeout", file=sys.stderr)
        cleanup()
        sys.exit(1)
    
    print("GUI service is ready!")
    
    # Create URL
    url = create_gui_url(port)
    
    # Open browser
    if not args.no_browser:
        print(f"Opening browser at {url}")
        if config_path:
            print(f"\nTo work with your configuration file ({config_path.name}):")
            print("  1. Click the 'Upload Case' button")
            print(f"  2. Navigate to and select: {config_path}")
            print("  3. Or create a ZIP file containing your config and data files")
        # Use regular browser mode by default for better compatibility.
        app_mode = args.app_mode and not args.no_app_mode
        open_browser(url, app_mode=app_mode)
    else:
        print(f"\nGUI service is running at: {url}")
    
    # Show status summary
    print("\n" + "=" * 60)
    print("gtopt GUI is running. Press Ctrl+C to stop.")
    print(f"GUI service logs: {guiservice_log_file}")
    if webservice_url:
        print(f"Webservice available at: {webservice_url}")
        print(f"Webservice logs: {webservice_log_file}")
        # Ping webservice for version info
        ping_info = query_webservice_ping(webservice_url)
        if ping_info:
            gtopt_version = ping_info.get("gtopt_version", "")
            gtopt_bin = ping_info.get("gtopt_bin", "")
            ws_log_file = ping_info.get("log_file", "")
            if gtopt_version:
                print(f"gtopt version: {gtopt_version}")
            if gtopt_bin:
                print(f"gtopt binary: {gtopt_bin}")
            if ws_log_file:
                print(f"Webservice log file: {ws_log_file}")
        print("You can now edit cases, submit them for solving, and view results.")
    else:
        print("Note: Webservice not available. Solve functionality disabled.")
    print("=" * 60 + "\n")
    
    # Stream logs from Flask
    try:
        while True:
            if flask_process.poll() is not None:
                print("\nFlask process terminated unexpectedly", file=sys.stderr)
                print(f"Check logs: {guiservice_log_file}", file=sys.stderr)
                sys.exit(1)
            
            # Also check webservice if it's running
            if webservice_process and webservice_process.poll() is not None:
                print("\nWebservice terminated unexpectedly", file=sys.stderr)
                print(f"Check logs: {webservice_log_file}", file=sys.stderr)
                # Continue running GUI even if webservice dies
            time.sleep(0.1)
    
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
