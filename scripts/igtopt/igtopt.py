#!/usr/bin/env python3

import argparse
import json
import logging
import pathlib
import re
import sys
import warnings
from typing import Any
from itertools import zip_longest

import numpy as np
import pandas as pd

warnings.filterwarnings("ignore", category=DeprecationWarning)

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

expected_sheets = [
    "options",
    "scenario_array",
    "stage_array",
    "block_array",
    "bus_array",
    "demand_array",
    "generator_array",
    "line_array",
    "generator_profile_array",
    "demand_profile_array",
    "batterie_array",
    "converter_array",
    "reserve_zone_array",
    "reserve_provision_array",
    "junction_array",
    "waterway_array",
    "flow_array",
    "outflow_array",
    "reservoir_array",
    "filtration_array",
    "turbine_array",
    "emission_zone_array",
    "generator_emission_array",
    "demand_emissions",
]

compact_indent = 0
compact_separators = separators = (",", ":")

pretty_indent = 4
pretty_separators = separators = (", ", ": ")

json_indent = compact_indent
json_separators = compact_separators

_LOG_LEVEL_CHOICES = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]


def split_in_columns(my_list):
    columns = 3
    result = []
    for first, second, third in zip_longest(
        my_list[0::columns], my_list[1::columns], my_list[2::columns], fillvalue=""
    ):
        result.append(f"  {first:<26}{second:<26}{third}")
    return "\n".join(result)


_DESCRIPTION = f"""\
Convert an Excel workbook to gtopt JSON input files.

Key features:
  - Sheets/columns whose name starts with "." (e.g. ".calc") are skipped
  - Sheets whose name contains "@" (e.g. "Demand@lmax") are written as
    time-series data files directly to the input directory
  - The "options" sheet populates the JSON options block
  - All other sheets become JSON arrays in the output file

Expected sheet names:
{split_in_columns(expected_sheets)}"""

_EPILOG = """
examples:
  # Basic conversion – output file and input directory derived from workbook name
  igtopt system_c0.xlsx

  # Explicit output JSON and input-data directory
  igtopt system.xlsx -j /tmp/system.json -d /tmp/system_input

  # Pretty-printed JSON, skip null values, parquet output
  igtopt system.xlsx --pretty --skip-nulls -f parquet

  # Convert multiple workbooks in one run
  igtopt case_a.xlsx case_b.xlsx -d /data/input

  # Show debug log messages
  igtopt system.xlsx -l DEBUG
"""


def df_to_file(df, input_path, cname, fname, input_format, compression):
    input_dir = pathlib.Path(input_path) / cname
    input_dir.mkdir(parents=True, exist_ok=True)
    input_file = input_dir / (fname + "." + input_format)

    types = {}
    btype = np.int32 if fname == "active" else np.float64
    for c in df.columns:
        types[c] = btype

    for c in ["scenario", "stage", "block"]:
        if c in df.columns:
            types[c] = np.int32

    df = df.astype(types)

    if input_format == "csv":
        df.to_csv(input_file, index=False)
    else:
        df.to_parquet(
            input_file,
            index=False,
            compression=compression if compression != "" else None,
        )

    return input_file


def df_to_opts(df, options):
    if "option" in df.columns and "value" in df.columns:
        opts = dict(zip(df.option, df.value))
        for key, value in options.items():
            opts[key] = value
        return opts

    logging.error(
        "'options' sheet requires both 'option' or 'value' columns, not found"
    )
    sys.exit(1)


def df_to_str(df, skip_nulls=True):
    """Convert a DataFrame to a JSON string representation."""
    dropc = []
    for c in df.columns:
        if c[0] == ".":
            dropc.append(c)
    df.drop(columns=dropc, inplace=True)

    types: dict[str, Any] = {}
    if "name" in df.columns:
        types["name"] = str
    for c in ["uid", "active"]:
        if c in df.columns:
            types[c] = np.int64
    df = df.astype(types)

    if skip_nulls:
        return json.dumps(
            list(df.agg(lambda x: x.dropna().to_dict(), axis=1)),
            indent=json_indent,
            separators=json_separators,
        )
    return df.to_json(
        lines=False,
        orient="records",
        date_format="epoch",
        double_precision=10,
        force_ascii=True,
        date_unit="ms",
        default_handler=None,
        indent=json_indent,
    )


