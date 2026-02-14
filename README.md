# Gtopt

[![Ubuntu](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/ubuntu.yml)
[![Webservice](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/webservice.yml)
[![Webservice E2E](https://github.com/marcelomatus/gtopt/actions/workflows/webservice-e2e.yml/badge.svg)](https://github.com/marcelomatus/gtopt/actions/workflows/webservice-e2e.yml)

A high-performance C++ tool for **Generation and Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost of operation and expansion of electrical power systems.

## Table of Contents

- [Features](#features)
- [Quick Install (Ubuntu)](#quick-install-ubuntu)
- [Usage](#usage)
- [Building from Source](#building-from-source)
- [Running the Sample Case](#running-the-sample-case)
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

## Usage

Run gtopt on a system configuration file:

```bash
gtopt system_config.json
```

Or specify an input directory and system file separately:

```bash
gtopt --input-directory data_dir --system-file config.json
```

Common options:

```bash
# Output results to a specific directory
gtopt system_c0.json --output-directory results/

# Output in Parquet format with gzip compression
gtopt system_c0.json --output-format parquet --compression-format gzip

# Single-bus mode (ignore network topology)
gtopt system_c0.json --use-single-bus

# Enable DC power flow (Kirchhoff's laws)
gtopt system_c0.json --use-kirchhoff

# Export the LP model to a file
gtopt system_c0.json --lp-file model.lp

# Build the LP model without solving
gtopt system_c0.json --lp-file model.lp --just-create

# Merge multiple system files
gtopt base_system.json additional_generators.json
```

### Options Reference

| Short | Long Flag | Argument | Description |
| ----- | --------- | -------- | ----------- |
| `-h` | `--help` | | Show help message and exit |
| `-V` | `--version` | | Show program version and exit |
| `-v` | `--verbose` | | Activate maximum verbosity (trace-level logging) |
| `-q` | `--quiet` | `[=arg]` | Suppress log output to stdout |
| `-s` | `--system-file` | `arg` | System JSON file(s) (also accepted as positional args) |
| `-D` | `--input-directory` | `arg` | Override input data directory |
| `-F` | `--input-format` | `arg` | Input data format: `parquet`, `csv` |
| `-d` | `--output-directory` | `arg` | Directory for output files |
| `-f` | `--output-format` | `arg` | Output format: `parquet`, `csv` |
| `-C` | `--compression-format` | `arg` | Parquet compression: `uncompressed`, `gzip`, `zstd`, `lzo` |
| `-b` | `--use-single-bus` | `[=arg]` | Use single-bus mode (ignore network) |
| `-k` | `--use-kirchhoff` | `[=arg]` | Use Kirchhoff (DC power flow) mode |
| `-n` | `--use-lp-names` | `[=arg]` | Use named rows/columns in LP (0=off, 1=names, 2=names+map) |
| `-l` | `--lp-file` | `arg` | Save LP model to a file |
| `-j` | `--json-file` | `arg` | Save merged system configuration to JSON |
| `-e` | `--matrix-eps` | `arg` | Epsilon for matrix sparsity |
| `-c` | `--just-create` | `[=arg]` | Build LP model and exit without solving |
| `-p` | `--fast-parsing` | `[=arg]` | Use fast (non-strict) JSON parsing |

For detailed usage instructions, system file format, input/output file
descriptions, and more examples, see **[USAGE.md](USAGE.md)**.

## Building from Source

### Dependencies

| Dependency | Purpose | Ubuntu Package |
|-----------|---------|----------------|
| GCC 14+ | C++26 compiler | `gcc-14 g++-14` |
| CMake 3.31+ | Build system | `cmake` (or install from [cmake.org](https://cmake.org/download/)) |
| Boost | Container library | `libboost-container-dev` |
| Apache Arrow | Parquet I/O | `libarrow-dev libparquet-dev` |
| COIN-OR CBC | LP/MIP solver | `coinor-libcbc-dev` |

#### Installing dependencies on Ubuntu

**GCC 14:**
```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
```

**Boost:**
```bash
sudo apt-get install -y -V libboost-container-dev
```

**Apache Arrow / Parquet:**
```bash
sudo apt-get install -y -V ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

**Solver** (CBC — open-source; alternatives: HiGHS, Clp, CPLEX, Gurobi):
```bash
sudo apt-get install -y -V coinor-libcbc-dev
```

### Building the standalone binary

```bash
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/gtopt --help
```

### Installing system-wide

```bash
sudo cmake --install build
# Installs to /usr/local/bin/gtopt by default
```

To install to a custom prefix:

```bash
cmake --install build --prefix /opt/gtopt
# Binary will be at /opt/gtopt/bin/gtopt
```

### Building and running the test suite

```bash
CC=gcc-14 CXX=g++-14 cmake -S test -B build/test
cmake --build build/test -j$(nproc)
ctest --test-dir build/test
```

### End-to-end integration tests

The standalone build includes end-to-end tests that run gtopt against the
sample case in `cases/c0/` and validate output against reference results:

```bash
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build
```

### Formatting and linting

```bash
cmake --build build --target fix-format
```

## Running the Sample Case

The repository includes a sample case in `cases/c0/` — a single-bus system
with one generator and one demand with capacity expansion over 5 stages.

```bash
cd cases/c0
gtopt system_c0.json
```

The solver produces output files organized by component type:

```
output/
├── solution.csv                    # Objective value and solver status
├── Generator/
│   ├── generation_sol.csv          # Dispatch (MW) per stage/block
│   └── generation_cost.csv         # Generation cost
├── Demand/
│   ├── load_sol.csv                # Served load per stage/block
│   ├── fail_sol.csv                # Unserved demand (load shedding)
│   ├── capainst_sol.csv            # Installed capacity per stage
│   ├── expmod_sol.csv              # Expansion decisions
│   └── ...                         # Cost and dual files
└── Bus/
    └── balance_dual.csv            # Nodal prices (marginal cost of energy)
```

The `solution.csv` reports the optimization result:

```
obj_value,23.163424133184083
    kappa,1
   status,0
```

A status of `0` indicates an optimal solution was found.

To write output to a separate directory:

```bash
gtopt system_c0.json --output-directory /tmp/c0_results
```

For a full description of the system file format, input data layout, output
files, and advanced examples, see **[USAGE.md](USAGE.md)**.

## GUI Service

A web-based graphical interface for creating, editing, and visualizing gtopt
cases. The GUI service also connects to the gtopt webservice for remote
solving.

### Quick Start with gtopt_gui

The easiest way to launch the GUI is with the `gtopt_gui` command:

```bash
# Install guiservice (independent of gtopt binary)
cmake -S guiservice -B build-gui
sudo cmake --install build-gui

# Install Python dependencies
pip3 install -r /usr/local/share/gtopt/guiservice/requirements.txt

# Launch interactively (opens browser in kiosk mode)
gtopt_gui

# Or with a specific configuration file
gtopt_gui system_c0.json
```

This opens a web browser with a standalone GUI interface for editing cases.
For detailed usage, see [guiservice/GTOPT_GUI.md](guiservice/GTOPT_GUI.md).

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

# Install Node.js dependencies
cd /usr/local/share/gtopt/webservice
npm install --production
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
