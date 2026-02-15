# gtopt_gui - Standalone GUI Launcher

A command-line launcher for the gtopt GUI service that provides a kiosk-like
web interface for editing gtopt configuration files.

## Installation

### Option 1: Install guiservice only (Recommended)

Install just the GUI service without the gtopt binary:

```bash
# Build and install guiservice
cd gtopt
cmake -S guiservice -B build-gui
sudo cmake --install build-gui

# Install Python dependencies for the GUI
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

### Option 2: Install with standalone gtopt binary

If you've already built the standalone gtopt binary, you can install it first,
then install guiservice separately:

```bash
# Build and install gtopt standalone binary
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build

# Then install guiservice
cmake -S guiservice -B build-gui
sudo cmake --install build-gui

# Install Python dependencies for the GUI
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt
```

After installation:
- The `gtopt_gui` launcher is installed to `/usr/local/bin/gtopt_gui`
- The GUI service files are installed to `/usr/local/share/gtopt/guiservice`
- No modification to your shell or Python environment is required
- The guiservice installation is completely independent of the gtopt binary

## Requirements

- Python 3.10 or later
- Python packages: Flask, pandas, pyarrow, requests (see `guiservice/requirements.txt`)
- A web browser (Chrome/Chromium recommended for app mode)
- (Optional) Webservice for running optimizations - auto-started if installed
- (Optional) gtopt binary for solving cases - automatically detected if installed

## Usage

### Basic usage - Fully Integrated Experience

Launch the GUI with auto-start webservice:

```bash
gtopt_gui
```

This opens your web browser to the GUI interface where you can:
- Create a new case from scratch
- Upload an existing case ZIP file
- Edit system elements, simulation parameters, and options
- **Submit cases for optimization** (uses auto-started webservice)
- **View optimization results** (retrieved from webservice)
- Download the case as a ZIP file

**Zero Configuration Required**: If you have installed both guiservice and webservice:
- The webservice is automatically started in the background
- The gtopt binary is automatically detected
- All components work together seamlessly
- Clean shutdown when you press Ctrl+C

### With a configuration file

```bash
gtopt_gui system_c0.json
```

This starts the GUI (and webservice) and displays instructions for uploading your
configuration file. The GUI will show the path to your config file so you can
easily locate it in the file picker.

### Advanced Options

#### Specify custom ports

```bash
gtopt_gui --port 5001                    # GUI on port 5001
gtopt_gui --webservice-port 8080         # Webservice on port 8080
```

#### Disable webservice auto-start

If you want to use an external webservice or don't need solve functionality:

```bash
gtopt_gui --no-webservice
```

#### Use an external webservice

```bash
gtopt_gui --webservice-url http://remote-server:3000
```

This connects the GUI to a webservice running on a different machine.

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
  config_file              Path to gtopt configuration JSON file (optional)

Options:
  --port PORT              Port for the GUI service (default: auto-select)
  --webservice-port PORT   Port for the webservice (default: 3000)
  --no-browser             Don't open a web browser automatically
  --no-app-mode            Deprecated alias for regular browser mode
  --app-mode               Try to open browser in app/kiosk mode
  --no-webservice          Don't auto-start the webservice
  --webservice-url URL     Use external webservice at this URL
  --debug                  Run Flask in debug mode with auto-reload
  -h, --help               Show help message
```

## Features

- **Fully Integrated**: Auto-starts webservice for solving optimization cases
- **Zero Configuration**: Automatically detects gtopt binary and webservice
- **Standalone operation**: Runs entirely in user space, no root required
- **Auto port selection**: Automatically finds available ports if not specified
- **Browser compatibility first**: Opens in regular browser mode by default; `--app-mode` is available for kiosk-style launch
- **Graceful shutdown**: Press Ctrl+C to stop all services cleanly
- **Cross-platform**: Works on Ubuntu, macOS, and WSL
- **Modular**: Webservice runs in separate process, clean separation of concerns

## Complete Workflow - Edit, Solve, View Results

With the integrated webservice, you can now complete the entire workflow without
leaving the GUI:

### 1. Create or Upload a Case

```bash
gtopt_gui
```

- Click "Upload Case" to import an existing case ZIP
- Or create a new case from scratch using the GUI forms

### 2. Edit the Case

- Add/edit buses, generators, storage, demands, etc.
- Configure simulation parameters (horizon, intervals, etc.)
- Set solver options

