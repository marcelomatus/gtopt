"""cvs2parquet – Convert CSV files to Apache Parquet format."""

import argparse
import sys
from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

# Columns that are always stored as int32 when present
_INT32_COLS = {"stage", "block", "scenario"}


def _infer_schema(df: pd.DataFrame) -> pa.Schema:
    """Build a PyArrow schema: int32 for index-like columns, float64 for the rest."""
    fields = []
    for col in df.columns:
        if col in _INT32_COLS:
            fields.append(pa.field(col, pa.int32()))
        else:
            fields.append(pa.field(col, pa.float64()))
    return pa.schema(fields)


def csv_to_parquet(
    csv_file_path, parquet_file_path, use_schema: bool = False, verbose: bool = False
):
    """Convert CSV to Parquet.

    Args:
        csv_file_path: Path to the input CSV file.
        parquet_file_path: Path for the output Parquet file.
        use_schema: When True use an explicit PyArrow schema (int32 for
            stage/block/scenario columns, float64 for everything else).
            When False use pandas dtype casting (same logic, via DataFrame).
        verbose: When True print column dtype information after conversion.
    """
    df = pd.read_csv(csv_file_path)

    if use_schema:
        schema = _infer_schema(df)
        table = pa.Table.from_pandas(df, schema=schema, preserve_index=False)
        pq.write_table(table, parquet_file_path)
        if verbose:
            print(f"Schema: {table.schema}")
    else:
        for col in df.columns:
            if col in _INT32_COLS:
                df[col] = df[col].astype("int32")
            else:
                df[col] = df[col].astype("float64")
        df.to_parquet(parquet_file_path, index=False)
        if verbose:
            print(f"Data types:\n{df.dtypes}")

    print(f"Converted {csv_file_path} → {parquet_file_path}")


_EPILOG = """
column type rules:
  Columns named 'stage', 'block', or 'scenario' are stored as int32.
  All other columns are stored as float64.

examples:
  # Convert a single CSV file (output: data.parquet)
  cvs2parquet data.csv

  # Specify the output path explicitly
  cvs2parquet data.csv -o /tmp/out.parquet

  # Convert several files at once
  cvs2parquet stage1.csv stage2.csv stage3.csv

  # Use an explicit PyArrow schema and show column types
  cvs2parquet data.csv --schema --verbose
"""


def main():
    """CLI entry point: convert one or more CSV files to Parquet."""
    parser = argparse.ArgumentParser(
        prog="cvs2parquet",
        description=(
            "Convert CSV files to Apache Parquet format.\n\n"
            "Index-like columns (stage, block, scenario) are stored as int32;\n"
            "all other columns are stored as float64."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "input",
        nargs="+",
        metavar="CSV",
        help="input CSV file(s) to convert",
    )
    parser.add_argument(
        "-o",
        "--output",
        metavar="PARQUET",
        help="output Parquet file path (only valid with a single input file)",
    )
    parser.add_argument(
        "--schema",
        action="store_true",
        default=False,
        help=(
            "use an explicit PyArrow schema instead of pandas dtype casting "
            "(produces identical types; useful when strict schema validation is needed)"
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        default=False,
        help="print column dtype information after each conversion",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    args = parser.parse_args()

    if args.output and len(args.input) > 1:
        parser.error("--output can only be used with a single input file")

    for csv_path in args.input:
        out_path = (
            args.output if args.output else str(Path(csv_path).with_suffix(".parquet"))
        )
        csv_to_parquet(csv_path, out_path, use_schema=args.schema, verbose=args.verbose)

    return 0


if __name__ == "__main__":
    sys.exit(main())
