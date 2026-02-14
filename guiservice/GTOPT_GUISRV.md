# gtopt_guisrv - GUI Service Web Server Launcher

A command-line launcher for the gtopt GUI service that runs it as a web server
(without automatically opening a browser), similar to how `gtopt_websrv` runs
the webservice.

## Difference from gtopt_gui

- **gtopt_gui**: Interactive launcher that opens a browser in kiosk/app mode for a standalone GUI experience
- **gtopt_guisrv**: Web server launcher that runs the Flask app as a daemon/service without opening a browser

## Installation

The `gtopt_guisrv` command is installed when you install the guiservice:

```bash
# Install guiservice
cmake -S guiservice -B build-gui
sudo cmake --install build-gui

# Install Python dependencies
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

After installation:
- `gtopt_gui` launcher at `/usr/local/bin/gtopt_gui` (interactive mode)
- `gtopt_guisrv` launcher at `/usr/local/bin/gtopt_guisrv` (web server mode)
- GUI service files at `/usr/local/share/gtopt/guiservice`

## Requirements

- Python 3.10 or later
- Python packages: Flask, pandas, pyarrow, requests (see `requirements.txt`)

## Usage

### Basic usage

Launch the GUI service as a web server on default port 5001:

```bash
gtopt_guisrv
```

The service will be available at `http://localhost:5001`.

### With options

```bash
# Use custom port
gtopt_guisrv --port 8080

# Bind to specific host
gtopt_guisrv --host 127.0.0.1

# Run in debug mode (with auto-reload)
gtopt_guisrv --debug
```

### Environment Variables

You can also use environment variables:

```bash
# Set port
GTOPT_GUI_PORT=8080 gtopt_guisrv

# Enable debug mode
FLASK_DEBUG=1 gtopt_guisrv
```

## Options

```
gtopt_guisrv [options]

Options:
  --port PORT          Port for the GUI service (default: 5001)
  --host HOST          Host to bind to (default: 0.0.0.0)
  --debug              Run Flask in debug mode with auto-reload
  --help               Show help message
```

## Systemd Service

A systemd service file template is included for running the GUI service as a
system daemon.

### Installation

```bash
# Copy the service file to systemd
sudo cp /usr/local/share/gtopt/guiservice/gtopt-guiservice.service \
        /etc/systemd/system/

# Edit the service file if needed (e.g., change port, user, paths)
sudo nano /etc/systemd/system/gtopt-guiservice.service

# Reload systemd
sudo systemctl daemon-reload

# Enable the service to start on boot
sudo systemctl enable gtopt-guiservice

# Start the service
sudo systemctl start gtopt-guiservice

# Check status
sudo systemctl status gtopt-guiservice
```

### Service Configuration

The default service file uses these settings:
- Port: 5001
- User: gtopt (create this user first)
- Working directory: /usr/local/share/gtopt/guiservice

Edit `/etc/systemd/system/gtopt-guiservice.service` to customize these settings.

## Features

The GUI service provides:
- **Case Editor**: Create and edit gtopt cases through a web interface
- **File Upload**: Upload existing cases as ZIP files
- **Data Management**: Edit system elements, simulation parameters, and options
- **Visualization**: View input data and configuration
- **Results Viewing**: Upload and visualize optimization results
- **Webservice Integration**: Connect to remote gtopt webservice for solving

## Comparison with gtopt_gui

| Feature | gtopt_gui | gtopt_guisrv |
|---------|-----------|--------------|
| Opens browser automatically | ✓ | ✗ |
| Kiosk/app mode | ✓ | ✗ |
| Runs as web server | ✓ | ✓ |
| Systemd service support | ✗ | ✓ |
| Remote access | Limited | Full |
| Use case | Interactive local use | Server deployment |

## Examples

### Basic workflow

```bash
# 1. Start the GUI service
gtopt_guisrv

# 2. Access from browser
# Open http://localhost:5001 in your browser

# 3. Upload and edit cases through the web interface
```

### Production deployment

```bash
# Install as system service
sudo cp /usr/local/share/gtopt/guiservice/gtopt-guiservice.service \
        /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable gtopt-guiservice
sudo systemctl start gtopt-guiservice

# Monitor logs
sudo journalctl -u gtopt-guiservice -f

# Stop service
sudo systemctl stop gtopt-guiservice
```

### Remote access

```bash
# Start on custom port accessible from network
gtopt_guisrv --port 8080 --host 0.0.0.0

# Access from another machine
# http://your-server-ip:8080
```

## Troubleshooting

### "Error: Required Python packages are not installed"

Install the dependencies:

```bash
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

### Port already in use

Use a different port:

```bash
gtopt_guisrv --port 5002
```

Or check what's using the port:

```bash
lsof -i :5001
```

### Service won't start (systemd)

Check the logs:

```bash
sudo journalctl -u gtopt-guiservice -n 50
```

Common issues:
- User 'gtopt' doesn't exist (create it first)
- Python dependencies not installed
- Port already in use

## Platform Support

- ✅ Ubuntu/Debian
- ✅ macOS
- ✅ WSL
- ✅ Windows (via gtopt_guisrv.bat)

## See Also

- [GTOPT_GUI.md](GTOPT_GUI.md) - Interactive kiosk mode launcher
- [README.md](README.md) - Full GUI service documentation
- [INSTALL.md](INSTALL.md) - Detailed installation guide
- [../webservice/GTOPT_WEBSRV.md](../webservice/GTOPT_WEBSRV.md) - Similar launcher for webservice