def _run(args) -> int:
    options = {}
    options["input_directory"] = str(args.input_directory)
    options["input_format"] = args.input_format

    prelude = {}
    prelude["name"] = args.name

    pstr = json.dumps(prelude, separators=json_separators)
    match = re.search(r"{(.*?)}$", pstr)
    pstr = match.group(1) if match is not None else pstr

    json_path = args.json_file.with_suffix(".json")
    json_file = None
    filenames = args.filenames
    for filename in filenames:
        filepath = pathlib.Path(filename)
        if filepath.is_dir() or not filepath.exists():
            filepath = filepath.with_suffix(".xlsx")
        if not filepath.exists():
            logging.info("skipping not existing file %s", filepath)
            continue

        xls = pd.read_excel(str(filepath), sheet_name=None, engine="openpyxl")
        for sheet_name, df in xls.items():
            if sheet_name[0] == ".":
                logging.info("skipping sheet %s", sheet_name)
                continue

            if "@" in sheet_name:
                [cname, fname] = sheet_name.split("@")
                input_file = df_to_file(
                    df,
                    args.input_directory,
                    cname,
                    fname,
                    args.input_format,
                    args.compression,
                )
                logging.info("sheet %s saved as %s", sheet_name, input_file)

                continue

            if sheet_name in expected_sheets:
                logging.info("processing sheet %s", sheet_name)
            else:
                if not args.parse_unexpected_sheets:
                    logging.warning("skipping unexpected sheet %s", sheet_name)
                    continue

                logging.warning("processing unexpected sheet %s", sheet_name)

            if sheet_name == "options":
                options = df_to_opts(df, options)
                continue

            df_str = df_to_str(df, args.skip_nulls)

            if df_str is not None:
                if not json_file:
                    # lazy open the json_file
                    json_file = json_path.open("w")
                    json_file.write("{\n")
                    json_file.write(f"{pstr}\n")

                json_file.write(f',"{sheet_name}":')
                json_file.write(df_str)
                json_file.write("\n")

    if json_file:
        # close the json_file
        if options:
            json_file.write(
                f',"options":{json.dumps(options, indent=json_indent, separators=json_separators)}\n'
            )

        json_file.write("}\n")
        json_file.close()
        logging.info("gtopt input file %s was successfully generated", str(json_path))
    else:
        logging.warning(
            "no valid data was found, the file %s was not generated", str(json_path)
        )

    return 0


def main() -> None:
    """CLI entry point: parse arguments and run igtopt conversion."""
    try:
        parser = argparse.ArgumentParser(
            prog="igtopt",
            description=_DESCRIPTION,
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=_EPILOG,
        )
        parser.add_argument(
            dest="filenames",
            nargs="+",
            metavar="XLSX",
            help="Excel workbook(s) to convert to gtopt input data",
        )
        parser.add_argument(
            "-j",
            "--json-file",
            type=pathlib.Path,
            metavar="FILE",
            help=(
                "output JSON file (default: <first workbook stem>.json in the "
                "current directory)"
            ),
        )
        parser.add_argument(
            "-d",
            "--input-directory",
            type=pathlib.Path,
            metavar="DIR",
            help=(
                "directory for time-series data files written from '@'-sheets "
                "(default: <first workbook stem>/)"
            ),
        )
        parser.add_argument(
            "-f",
            "--input-format",
            choices=["csv", "parquet"],
            default="parquet",
            help="file format for time-series data files (default: %(default)s)",
        )
        parser.add_argument(
            "-n",
            "--name",
            metavar="NAME",
            help=(
                "system name written to the JSON output "
                "(default: <first workbook stem>)"
            ),
        )
        parser.add_argument(
            "-c",
            "--compression",
            default="gzip",
            metavar="ALG",
            help=(
                "compression algorithm for Parquet output files "
                "(default: %(default)s); pass '' to disable compression"
            ),
        )
        parser.add_argument(
            "-p",
            "--pretty",
            action=argparse.BooleanOptionalAction,
            default=False,
            help="write JSON with indented pretty-print formatting (default: compact)",
        )
        parser.add_argument(
            "-N",
            "--skip-nulls",
            action=argparse.BooleanOptionalAction,
            default=False,
            help="omit keys with null/NaN values from the JSON output (default: include)",
        )
        parser.add_argument(
            "-U",
            "--parse-unexpected-sheets",
            action=argparse.BooleanOptionalAction,
            default=False,
            help=(
                "also process sheets whose names are not in the expected list "
                "(default: warn and skip)"
            ),
        )
        parser.add_argument(
            "-l",
            "--log-level",
            default="INFO",
            choices=_LOG_LEVEL_CHOICES,
            metavar="LEVEL",
            help=(
                "logging verbosity: DEBUG, INFO, WARNING, ERROR, CRITICAL "
                "(default: %(default)s)"
            ),
        )
        parser.add_argument(
            "-V",
            "--version",
            action="version",
            version=f"%(prog)s {__version__}",
        )

        args = parser.parse_args()

        logging.basicConfig(
            level=getattr(logging, args.log_level),
            format="%(asctime)s %(levelname)s %(message)s",
        )

        if args.pretty:
            global json_indent, json_separators  # noqa: PLW0603
            json_indent = pretty_indent
            json_separators = pretty_separators

        if not args.json_file:
            args.json_file = pathlib.Path(args.filenames[0]).with_suffix(".json")
            logging.info("using json_file %s", args.json_file)

        if not args.input_directory:
            args.input_directory = pathlib.Path(args.filenames[0]).stem
            logging.info("using input_directory %s", args.input_directory)

        if not args.name:
            args.name = pathlib.Path(args.filenames[0]).stem
            logging.info("using system name %s", args.name)

        result = _run(args)
    except (IOError, ValueError, argparse.ArgumentError) as e:
        logging.error("unexpected error: %s", str(e))
        sys.exit(1)

    sys.exit(result)


if __name__ == "__main__":
    main()
