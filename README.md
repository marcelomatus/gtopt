# Gtopt

A high-performance C++ tool for **Generation and Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost of operation and expansion of electrical power systems.

## Features

* **Cost Optimization**: minimizes investment (CAPEX) and operational (OPEX) costs.
* **System Modeling**: supports single-bus or multi-bus DC power flow (Kirchhoff laws).
* **Flexible I/O**: high-speed parsing and export to Parquet, CSV, and JSON.
* **Scalability**: designed for large-scale grids with sparse matrix assembly.

## Usage

1. [Build the standalone target](building-the-standalone-target)
2. Run the binary as follows:
```bash
./build/standalone/gtopt --input-directory data/ --system-file config.json

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
* **Boost**: program_options, filesystem: See https://www.boost.org/doc/user-guide/getting-started.html
* **Apache Arrow**: Parquet support: See https://arrow.apache.org/install/
* **Solver**: (e.g., HiGHS, Clp, CPLEX, Gurobi)

### Building the standalone target

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/standalone/gtopt --help

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
        subprocess.run(["./build/standalone/gtopt", "-s", f.name, "-D", folder])

if __name__ == "__main__":
    run_all_scenarios("cases/c0/")

```
