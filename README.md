# Gtopt

[![Ubuntu](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml)
[![Webservice](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml)
[![Guiservice](https://github.com/marcelomatus/gtopt/actions/workflows/guiservice.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/guiservice.yml)
[![Style](https://github.com/marcelomatus/gtopt/actions/workflows/style.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/style.yml)

A high-performance C++ tool for **Generation and Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost of operation and expansion of electrical power systems.

## Documentation Guide

This project includes comprehensive documentation for different use cases:

- **[README.md](README.md)** (this file) - Project overview, quick installation, and basic usage
- **[PLANNING_GUIDE.md](PLANNING_GUIDE.md)** - Complete planning guide: time structure, system elements, JSON format, and worked examples
- **[MATHEMATICAL_FORMULATION.md](docs/formulation/MATHEMATICAL_FORMULATION.md)** - Full LP/MIP optimization formulation with LaTeX notation
- **[BUILDING.md](BUILDING.md)** - Detailed build instructions for all platforms, dependencies, and troubleshooting
- **[USAGE.md](USAGE.md)** - Complete command-line reference, examples, and advanced usage patterns
- **[INPUT_DATA.md](INPUT_DATA.md)** - Input data structure and file format reference
- **[USER_CONSTRAINTS.md](USER_CONSTRAINTS.md)** - User-defined LP constraints: AMPL-inspired syntax, domain specs, external files
- **[SCRIPTS.md](SCRIPTS.md)** - Python conversion utilities ([igtopt](docs/scripts/igtopt.md), [plp2gtopt](docs/scripts/plp2gtopt.md), [pp2gtopt](docs/scripts/pp2gtopt.md), [ts2gtopt](docs/scripts/ts2gtopt.md), [cvs2parquet](docs/scripts/cvs2parquet.md), [gtopt_diagram](docs/scripts/gtopt_diagram.md))
- **[TOOL_COMPARISON.md](docs/TOOL_COMPARISON.md)** - Detailed comparison of gtopt vs PLP, pandapower, and other tools (elements, parameters, units, methodology)
- **[SDDP_SOLVER.md](docs/SDDP_SOLVER.md)** - SDDP solver: theory, options, monitoring API, elastic filter modes, and JSON configuration
- **[MONOLITHIC_SOLVER.md](docs/MONOLITHIC_SOLVER.md)** - Default monolithic solver, boundary cuts, and sequential mode
- **[CHANGELOG.md](CHANGELOG.md)** - Release history and notable changes
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - Contribution guidelines, code style, and testing
- **[webservice/INSTALL.md](webservice/INSTALL.md)** - Web service installation, deployment, and API reference
- **[guiservice/INSTALL.md](guiservice/INSTALL.md)** - GUI service installation, deployment, and usage guide
- **[DIAGRAM_TOOL.md](DIAGRAM_TOOL.md)** - Network and planning diagram tool with aggregation and large-case support

## Table of Contents

- [Features](#features)
- [Quick Install (Ubuntu)](#quick-install-ubuntu)
- [Usage](#usage)
- [Running the Sample Case](#running-the-sample-case)
- [Python Scripts](#python-scripts)
- [GUI Service](#gui-service)
- [Web Service](#web-service)
- [Related Tools](#related-tools)
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

For a step-by-step walkthrough of running and interpreting the simplest case
(`ieee_4b_ori`), see the **[Quickstart: Your First Solve](PLANNING_GUIDE.md#quickstart-your-first-solve)**
section in the Planning Guide. For detailed output file descriptions, system
file format, and advanced examples, see **[USAGE.md](USAGE.md#running-the-sample-case)**.

## Python Scripts

The `scripts/` directory contains three Python utilities for preparing and
converting data for use with gtopt:

| Command | Purpose |
|---------|---------|
| `plp2gtopt` | Convert a PLP case to gtopt JSON + Parquet |
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

## Related Tools

gtopt integrates with and compares against several established power system
tools. The Python scripts in `scripts/` provide converters and validators
for interoperability:

| Tool | Language | Role with gtopt |
|------|----------|-----------------|
| **[pandapower](https://www.pandapower.org/)** | Python | DC OPF reference solver; `pp2gtopt` imports pandapower networks; `gtopt_compare` validates gtopt results against pandapower on standard IEEE test cases |
| **[PLP](https://github.com/marcelomatus/plp_storage)** | Fortran | Hydrothermal scheduling tool widely used in Latin America; `plp2gtopt` converts PLP `.dat` input files to gtopt JSON + Parquet |
| **[PyPSA](https://pypsa.org/)** | Python | Linear optimal power flow with multi-period investment planning; shares the same LP/MIP mathematical structure as gtopt (see [Mathematical Formulation](docs/formulation/MATHEMATICAL_FORMULATION.md)) |
| **[GenX](https://genxproject.github.io/GenX/)** | Julia | Capacity expansion model; similar modular investment + storage SoC formulation to gtopt |

### pandapower

[pandapower](https://www.pandapower.org/) is an open-source Python tool for
power system analysis, including AC/DC power flow and DC optimal power flow.

- **`pp2gtopt`**: converts a pandapower network (JSON) to a gtopt case,
  enabling direct use of any pandapower network in gtopt.
- **`gtopt_compare`**: validates gtopt dispatch results against pandapower
  DC OPF on the built-in IEEE test cases (`ieee_4b_ori`, `ieee30b`, `ieee_57b`,
  `bat_4b_24`).

### PLP

[PLP](https://github.com/marcelomatus/plp_storage) (*Programación de Largo Plazo*) is a
Fortran-based hydrothermal unit commitment and economic dispatch tool used in
several Latin American power systems.

- **`plp2gtopt`**: reads the full set of PLP `.dat` files and produces a
  self-contained gtopt JSON + Parquet case, preserving multi-scenario
  hydrology, block/stage structure, battery storage, and maintenance schedules.
- Sample PLP cases are in `scripts/cases/` (e.g., `plp_dat_ex`, `plp_min_bess`,
  `plp_bat_4b_24`).

## License

This project is released into the public domain under the
[Unlicense](LICENSE).
