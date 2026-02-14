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
- [Project Structure](#project-structure)
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

### Quick Start

```bash
pip install -r guiservice/requirements.txt
cd guiservice
python app.py
# Open http://localhost:5001 in your browser
```

For detailed installation and deployment instructions, see
[guiservice/INSTALL.md](guiservice/INSTALL.md).

### Running the tests

```bash
pip install pytest
python -m pytest guiservice/tests/test_app.py -v
```

## Web Service

gtopt includes a web service that lets you upload optimization cases, run the
solver, and download results — all through a browser or REST API.

# 1. Build and install gtopt (see above)
sudo cmake --install build

# 2. Build the web service
cd webservice
npm ci
npm run build

# 3. Start the service
GTOPT_BIN=/usr/local/bin/gtopt npm run start
```

The service will be available at `http://localhost:3000`.

### How It Works

1. **Upload** a `.zip` archive containing a system JSON file and its data
   directory (e.g., `system_c0.json` + `system_c0/`).
2. **Submit** the job and receive a unique **token**.
3. **Check status** using the token — the solver runs asynchronously.
4. **Download results** as a `.zip` once the job completes.

### API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/jobs` | Upload a case archive and start a job |
| `GET` | `/api/jobs` | List all submitted jobs |
| `GET` | `/api/jobs/:token` | Check job status |
| `GET` | `/api/jobs/:token/download` | Download results |

### Deployment Guide

For complete instructions on server setup, production deployment with systemd,
nginx reverse proxy, TLS, and troubleshooting, see
**[INSTALL_WEBSERVICE.md](INSTALL_WEBSERVICE.md)**.

## Project Structure

```
gtopt/
├── CMakeLists.txt              # Library build configuration
├── include/gtopt/              # Public C++ headers
├── source/                     # Library source files
├── standalone/                 # Standalone binary (cmake -S standalone)
│   ├── CMakeLists.txt
│   └── source/
├── test/                       # Unit tests (cmake -S test)
│   └── CMakeLists.txt
├── cases/                      # Sample optimization cases
│   └── c0/                     # Sample case with reference output
├── webservice/                 # Next.js web service
│   ├── src/app/                # UI and API routes
│   ├── test/                   # Integration and e2e tests
│   └── README.md               # Webservice-specific docs
├── cmake/                      # CMake modules (CPM, tools)
├── cmake_local/                # Solver detection modules
├── USAGE.md                    # Detailed usage guide and examples
├── INSTALL_WEBSERVICE.md       # Webservice deployment guide
└── README.md                   # This file
```

## License

This project is released into the public domain under the
[Unlicense](LICENSE).
