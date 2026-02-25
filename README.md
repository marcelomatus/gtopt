# Gtopt

[![Ubuntu](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml)
[![Webservice](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml)
[![Guiservice](https://github.com/marcelomatus/gtopt/actions/workflows/guiservice.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/guiservice.yml)
[![Style](https://github.com/marcelomatus/gtopt/actions/workflows/style.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/style.yml)

A high-performance C++ tool for **Generation and Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost of operation and expansion of electrical power systems.

## Documentation Guide

This project includes comprehensive documentation for different use cases:

- **[README.md](README.md)** (this file) - Project overview, quick installation, and basic usage
- **[BUILDING.md](BUILDING.md)** - Detailed build instructions for all platforms, dependencies, and troubleshooting
- **[USAGE.md](USAGE.md)** - Complete command-line reference, examples, and advanced usage patterns
- **[INPUT_DATA.md](INPUT_DATA.md)** - Input data structure and file format reference
- **[webservice/INSTALL.md](webservice/INSTALL.md)** - Web service installation, deployment, and API reference
- **[guiservice/INSTALL.md](guiservice/INSTALL.md)** - GUI service installation, deployment, and usage guide
- **[SCRIPTS.md](SCRIPTS.md)** - Python conversion utilities (`plp2gtopt`, `igtopt`, `cvs2parquet`)

## Table of Contents

- [Features](#features)
- [Quick Install (Ubuntu)](#quick-install-ubuntu)
- [Usage](#usage)
- [Running the Sample Case](#running-the-sample-case)
- [Python Scripts](#python-scripts)
- [GUI Service](#gui-service)
- [Web Service](#web-service)
- [License](#license)

## Features

* **Cost Optimization**: minimizes investment (CAPEX) and operational (OPEX) costs.
* **System Modeling**: supports single-bus or multi-bus DC power flow (Kirchhoff laws).
* **Flexible I/O**: high-speed parsing and export to Parquet, CSV, and JSON.
* **Scalability**: designed for large-scale grids with sparse matrix assembly.
* **Web Service**: browser-based UI and REST API for submitting and retrieving optimization results.

## Quick Install (Ubuntu)

Install all dependencies, build, and install `gtopt` system-wide:

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14 libboost-container-dev coinor-libcbc-dev \
  ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev

# 2. Clone and build
git clone https://github.com/marcelomatus/gtopt.git
cd gtopt
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 3. Install system-wide
sudo cmake --install build

# 4. Verify
gtopt --version
```

For detailed build instructions, alternative platforms, troubleshooting, and build options, see **[BUILDING.md](BUILDING.md)**.

## Usage

Run gtopt on a system configuration file:

```bash
gtopt system_config.json
```

Common options:

```bash
# Output results to a specific directory
gtopt system_c0.json --output-directory results/

# Single-bus mode (ignore network topology)
gtopt system_c0.json --use-single-bus

# Enable DC power flow (Kirchhoff's laws)
gtopt system_c0.json --use-kirchhoff
```

For complete command-line reference, advanced examples, and detailed usage instructions, see **[USAGE.md](USAGE.md)**.

## Running the Sample Case

The repository includes a sample case in `cases/c0/` — a single-bus system with one generator and one demand with capacity expansion over 5 stages.

```bash
cd cases/c0
gtopt system_c0.json
```

The solver produces output files organized by component type in the `output/` directory. A status of `0` in `solution.csv` indicates an optimal solution was found.

For detailed output file descriptions, system file format, and advanced examples, see **[USAGE.md](USAGE.md#running-the-sample-case)**.

## Python Scripts

The `scripts/` directory contains three Python utilities for preparing and
converting data for use with gtopt:

| Command | Purpose |
|---------|---------|
| `plp2gtopt` | Convert a PLP (PLPMAX/PLPOPT) case to gtopt JSON + Parquet |
| `igtopt` | Convert an Excel workbook to a gtopt JSON case |
| `cvs2parquet` | Convert CSV time-series files to Parquet format |

### Install

```bash
pip install ./scripts
```

### plp2gtopt — PLP to gtopt converter

Reads the standard PLP data files and writes a self-contained gtopt JSON file
together with Parquet time-series files:

```bash
# Basic conversion
plp2gtopt -i plp_case_dir -o gtopt_case_dir

# Create a ZIP archive ready to upload to gtopt_guisrv / gtopt_websrv
plp2gtopt -z -i plp_case_2y -o gtopt_case_2y

# Two hydrology scenarios with 60/40 probability split
plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

# Apply a 10% annual discount rate
plp2gtopt -i input/ -d 0.10
```

After conversion, `plp2gtopt` prints statistics covering the number of buses,
generators, demands, lines, blocks, stages, and scenarios — similar to the
stats printed by the `gtopt` solver itself.

For the full reference, see **[SCRIPTS.md](SCRIPTS.md)**.

## GUI Service

A web-based graphical interface for creating, editing, and visualizing gtopt
cases. The GUI service also connects to the gtopt webservice for remote
solving.

### Quick Start with gtopt_gui

The easiest way to launch the GUI is with the `gtopt_gui` command, which provides
a complete integrated environment for editing, solving, and viewing optimization cases:

```bash
# Install guiservice (independent of gtopt binary)
cmake -S guiservice -B build-gui
sudo cmake --install build-gui

# Install Python dependencies
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt

# Launch interactively (opens browser, auto-starts webservice)
gtopt_gui

# Or with a specific configuration file
gtopt_gui system_c0.json
```

**Fully Integrated Workflow:**
- Opens a web browser with a standalone GUI interface
- Automatically starts local webservice (if installed) for solving cases
- Automatically detects gtopt binary
- Complete workflow: Edit → Submit for Solving → Monitor → View Results
- Zero configuration required

**In the browser:**
1. Upload or create a case
2. Edit system elements (buses, generators, storage, demands)
3. Click "Submit for Solving" - runs on auto-started local webservice
4. Monitor job progress in real-time
5. View results with interactive charts and tables

For detailed usage and options, see [guiservice/GTOPT_GUI.md](guiservice/GTOPT_GUI.md).

### Quick Start with gtopt_guisrv

For running the GUI service as a web server (without opening a browser):

```bash
# Start GUI service on default port 5001
gtopt_guisrv

# Or with custom options
gtopt_guisrv --port 8080
gtopt_guisrv --debug
```

For detailed usage and systemd service setup, see [guiservice/GTOPT_GUISRV.md](guiservice/GTOPT_GUISRV.md).

### Manual Start

You can also run the GUI service directly:

```bash
pip install -r guiservice/requirements.txt
cd guiservice
python app.py
# Open http://localhost:5001 in your browser
```

For detailed installation and deployment instructions, see
[guiservice/INSTALL.md](guiservice/INSTALL.md).

## Web Service

gtopt includes a web service that lets you upload optimization cases, run the
solver, and download results — all through a browser or REST API.

### Quick Start with gtopt_websrv

Install and run the web service using the `gtopt_websrv` command:

```bash
# Install webservice (independent of gtopt binary)
cmake -S webservice -B build-web
sudo cmake --install build-web

# Install Node.js dependencies (including build dependencies)
cd /usr/local/share/gtopt/webservice
npm install
npm run build

# Launch the web service
gtopt_websrv

# Or with options
gtopt_websrv --port 8080
gtopt_websrv --gtopt-bin /path/to/gtopt
```

For detailed installation and deployment instructions, see [webservice/INSTALL.md](webservice/INSTALL.md).

## License

This project is released into the public domain under the
[Unlicense](LICENSE).
