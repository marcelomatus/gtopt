#!/usr/bin/env python3

import argparse
import json
import logging
import pathlib
import re
import sys
import time
import warnings
import zipfile
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

_COMPACT_INDENT = 0
_COMPACT_SEPARATORS = (",", ":")

_PRETTY_INDENT = 4
_PRETTY_SEPARATORS = (", ", ": ")

# Module-level defaults kept for backwards-compatibility with external code that
# may read ``json_indent`` / ``json_separators`` directly.  New code should
# use the ``indent`` / ``separators`` parameters of :func:`df_to_str` instead,
# or rely on :func:`_run` which uses its own local copies derived from
# ``args.pretty`` so that multiple calls are fully independent.
json_indent = _COMPACT_INDENT  # pylint: disable=invalid-name
json_separators = _COMPACT_SEPARATORS  # pylint: disable=invalid-name

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

  # Bundle JSON + data files into a ZIP archive (ready for gtopt_guisrv/websrv)
  igtopt system.xlsx --zip

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


def df_to_str(
    df, skip_nulls=True, indent=_COMPACT_INDENT, separators=_COMPACT_SEPARATORS
):
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
            indent=indent,
            separators=separators,
        )
    return df.to_json(
        lines=False,
        orient="records",
        date_format="epoch",
        double_precision=10,
        force_ascii=True,
        date_unit="ms",
        default_handler=None,
        indent=indent,
    )


def create_zip_output(json_path: pathlib.Path, input_dir: pathlib.Path) -> pathlib.Path:
    """Create a ZIP archive containing the JSON file and all data files.

    The archive layout mirrors what gtopt_guisrv / gtopt_websrv expect:
    ``{case_name}.json`` at the root plus all files under the input directory.
    """
    zip_path = json_path.with_suffix(".zip")
    case_name = json_path.stem
    input_dir_name = input_dir.name

    logging.info("Creating ZIP archive: %s", zip_path)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(json_path, arcname=f"{case_name}.json")
        if input_dir.exists():
            for data_file in sorted(input_dir.rglob("*")):
                if data_file.is_file():
                    arcname = f"{input_dir_name}/{data_file.relative_to(input_dir)}"
                    zf.write(data_file, arcname=arcname)

    logging.info(
        "ZIP archive written: %s (%d bytes)",
        zip_path,
        zip_path.stat().st_size,
    )
    return zip_path


def log_conversion_stats(
    counts: dict[str, int],
    options: dict[str, Any],
    elapsed: float,
) -> None:
    """Log conversion statistics similar to plp2gtopt."""
    logging.info("=== System statistics ===")
    logging.info("  Buses           : %d", counts.get("bus_array", 0))
    logging.info("  Generators      : %d", counts.get("generator_array", 0))
    logging.info("  Generator profs : %d", counts.get("generator_profile_array", 0))
    logging.info("  Demands         : %d", counts.get("demand_array", 0))
    logging.info("  Lines           : %d", counts.get("line_array", 0))
    logging.info("  Batteries       : %d", counts.get("batterie_array", 0))
    logging.info("  Converters      : %d", counts.get("converter_array", 0))
    logging.info("  Junctions       : %d", counts.get("junction_array", 0))
    logging.info("  Reservoirs      : %d", counts.get("reservoir_array", 0))
    logging.info("  Turbines        : %d", counts.get("turbine_array", 0))
    logging.info("=== Simulation statistics ===")
    logging.info("  Blocks          : %d", counts.get("block_array", 0))
    logging.info("  Stages          : %d", counts.get("stage_array", 0))
    logging.info("  Scenarios       : %d", counts.get("scenario_array", 0))
    logging.info("=== Key options ===")
    logging.info("  use_single_bus  : %s", options.get("use_single_bus", False))
    logging.info("  scale_objective : %s", options.get("scale_objective", 1000))
    logging.info("  demand_fail_cost: %s", options.get("demand_fail_cost", 1000))
    logging.info("  input_directory : %s", options.get("input_directory", ""))
    logging.info("=== Conversion time ===")
    logging.info("  Elapsed         : %.3fs", elapsed)


def _run(args) -> int:
    t_start = time.monotonic()

    # Use local formatting variables so multiple calls are independent.
    pretty = getattr(args, "pretty", False)
    _indent = _PRETTY_INDENT if pretty else _COMPACT_INDENT
    _separators = _PRETTY_SEPARATORS if pretty else _COMPACT_SEPARATORS

    options: dict[str, Any] = {}
    options["input_directory"] = str(args.input_directory)
    options["input_format"] = args.input_format

    prelude = {}
    prelude["name"] = args.name

    pstr = json.dumps(prelude, separators=_separators)
    match = re.search(r"{(.*?)}$", pstr)
    pstr = match.group(1) if match is not None else pstr

    json_path = args.json_file.with_suffix(".json")
    json_file = None
    filenames = args.filenames
    # Pre-initialise with zeros so log_conversion_stats always has every key.
    counts: dict[str, int] = {s: 0 for s in expected_sheets if s != "options"}
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

            df_str = df_to_str(
                df, args.skip_nulls, indent=_indent, separators=_separators
            )
            counts[sheet_name] = len(df)

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
            opts_str = json.dumps(options, indent=_indent, separators=_separators)
            json_file.write(f',"options":{opts_str}\n')

        json_file.write("}\n")
        json_file.close()
        logging.info("gtopt input file %s was successfully generated", str(json_path))

        elapsed = time.monotonic() - t_start
        log_conversion_stats(counts, options, elapsed)

        if getattr(args, "zip", False):
            zip_path = create_zip_output(json_path, pathlib.Path(args.input_directory))
            print(f"ZIP archive created: {zip_path}")
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
            "-z",
            "--zip",
            action=argparse.BooleanOptionalAction,
            default=False,
            help=(
                "bundle the JSON file and all data files into a single ZIP archive "
                "(compatible with gtopt_guisrv and gtopt_websrv)"
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
