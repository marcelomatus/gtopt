# Gtopt

A high-performance C++ tool for **Generation and Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost of operation and expansion of electrical power systems.

## Features

* **Cost Optimization**: minimizes investment (CAPEX) and operational (OPEX) costs.
* **System Modeling**: supports single-bus or multi-bus DC power flow (Kirchhoff laws).
* **Flexible I/O**: high-speed parsing and export to Parquet, CSV, and JSON.
* **Scalability**: designed for large-scale grids with sparse matrix assembly.

## Usage

1. Build and install the standalone binary
In Ubuntu:
```bash
cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
sudo cmake --build build --target install -j$(nproc)
```
2. Run the binary as follows:

In Ubuntu:
```bash
gtopt --input-directory data_dir --system-file config.json
```



### Options Reference

| Short Flag | Long Flag | Argument | Description |
| --- | --- | --- | --- |
| `-h` | `--help` |  | describes arguments |
| `-v` | `--verbose` |  | activates maximum verbosity |
| `-q` | `--quiet` | `[=arg]` | do not log to stdout |
| `-V` | `--version` |  | shows program version |
| `-s` | `--system-file` | `arg` | name of the system file |
| `-l` | `--lp-file` | `arg` | name of the lp file to save |
| `-j` | `--json-file` | `arg` | name of the json file to save |
| `-D` | `--input-directory` | `arg` | input directory |
| `-f` | `--output-format` | `arg` | output format `[parquet, csv]` |
| `-C` | `--compression` | `arg` | compression in parquet `[uncompressed, gzip, zstd, lzo]` |
| `-b` | `--use-single-bus` | `[=arg]` | use single bus mode |
| `-k` | `--use-kirchhoff` | `[=arg]` | use Kirchhoff mode |
| `-e` | `--matrix-eps` | `arg` | eps value for matrix sparsity |
| `-c` | `--just-create` | `[=arg]` | build model and exit without solving |

## Building from Source

### Dependencies

* **C++26** compiler (i.e., g++14 or clang++22)

In Ubuntu:
```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
```
* **Boost**: program_options, filesystem: See https://www.boost.org/doc/user-guide/getting-started.html

In Ubuntu:
```bash
sudo apt-get install -y -V libboost-container-dev
```

* **Apache Arrow**: Parquet support: See https://arrow.apache.org/install/

In Ubuntu:
```bash
sudo apt-get install -y -V ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

* **Solver**: (e.g., HiGHS, Clp, CPLEX, Gurobi)

In Ubuntu:
```bash
sudo apt-get install -y -V coinor-libcbc-dev
```

### Building the standalone target

```bash
cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/gtopt --help
```

### Building and run test suite

```bash
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test

```

### Formatting and Linting

```bash
# apply clang-format
cmake --build build --target fix-format

```

### Batch execution (Python)

```python
import subprocess
from pathlib import Path

def run_all_scenarios(folder):
    # find scenario files
    for f in Path(folder).glob("*.json"):
        # run gtopt for each system file
        subprocess.run(["gtopt", "-s", f.name, "-D", folder])

if __name__ == "__main__":
    run_all_scenarios("cases/c0/")

```

## Web Service

gtopt includes a web service that lets you upload optimization cases, run the
solver, and download results — all through a browser or REST API.

### Quick Start

```bash
# 1. Build and install gtopt (see "Building from Source" above)
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
