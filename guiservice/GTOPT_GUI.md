# gtopt_gui - Standalone GUI Launcher

A command-line launcher for the gtopt GUI service that provides a kiosk-like
web interface for editing gtopt configuration files.

## Installation

The `gtopt_gui` command is installed automatically when you build and install
the standalone gtopt binary:

```bash
# Build and install gtopt
cd gtopt
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build

# Install Python dependencies for the GUI
pip3 install -r guiservice/requirements.txt
```

After installation:
- The `gtopt_gui` launcher is installed to `/usr/local/bin/gtopt_gui`
- The GUI service files are installed to `/usr/local/share/gtopt/guiservice`
- No modification to your shell or Python environment is required

## Requirements

- Python 3.10 or later
- Python packages: Flask, pandas, pyarrow, requests (see `guiservice/requirements.txt`)
- A web browser (Chrome/Chromium recommended for app mode)

## Usage

### Basic usage

Launch the GUI without a config file:

```bash
gtopt_gui
```

This opens your web browser to the GUI interface where you can:
- Create a new case from scratch
- Upload an existing case ZIP file
- Edit system elements, simulation parameters, and options
- Download the case as a ZIP file
- Upload and view results

### With a configuration file

```bash
gtopt_gui system_c0.json
```

This starts the GUI and displays instructions for uploading your configuration
file. The GUI will show the path to your config file so you can easily locate
it in the file picker.

### Specify a custom port

```bash
gtopt_gui --port 5001
```

### Run without opening a browser

```bash
gtopt_gui --no-browser
```

This starts the service but doesn't open a browser. Useful if you want to
manually open the browser or connect from a different machine.

### Debug mode

```bash
gtopt_gui --debug
```

Runs Flask in debug mode with auto-reload for development.

## Options

```
gtopt_gui [options] [config_file]

Positional arguments:
  config_file          Path to gtopt configuration JSON file (optional)

Options:
  --port PORT          Port for the web service (default: auto-select)
  --no-browser         Don't open a web browser automatically
  --app-mode           Try to open browser in app/kiosk mode (default: True)
  --debug              Run Flask in debug mode with auto-reload
  -h, --help           Show help message
```

## Features

- **Standalone operation**: Runs entirely in user space, no root required
- **Auto port selection**: Automatically finds an available port if not specified
- **Browser app mode**: Opens in app mode on Chrome/Chromium for a cleaner interface
- **Graceful shutdown**: Press Ctrl+C to stop the service cleanly
- **Cross-platform**: Works on Ubuntu, macOS, and WSL

## Working with Config Files

The GUI service doesn't load config files directly from the command line.
Instead, it provides a web interface where you can:

1. **Upload a case**: Click "Upload Case" and select a ZIP file containing
   your system JSON and input data files (Parquet or CSV).

2. **Edit the case**: Use the web interface to modify system elements,
   simulation settings, and options.

3. **Download the case**: Click "Download ZIP" to save your work.

4. **View results**: If you have run `gtopt` on your config file and have
   results in the output directory, you can zip the output directory and
   upload it via "View Results" to see charts and tables.

## Examples

### Basic workflow

```bash
# 1. Start the GUI
gtopt_gui

# 2. In the browser:
#    - Click "Upload Case"
#    - Navigate to your case directory and select the ZIP file
#    - Edit as needed
#    - Click "Download ZIP" to save

# 3. Run gtopt on the downloaded case
unzip my_case.zip
cd my_case
gtopt system.json

# 4. View results in the GUI
cd ..
zip -r results.zip my_case/output
# Upload results.zip via "View Results" button
```

### With an existing case

```bash
cd /path/to/my/cases/c0
gtopt_gui system_c0.json

# The terminal will show:
# "To work with your configuration file (system_c0.json):
#   1. Click the 'Upload Case' button
#   2. Navigate to and select: /path/to/my/cases/c0/system_c0.json"
```

### Custom port for remote access

```bash
gtopt_gui --port 8080 --no-browser

# Connect from another machine:
# http://your-machine-ip:8080
```

## Troubleshooting

### "Required Python packages are not installed"

Install the dependencies:

```bash
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

Or use a virtual environment:

```bash
python3 -m venv ~/.gtopt-gui-venv
source ~/.gtopt-gui-venv/bin/activate
pip install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

Then run `gtopt_gui` from within the activated environment.

### "Address already in use"

Another process is using the port. Either:
- Stop the other process, or
- Use a different port: `gtopt_gui --port 5002`

### Browser doesn't open

Use the `--no-browser` flag and manually open the URL shown in the terminal.

## How It Works

The `gtopt_gui` command:

1. Locates the guiservice directory (installed alongside the gtopt binary)
2. Finds an available TCP port (or uses the one specified with `--port`)
3. Starts the Flask application on that port
4. Opens your default web browser to the GUI interface
5. Monitors the Flask process and displays its logs
6. Gracefully shuts down the Flask process when you press Ctrl+C

## Platform Support

- **Ubuntu / Debian**: Fully supported
- **macOS**: Fully supported
- **WSL (Windows Subsystem for Linux)**: Fully supported
- **Windows**: Not directly supported, but can run via WSL

## Security Notes

- The service binds to `0.0.0.0` (all interfaces) by default
- For remote access, consider using SSH port forwarding or a reverse proxy
- No authentication is provided by default
- Suitable for local development and trusted networks only

## See Also

- [guiservice/README.md](guiservice/README.md) - Full GUI service documentation
- [guiservice/INSTALL.md](guiservice/INSTALL.md) - Detailed installation guide
- [USAGE.md](../USAGE.md) - gtopt command-line usage
