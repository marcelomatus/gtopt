"""cvs2parquet – Convert CSV files to Apache Parquet format."""

import argparse
import sys
from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq


def csv_to_parquet(csv_file_path, parquet_file_path):
    """Convert CSV to Parquet with specific column types."""
    # Read CSV file
    df = pd.read_csv(csv_file_path)

    # Convert column types
    df["stage"] = df["stage"].astype("int32")
    df["block"] = df["block"].astype("int32")
    df["g1"] = df["g1"].astype("float64")  # double precision

    # Save as Parquet
    df.to_parquet(parquet_file_path, index=False)

    print(f"Successfully converted {csv_file_path} to {parquet_file_path}")
    print(f"Data types: {df.dtypes}")


# Alternative method using PyArrow for more control over schema


def csv_to_parquet_with_schema(csv_file_path, parquet_file_path):
    """Convert CSV to Parquet using PyArrow with explicit schema definition."""
    # Read CSV
    df = pd.read_csv(csv_file_path)

    # Define explicit schema
    schema = pa.schema(
        [("stage", pa.int32()), ("block", pa.int32()), ("g1", pa.float64())]
    )

    # Convert to PyArrow table with schema
    table = pa.Table.from_pandas(df[["stage", "block", "g1"]], schema=schema)

    # Write to Parquet
    pq.write_table(table, parquet_file_path)

    print(f"Successfully converted {csv_file_path} to {parquet_file_path}")
    print(f"Schema: {table.schema}")


def main():
    """CLI entry point: convert one or more CSV files to Parquet."""
    parser = argparse.ArgumentParser(
        description="Convert CSV files to Apache Parquet format"
    )
    parser.add_argument(
        "input",
        nargs="+",
        help="CSV file(s) to convert",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="output Parquet file path (only valid with a single input file)",
    )
    parser.add_argument(
        "--schema",
        action="store_true",
        default=False,
        help="use explicit PyArrow schema (stage:int32, block:int32, g1:float64)",
    )
    args = parser.parse_args()

    if args.output and len(args.input) > 1:
        parser.error("--output can only be used with a single input file")

    for csv_path in args.input:
        out_path = args.output if args.output else str(Path(csv_path).with_suffix(".parquet"))
        if args.schema:
            csv_to_parquet_with_schema(csv_path, out_path)
        else:
            csv_to_parquet(csv_path, out_path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
