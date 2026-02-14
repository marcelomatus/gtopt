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
        s.bind(("", 0))
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

The GUI will open in your default web browser. To upload your configuration
file, use the "Upload Case" button in the interface.
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
        help="Port for the web service (default: auto-select)",
    )
    
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Don't open a web browser automatically",
    )
    
    parser.add_argument(
        "--app-mode",
        action="store_true",
        default=True,
        help="Try to open browser in app/kiosk mode (default: True)",
    )
    
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Run Flask in debug mode with auto-reload",
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
    
    # Determine port
    port = args.port if args.port else find_free_port()
    
    # Start Flask server
    print(f"Starting gtopt GUI service on port {port}...")
    
    env = os.environ.copy()
    env["FLASK_DEBUG"] = "1" if args.debug else "0"
    env["GTOPT_GUI_PORT"] = str(port)
    
    # Use python from current environment
    python_exe = sys.executable
    
    flask_process = subprocess.Popen(
        [python_exe, "-u", "app.py"],
        cwd=str(guiservice_dir),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    
    # Register cleanup handler
    def cleanup():
        """Terminate Flask server on exit."""
        if flask_process.poll() is None:
            print("\nShutting down gtopt GUI service...")
            flask_process.terminate()
            try:
                flask_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                flask_process.kill()
    
    atexit.register(cleanup)
    
    # Handle Ctrl+C gracefully
    def signal_handler(sig, frame):
        print("\nReceived interrupt signal, shutting down...")
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # Wait for service to be ready
    print("Waiting for service to start...")
    if not wait_for_service(port):
        print("Error: Service failed to start within timeout", file=sys.stderr)
        cleanup()
        sys.exit(1)
    
    print("Service is ready!")
    
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
        open_browser(url, app_mode=args.app_mode)
    else:
        print(f"\nGUI service is running at: {url}")
    
    # Keep running and show logs
    print("\n" + "=" * 60)
    print("gtopt GUI is running. Press Ctrl+C to stop.")
    print("=" * 60 + "\n")
    
    # Stream logs from Flask
    try:
        while True:
            if flask_process.poll() is not None:
                print("\nFlask process terminated unexpectedly", file=sys.stderr)
                # Print any error output
                stderr_output = flask_process.stderr.read()
                if stderr_output:
                    print("Error output:", file=sys.stderr)
                    print(stderr_output, file=sys.stderr)
                sys.exit(1)
            
            # Read and display logs
            line = flask_process.stdout.readline()
            if line:
                print(line.rstrip())
            else:
                time.sleep(0.1)
    
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
