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

from igtopt.template_builder import (
    _find_repo_root,
    _build_workbook,
    _list_sheets,
)

warnings.filterwarnings("ignore", category=DeprecationWarning)

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

# Sheets that belong to the ``simulation`` section of the gtopt JSON schema.
# These must match the fields declared in ``json_simulation.hpp``.
_SIMULATION_SHEETS = frozenset(
    {
        "block_array",
        "stage_array",
        "scenario_array",
        "phase_array",
        "scene_array",
    }
)

# Sheets that belong to the ``system`` section of the gtopt JSON schema.
# These must match the fields declared in ``json_system.hpp``.
_SYSTEM_SHEETS = frozenset(
    {
        "bus_array",
        "demand_array",
        "generator_array",
        "line_array",
        "generator_profile_array",
        "demand_profile_array",
        "battery_array",
        "converter_array",
        "reserve_zone_array",
        "reserve_provision_array",
        "junction_array",
        "waterway_array",
        "flow_array",
        "reservoir_array",
        "filtration_array",
        "turbine_array",
        "reservoir_efficiency_array",
        "user_constraint_array",
    }
)

expected_sheets = (
    ["options", "boundary_cuts"] + sorted(_SIMULATION_SHEETS) + sorted(_SYSTEM_SHEETS)
)

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

  # Validate the workbook without writing any output (check-only mode)
  igtopt system.xlsx --validate

  # Proceed even if some sheets fail to convert (partial output)
  igtopt system.xlsx --ignore-errors

  # Show debug log messages
  igtopt system.xlsx -l DEBUG

  # Generate the igtopt Excel template from C++ JSON headers
  igtopt --make-template

  # Write template to a custom path
  igtopt --make-template -j /tmp/my_template.xlsx

  # List sheets derived from C++ headers (no file written)
  igtopt --make-template --list-sheets