### 3. Submit for Optimization

- Click "Submit for Solving" button
- The case is automatically sent to the local webservice
- You receive a job token to track progress

### 4. Monitor Progress

- The GUI polls the webservice automatically
- View job status: pending → running → completed/failed

### 5. View Results

- Once completed, results are automatically retrieved
- View generation schedules, storage levels, demand fulfillment
- See charts and tables in the GUI

All of this happens seamlessly without any manual configuration!

## Working with Config Files

The GUI service doesn't load config files directly from the command line.
Instead, it provides a web interface where you can:

1. **Upload a case**: Click "Upload Case" and select a ZIP file containing
   your system JSON and input data files (Parquet or CSV).

2. **Edit the case**: Use the web interface to modify system elements,
   simulation settings, and options.

3. **Solve the case**: Click "Submit for Solving" to run optimization
   (requires webservice - auto-started by default).

4. **View results**: Once solving completes, view results directly in the GUI
   with interactive charts and tables.

5. **Download**: Click "Download ZIP" to save your case or results.

## Examples

### Complete workflow with integrated services

```bash
# Start GUI with webservice
gtopt_gui

# In the browser:
# 1. Upload or create a case
# 2. Edit system elements
# 3. Click "Submit for Solving"
# 4. Wait for completion
# 5. View results
```

### Basic workflow without solving

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

### "Webservice not available. Solve functionality disabled."

This message appears when the webservice is not installed or failed to start.

**Solutions:**
1. Install the webservice:
   ```bash
   cmake -S webservice -B build-web
   sudo cmake --install build-web
   cd /usr/local/share/gtopt/webservice
   npm install && npm run build
   ```

2. Or use an external webservice:
   ```bash
   gtopt_gui --webservice-url http://localhost:3000
   ```

### "Warning: gtopt binary not found"

The webservice will start but solving will fail without the gtopt binary.

**Solutions:**
1. Install gtopt standalone:
   ```bash
   cmake -S standalone -B build
   cmake --build build
   sudo cmake --install build
   ```

2. Or set the binary location:
   ```bash
   export GTOPT_BIN=/path/to/gtopt
   gtopt_gui
   ```

### Webservice fails to start

If you see "Failed to start webservice", check:

1. Node.js and npm are installed:
   ```bash
   node --version
   npm --version
   ```

2. Webservice is built:
   ```bash
   ls -la /usr/local/share/gtopt/webservice/.next
   ```

3. Port 3000 is not already in use:
   ```bash
   gtopt_gui --webservice-port 8080
   ```

### Browser doesn't open in app mode

App mode requires Chrome or Chromium and is only used when `--app-mode` is set. If not available:
- The browser opens in a regular tab instead
- Omit `--app-mode` (default behavior) for maximum compatibility

## Architecture

The integrated `gtopt_gui` consists of three components:

1. **GUI Service (Flask)**: Provides the web interface and API for case editing
   - Runs on auto-selected port (or specified with `--port`)
   - Located in guiservice/

2. **Webservice (Next.js)**: Handles job submission and execution
   - Runs on port 3000 (or specified with `--webservice-port`)
   - Auto-started by gtopt_gui (unless `--no-webservice`)
   - Located in webservice/

3. **gtopt Binary**: The optimization solver
   - Auto-detected in PATH or common locations
   - Called by webservice to solve cases

All three components are automatically started and coordinated by `gtopt_gui`.
On exit (Ctrl+C), all processes are cleanly terminated.

## Platform Support

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
- **Windows**: Supported via `gtopt_gui.bat` (requires Python installation)

### Windows Usage

On Windows (not WSL), use the batch script:

```cmd
gtopt_gui.bat
gtopt_gui.bat system_c0.json
```

The batch script (`gtopt_gui.bat`) provides the same functionality as the Unix
shell script and is installed alongside the gtopt binary on Windows.

**Note**: On Windows, make sure Python is installed and available in your PATH.
Download Python from [python.org](https://www.python.org/) if needed.

## Security Notes

- The service binds to `0.0.0.0` (all interfaces) by default
- For remote access, consider using SSH port forwarding or a reverse proxy
- No authentication is provided by default
- Suitable for local development and trusted networks only

## See Also

- [README.md](README.md) - Full GUI service documentation
- [INSTALL.md](INSTALL.md) - Detailed installation guide
- [../USAGE.md](../USAGE.md) - gtopt command-line usage
