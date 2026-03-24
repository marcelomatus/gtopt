# cvs2parquet — CSV to Parquet Converter

Converts **CSV time-series files** to Apache Parquet format with automatic
column type enforcement.

---

## Overview

gtopt accepts time-series input data in both CSV and Parquet formats, but
Parquet is preferred for production use due to its compression, type safety,
and faster I/O.  `cvs2parquet` provides a simple batch conversion tool that
reads one or more CSV files and writes Parquet equivalents with consistent
column types.

The tool enforces gtopt's column conventions:

- Index columns (`stage`, `block`, `scenario`) are stored as **int32**.
- All data columns (generation values, demand limits, costs, etc.) are stored
  as **float64**.

This ensures that Parquet files produced by `cvs2parquet` are directly
compatible with gtopt's Arrow-based table reader without any type-casting
issues at solve time.

---

## Installation

Install as part of the gtopt scripts package:

```bash
# Standard install
pip install ./scripts

# Editable install with dev dependencies
pip install -e "./scripts[dev]"
```

```bash
cvs2parquet --version
cvs2parquet --help
```

### Dependencies

| Package   | Purpose                      |
|-----------|------------------------------|
| `pandas`  | CSV reading and type casting |
| `pyarrow` | Parquet writing              |

---

## Quick Start

```bash
# Convert a single file (output name auto-derived: input.parquet)
cvs2parquet input.csv

# Convert a single file with explicit output path
cvs2parquet input.csv -o output.parquet

# Convert multiple files at once (each gets a .parquet sibling)
cvs2parquet Demand/lmax.csv Generator/pmax.csv Generator/pmin.csv

# Use an explicit PyArrow schema for strict type enforcement
cvs2parquet --schema input.csv -o output.parquet

# Verbose mode (print column dtype information)
cvs2parquet -v input.csv
```

---

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `INPUT` (positional) | — | One or more CSV files to convert (required) |
| `-o, --output FILE` | `<input>.parquet` | Output Parquet file path (only valid with a single input file) |
| `--schema` | off | Use an explicit PyArrow schema instead of pandas dtype casting |
| `-v, --verbose` | off | Print column dtype information after each conversion |
| `-V, --version` | — | Print version and exit |

> **Note**: The `-o` flag can only be used when converting a single input file.
> When converting multiple files, each CSV gets a `.parquet` file in the same
> directory with the same base name.

---

## Column Type Casting Rules

`cvs2parquet` applies the following type rules to every CSV file:

| Column name | Output type | Rationale |
|-------------|-------------|-----------|
| `stage` | `int32` | Planning stage index (integer) |
| `block` | `int32` | Time block index (integer) |
| `scenario` | `int32` | Scenario index (integer) |
| All other columns | `float64` | Numeric data values (MW, $/MWh, etc.) |

These rules match the expectations of gtopt's `csv_read_table` and
`parquet_read_table` functions in `array_index_traits.hpp`, which cast
index columns to `int32` and data columns to `double` when reading Arrow
tables.

---

## Schema Mode vs Default Mode

### Default mode (pandas dtype casting)

The default behaviour reads the CSV with pandas, casts the index columns to
`int32` and all other columns to `float64`, then writes to Parquet via
PyArrow.

```bash
cvs2parquet lmax.csv
```

### Schema mode (`--schema`)

With `--schema`, the tool constructs an explicit PyArrow schema before writing.
This provides stricter type enforcement and can catch type mismatches earlier:

```bash
cvs2parquet --schema lmax.csv
```

Both modes produce **identical output** for well-formed input files.  The
schema mode is useful when debugging type issues or when CSV files contain
unexpected string values in numeric columns.

---

## Example: Converting Demand Time-Series

Suppose you have a demand time-series CSV file `Demand/lmax.csv`:

```csv
scenario,stage,block,uid:1,uid:2,uid:3
1,1,1,150.0,100.0,90.0
1,1,2,145.0,95.0,85.0
1,1,3,160.0,110.0,95.0
...
```

Convert it to Parquet:

```bash
cvs2parquet Demand/lmax.csv
# → Demand/lmax.parquet (scenario/stage/block as int32, uid:* as float64)

# Verify the output
python -c "
import pyarrow.parquet as pq
t = pq.read_table('Demand/lmax.parquet')
print(t.schema)
print(t.to_pandas().head())
"
```

Output schema:

```
scenario: int32
stage: int32
block: int32
uid:1: double
uid:2: double
uid:3: double
```

### Batch conversion of an entire case

```bash
# Convert all CSV files in a case's input directory
cvs2parquet input/Demand/*.csv input/Generator/*.csv input/Line/*.csv -v
```

---

## See Also

- [SCRIPTS.md](../scripts-guide.md) — Overview of all gtopt Python scripts
- [INPUT\_DATA.md](../input-data.md) — gtopt input file format reference
- [ts2gtopt](ts2gtopt.md) — Project hourly time-series onto gtopt horizons
- [plp2gtopt](plp2gtopt.md) — Convert PLP cases (produces Parquet directly)