"""


def _array_name_to_cname(array_name: str) -> str:
    """Convert a snake_case_array sheet name to its CamelCase element type.

    Examples
    --------
    >>> _array_name_to_cname("demand_array")
    'Demand'
    >>> _array_name_to_cname("generator_profile_array")
    'GeneratorProfile'
    """
    if array_name.endswith("_array"):
        base = array_name[:-6]  # strip "_array"
    else:
        base = array_name
    # Convert snake_case to CamelCase
    camel = re.sub(r"_([a-z])", lambda m: m.group(1).upper(), base)
    return camel[0].upper() + camel[1:] if camel else camel


def _build_name_to_uid_maps(xls: dict) -> dict[str, dict[str, str]]:
    """Build element-name → ``uid:N`` column-name mappings from all array sheets.

    Iterates every sheet that is not a dot-sheet, not an ``@``-sheet, and not
    the ``options`` sheet.  For sheets whose DataFrame has both ``uid`` and
    ``name`` columns, it builds a mapping ``{element_name: "uid:<uid>"}`` and
    stores it under the corresponding CamelCase type key (e.g. ``"Demand"``).

    This mapping is used by :func:`_resolve_at_sheet_columns` to rename
    human-readable element-name columns (e.g. ``"LAJA"``) back to the
    ``uid:N`` format expected by the gtopt C++ binary.

    Args:
        xls: Dict of ``{sheet_name: DataFrame}`` from ``pd.read_excel()``.

    Returns:
        Dict mapping CamelCase type names to ``{element_name: "uid:N"}`` maps.
        Only types for which a non-empty mapping could be built are included.
    """
    result: dict[str, dict[str, str]] = {}
    for sheet_name, df in xls.items():
        if sheet_name.startswith(".") or "@" in sheet_name or sheet_name == "options":
            continue
        if "uid" not in df.columns or "name" not in df.columns:
            continue
        cname = _array_name_to_cname(sheet_name)
        name_map: dict[str, str] = {}
        collision = False
        for _, row in df.iterrows():
            try:
                uid = int(row["uid"])
                name = str(row["name"])
                if name and name != "nan":
                    uid_col = f"uid:{uid}"
                    if name in name_map and name_map[name] != uid_col:
                        # Two elements share a name – disable resolution for
                        # this entire type to avoid ambiguous column mapping.
                        collision = True
                        logging.warning(
                            "Duplicate element name '%s' in sheet '%s'; "
                            "name-based column resolution disabled for this type.",
                            name,
                            sheet_name,
                        )
                        break
                    name_map[name] = uid_col
            except (ValueError, TypeError):
                pass
        if name_map and not collision:
            result[cname] = name_map
    return result


def _resolve_at_sheet_columns(
    df: pd.DataFrame,
    name_to_uid: dict[str, str],
) -> pd.DataFrame:
    """Rename element-name columns to ``uid:N`` format in a time-series DataFrame.

    Index-like columns (``scenario``, ``stage``, ``block``) are left unchanged.
    Columns already in ``uid:N`` format are also left unchanged.  Any column
    whose name matches a key in *name_to_uid* is renamed to the corresponding
    ``uid:N`` value.

    Args:
        df:           DataFrame from an ``@``-sheet (e.g. ``Demand@lmax``).
        name_to_uid:  Mapping ``{element_name: "uid:N"}`` for the element type.

    Returns:
        DataFrame with element-name columns renamed to ``uid:N`` format.
        The original DataFrame is not modified (a copy is returned only when
        renaming is actually needed).
    """
    _INDEX_COLS = frozenset({"scenario", "stage", "block"})
    rename_map: dict[str, str] = {}
    for col in df.columns:
        if col in _INDEX_COLS:
            continue
        if col.startswith("uid:"):
            continue  # already in uid:N format
        target = name_to_uid.get(col)
        if target is not None:
            rename_map[col] = target
    if rename_map:
        logging.info(
            "Resolving element names to uid columns: %s",
            ", ".join(f"{k}→{v}" for k, v in rename_map.items()),
        )
        return df.rename(columns=rename_map)
    return df


def _write_boundary_cuts_csv(df, input_path):
    """Write a ``boundary_cuts`` Excel sheet to a CSV file.

    The sheet is expected to have columns ``name``, ``scenario``, ``rhs``,
    followed by one column per state variable (reservoir/junction name).

    The CSV is written to ``<input_path>/boundary_cuts.csv`` and the
    returned path should be stored in the ``sddp_boundary_cuts_file``
    option so the C++ solver can load it.
    """
    out_dir = pathlib.Path(input_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "boundary_cuts.csv"
    df.to_csv(csv_path, index=False)
    return csv_path


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


def _try_parse_json(value):
    """Try to parse a string as JSON; return the parsed value or the original string.

    This handles Excel cells whose content is a JSON-encoded scalar, list, or
    object (e.g. ``[[55.0]]``, ``[1, 2, 3]``, ``true``).  Plain strings that
    are not valid JSON are returned unchanged.
    """
    if not isinstance(value, str):
        return value
    stripped = value.strip()
    # Only attempt parsing if the string looks like a JSON literal
    if (
        not stripped
        or stripped[0] not in ("{", "[", "t", "f", "n", "-")
        and not (stripped[0].isdigit())
    ):
        return value
    try:
        return json.loads(stripped)
    except (json.JSONDecodeError, ValueError):
        return value


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
        records = []
        for _, row in df.iterrows():
            rec = {}
            for col, val in row.dropna().items():
                rec[col] = _try_parse_json(val)
            records.append(rec)
        return json.dumps(records, indent=indent, separators=separators)
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
    logging.info("  Batteries       : %d", counts.get("battery_array", 0))
    logging.info("  Converters      : %d", counts.get("converter_array", 0))
    logging.info("  Junctions       : %d", counts.get("junction_array", 0))
    logging.info("  Waterways       : %d", counts.get("waterway_array", 0))
    logging.info("  Reservoirs      : %d", counts.get("reservoir_array", 0))
    logging.info("  Turbines        : %d", counts.get("turbine_array", 0))
    logging.info("  Filtrations     : %d", counts.get("filtration_array", 0))
    logging.info("  Res. effics     : %d", counts.get("reservoir_efficiency_array", 0))
    logging.info("  User constraints: %d", counts.get("user_constraint_array", 0))
    logging.info("=== Simulation statistics ===")
    logging.info("  Blocks          : %d", counts.get("block_array", 0))
    logging.info("  Stages          : %d", counts.get("stage_array", 0))
    logging.info("  Scenarios       : %d", counts.get("scenario_array", 0))
    logging.info("  Phases          : %d", counts.get("phase_array", 0))
    logging.info("  Scenes          : %d", counts.get("scene_array", 0))
    logging.info("=== Key options ===")
    logging.info("  use_single_bus  : %s", options.get("use_single_bus", False))
    logging.info("  scale_objective : %s", options.get("scale_objective", 1000))
    logging.info("  demand_fail_cost: %s", options.get("demand_fail_cost", 1000))
    logging.info("  input_directory : %s", options.get("input_directory", ""))
    logging.info("=== Conversion time ===")
    logging.info("  Elapsed         : %.3fs", elapsed)


# ---------------------------------------------------------------------------
# Template generation (igtopt --make-template)
# ---------------------------------------------------------------------------


# JSON-type category strings shown in help rows
def _run(args) -> int:
    t_start = time.monotonic()

    # Use local formatting variables so multiple calls are independent.
    pretty = getattr(args, "pretty", False)
    _indent = _PRETTY_INDENT if pretty else _COMPACT_INDENT
    _separators = _PRETTY_SEPARATORS if pretty else _COMPACT_SEPARATORS

    options: dict[str, Any] = {}
    options["input_directory"] = str(args.input_directory)
    options["input_format"] = args.input_format

    # Collect simulation and system arrays as ordered dicts so we can write
    # the nested {options, simulation, system} structure that gtopt expects.
    simulation: dict[str, Any] = {}
    system: dict[str, Any] = {"name": args.name}

    json_path = args.json_file.with_suffix(".json")
    filenames = args.filenames
    # Pre-initialise with zeros so log_conversion_stats always has every key.
    counts: dict[str, int] = {s: 0 for s in expected_sheets if s != "options"}
    errors: list[str] = []
    for filename in filenames:
        filepath = pathlib.Path(filename)
        if filepath.is_dir() or not filepath.exists():
            filepath = filepath.with_suffix(".xlsx")
        if not filepath.exists():
            msg = f"file not found: {filepath}"
            logging.error(msg)
            errors.append(msg)
            continue

        suffix = filepath.suffix.lower()
        if suffix not in {".xlsx", ".xls", ".xlsm", ".ods"}:
            msg = (
                f"unsupported file type '{suffix}' for '{filepath}'. "
                "Expected an Excel workbook (.xlsx, .xls, .xlsm) or ODS file."
            )
            logging.error(msg)
            errors.append(msg)
            continue

        try:
            xls = pd.read_excel(str(filepath), sheet_name=None, engine="openpyxl")
        except FileNotFoundError:
            msg = f"file not found while opening '{filepath}' (may have been removed)"
            logging.error(msg)
            errors.append(msg)
            continue
        except PermissionError as exc:
            msg = f"permission denied reading '{filepath}': {exc}"
            logging.error(msg)
            errors.append(msg)
            continue
        except Exception as exc:  # pylint: disable=broad-except
            msg = (
                f"could not read '{filepath}': {exc}. "
                "Check that the file is a valid Excel workbook and is not "
                "open in another application."
            )
            logging.error(msg)
            errors.append(msg)
            continue

        # Build element-name → uid:N maps from array sheets in this workbook.
        # Used below to resolve human-readable column names in @-sheets.
        name_to_uid_maps = _build_name_to_uid_maps(xls)
        for sheet_name, df in xls.items():
            if not sheet_name or sheet_name[0] == ".":
                logging.info("skipping sheet %s", sheet_name)
                continue

            if "@" in sheet_name:
                parts = sheet_name.split("@", 1)
                if not parts[0].strip() or not parts[1].strip():
                    logging.warning(
                        "ignoring malformed '@'-sheet name '%s' "
                        "(expected 'ElementType@field', e.g. 'Demand@lmax')",
                        sheet_name,
                    )
                    continue
                cname, fname = parts[0].strip(), parts[1].strip()
                # Resolve element-name columns (e.g. "LAJA") to uid:N format
                # (e.g. "uid:5") if a mapping is available for this type.
                resolved_df = _resolve_at_sheet_columns(
                    df, name_to_uid_maps.get(cname, {})
                )
                try:
                    input_file = df_to_file(
                        resolved_df,
                        args.input_directory,
                        cname,
                        fname,
                        args.input_format,
                        args.compression,
                    )
                    logging.info("sheet %s saved as %s", sheet_name, input_file)
                except Exception as exc:  # pylint: disable=broad-except
                    msg = f"could not write data file for sheet '{sheet_name}': {exc}"
                    logging.error(msg)
                    errors.append(msg)
                continue

            if sheet_name in expected_sheets:
                logging.info("processing sheet %s", sheet_name)
            else:
                if not args.parse_unexpected_sheets:
                    logging.warning(
                        "skipping unexpected sheet '%s'. "
                        "Known sheets: %s. "
                        "Use -U/--parse-unexpected-sheets to process it anyway.",
                        sheet_name,
                        ", ".join(sorted(expected_sheets)),
                    )
                    continue

                logging.warning("processing unexpected sheet %s", sheet_name)

            if sheet_name == "options":
                options = df_to_opts(df, options)
                continue

            if sheet_name == "boundary_cuts":
                try:
                    csv_path = _write_boundary_cuts_csv(df, args.input_directory)
                    # Place inside sddp_options so the C++ parser maps it
                    # to Options::sddp_options::sddp_boundary_cuts_file.
                    sddp_opts = options.setdefault("sddp_options", {})
                    if isinstance(sddp_opts, dict):
                        sddp_opts["sddp_boundary_cuts_file"] = str(csv_path)
                    else:
                        options["sddp_boundary_cuts_file"] = str(csv_path)
                    logging.info(
                        "boundary_cuts sheet → %s (%d cuts)", csv_path, len(df)
                    )
                except Exception as exc:  # pylint: disable=broad-except
                    msg = f"could not write boundary_cuts: {exc}"
                    logging.error(msg)
                    errors.append(msg)
                continue

            try:
                df_parsed = json.loads(
                    df_to_str(
                        df, args.skip_nulls, indent=_indent, separators=_separators
                    )
                )
            except Exception as exc:  # pylint: disable=broad-except
                msg = f"could not parse sheet '{sheet_name}': {exc}"
                logging.error(msg)
                errors.append(msg)
                continue

            counts[sheet_name] = len(df_parsed)

            if sheet_name in _SIMULATION_SHEETS:
                simulation[sheet_name] = df_parsed
            else:
                system[sheet_name] = df_parsed

    # In validate mode, report findings and exit without writing any output.
    if getattr(args, "validate", False):
        if errors:
            logging.error(
                "Validation found %d error(s). The workbook would NOT produce "
                "a valid gtopt input file.",
                len(errors),
            )
            for err in errors:
                logging.error("  • %s", err)
            return 1
        logging.info(
            "Validation passed. The workbook can be converted to a valid "
            "gtopt input file."
        )
        log_conversion_stats(counts, options, time.monotonic() - t_start)
        return 0

    if errors and not getattr(args, "ignore_errors", False):
        logging.error(
            "%d error(s) occurred during conversion. "
            "Use --ignore-errors to proceed despite errors.",
            len(errors),
        )
        return 1

    has_data = len(simulation) > 0 or len(system) > 1  # system always has "name"
    if has_data:
        planning: dict[str, Any] = {}
        planning["options"] = options
        if simulation:
            planning["simulation"] = simulation
        planning["system"] = system

        with json_path.open("w") as json_file:
            json.dump(
                planning,
                json_file,
                indent=_indent if pretty else None,
                separators=None if pretty else _separators,
            )
            json_file.write("\n")

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


def main(argv: list[str] | None = None) -> None:
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
            nargs="*",  # changed from "+" to allow --make-template with no XLSX
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
            "--validate",
            action=argparse.BooleanOptionalAction,
            default=False,
            help=(
                "validate the workbook (check for errors) without writing any output "
                "files; exits with code 0 on success, 1 on error"
            ),
        )
        parser.add_argument(
            "--ignore-errors",
            action=argparse.BooleanOptionalAction,
            default=False,
            help=(
                "proceed with conversion even if some sheets or data files have "
                "errors; the output may be incomplete (default: abort on first error)"
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
        # Template generation options
        parser.add_argument(
            "-T",
            "--make-template",
            action="store_true",
            default=False,
            help=(
                "generate the gtopt igtopt Excel template from C++ JSON headers "
                "instead of converting a workbook; see also --header-dir, --list-sheets"
            ),
        )
        parser.add_argument(
            "--header-dir",
            metavar="DIR",
            help=(
                "path to include/gtopt/ (auto-detected from the repo root if omitted); "
                "only used with --make-template"
            ),
        )
        parser.add_argument(
            "--list-sheets",
            action="store_true",
            default=False,
            help=(
                "print the list of sheets derived from C++ headers and exit; "
                "only used with --make-template"
            ),
        )

        args = parser.parse_args(argv)

        logging.basicConfig(
            level=getattr(logging, args.log_level),
            format="%(asctime)s %(levelname)s %(message)s",
        )

        # --make-template mode: generate the Excel template and exit
        if args.make_template or args.list_sheets:
            repo_root = _find_repo_root(pathlib.Path(__file__).parent)
            if args.header_dir:
                header_dir = pathlib.Path(args.header_dir)
            else:
                header_dir = repo_root / "include" / "gtopt"

            if not header_dir.is_dir():
                logging.error(
                    "header directory '%s' not found; "
                    "use --header-dir to specify the path to include/gtopt/",
                    header_dir,
                )
                sys.exit(1)

            if args.list_sheets:
                _list_sheets(header_dir)
                return

            if args.json_file:
                output_path = pathlib.Path(args.json_file)
            else:
                output_path = repo_root / "docs" / "templates" / "gtopt_template.xlsx"

            output_path.parent.mkdir(parents=True, exist_ok=True)
            _build_workbook(output_path, header_dir)
            logging.info("Template written to %s", output_path)
            return

        # Normal conversion mode: require at least one XLSX file
        if not args.filenames:
            parser.error("at least one XLSX file is required (or use --make-template)")

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
