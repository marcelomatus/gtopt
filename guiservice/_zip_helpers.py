"""ZIP / case JSON build + parse helpers for the gtopt GUI service.

Exports:

* ``_sanitize_value`` — coerce a single value to a JSON-safe type.
* ``_df_to_rows``     — convert a Parquet/CSV DataFrame to GUI rows.
* ``_build_case_json``— stitch options + system + simulation + time-series.
* ``_build_zip``      — serialise a case dict + assets into a ZIP.
* ``_parse_uploaded_zip`` — round-trip a ZIP back into the case dict.
* ``_parse_results_zip``  — extract results parquet/csv → JSON-friendly dict.

Imported by :mod:`guiservice.app` and re-exported for back-compat with
existing tests (``from guiservice.app import _build_case_json``).
"""

from __future__ import annotations

import csv
import gzip
import io
import json
import logging
import math
import os
import zipfile
from base64 import b64decode, b64encode
from typing import Any

import pandas as pd

from guiservice._schemas import ELEMENT_TO_ARRAY_KEY

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------


def _sanitize_value(x):
    """Convert a single value to a JSON-safe Python type.

    Numpy scalars are converted via ``.item()``.  Float ``NaN`` and
    ``Inf`` values become ``None`` so they serialize as JSON ``null``
    instead of the non-standard ``NaN`` / ``Infinity`` tokens.
    """
    val = x.item() if hasattr(x, "item") else x
    if isinstance(val, float) and not math.isfinite(val):
        return None
    return val


def _df_to_rows(df):
    """Convert a DataFrame to a list of rows preserving column types.

    Using ``df.values.tolist()`` coerces all columns to a single numpy
    dtype (e.g. float64 when int and float columns are mixed), which
    turns integer values into floats.  Converting per-row via
    ``itertuples`` keeps each value in its native Python type.

    Float ``NaN`` and ``Inf`` values are replaced with ``None`` to
    produce valid JSON (the JSON specification does not allow ``NaN``).
    """
    return [[_sanitize_value(x) for x in row] for row in df.itertuples(index=False, name=None)]


def _build_case_json(case_data):
    """Build the gtopt-compatible JSON from the GUI case data."""
    options = case_data.get("options", {})
    simulation = case_data.get("simulation", {})
    system_elements = case_data.get("system", {})

    system_section = {"name": case_data.get("case_name", "case")}
    for elem_type, array_key in ELEMENT_TO_ARRAY_KEY.items():
        items = system_elements.get(elem_type, [])
        if items:
            cleaned = []
            for item in items:
                entry = {}
                for k, v in item.items():
                    if v is None or v == "":
                        continue
                    entry[k] = v
                cleaned.append(entry)
            system_section[array_key] = cleaned

    result = {}
    if options:
        result["options"] = options
    if simulation:
        result["simulation"] = simulation
    result["system"] = system_section
    return result


def _build_zip(case_data):
    """Build a ZIP file containing the full case configuration.

    This is used when creating cases from the GUI editor.  When a case
    was loaded via upload, ``submit_solve`` forwards the original ZIP
    instead of calling this function.
    """
    case_name = case_data.get("case_name", "case")
    case_json = _build_case_json(case_data)

    input_dir = case_data.get("options", {}).get("input_directory", case_name)
    input_format = case_data.get("options", {}).get("input_format", "csv")

    # Ensure the JSON has input_directory matching the data file prefix
    # so that gtopt can find the data files after zip extraction.
    if "options" not in case_json:
        case_json["options"] = {}
    case_json["options"]["input_directory"] = input_dir

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        # Main JSON config
        zf.writestr(
            f"{case_name}.json",
            json.dumps(case_json, indent=2),
        )

        # External data files
        data_files = case_data.get("data_files", {})
        for file_path, file_content in data_files.items():
            if input_format == "parquet":
                raw_b64 = file_content.get("_raw_parquet_b64")
                if raw_b64:
                    zf.writestr(
                        f"{input_dir}/{file_path}.parquet",
                        b64decode(raw_b64),
                    )
                else:
                    df = pd.DataFrame(file_content["data"], columns=file_content["columns"])
                    parquet_buf = io.BytesIO()
                    df.to_parquet(parquet_buf, index=False)
                    zf.writestr(
                        f"{input_dir}/{file_path}.parquet",
                        parquet_buf.getvalue(),
                    )
            else:
                output = io.StringIO()
                writer = csv.writer(output)
                writer.writerow(file_content["columns"])
                for row in file_content["data"]:
                    writer.writerow(row)
                zf.writestr(
                    f"{input_dir}/{file_path}.csv",
                    output.getvalue(),
                )

    buf.seek(0)
    return buf


def _parse_uploaded_zip(zip_bytes):
    """Parse an uploaded ZIP file into case data structure.

    The original ZIP bytes are preserved (base64-encoded) so that
    ``submit_solve`` can forward them to the webservice unchanged.
    """
    case_data: dict[str, Any] = {
        "case_name": "uploaded_case",
        "options": {},
        "simulation": {},
        "system": {},
        "data_files": {},
    }

    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        json_files = [n for n in zf.namelist() if n.endswith(".json")]
        if not json_files:
            return case_data

        # Parse the main JSON
        main_json_name = json_files[0]
        case_data["case_name"] = os.path.splitext(os.path.basename(main_json_name))[0]

        raw = json.loads(zf.read(main_json_name))
        case_data["options"] = raw.get("options", {})
        case_data["simulation"] = raw.get("simulation", {})

        # Parse system elements
        system_raw = raw.get("system", {})
        for elem_type, array_key in ELEMENT_TO_ARRAY_KEY.items():
            if array_key in system_raw:
                case_data["system"][elem_type] = system_raw[array_key]

        # Parse data files (CSV and Parquet) into JSON for GUI display;
        # original ZIP bytes are preserved separately for submission.
        for name in zf.namelist():
            if name.endswith(".csv") and not name.endswith(".json"):
                rel = name
                # Strip input_directory prefix if present
                input_dir = case_data["options"].get("input_directory", "")
                if input_dir and rel.startswith(input_dir + "/"):
                    rel = rel[len(input_dir) + 1 :]
                if rel.endswith(".csv"):
                    rel = rel[:-4]
                try:
                    content = zf.read(name).decode("utf-8")
                    reader = csv.reader(io.StringIO(content))
                    rows = list(reader)
                    if rows:
                        case_data["data_files"][rel] = {
                            "columns": rows[0],
                            "data": rows[1:],
                        }
                except Exception as e:  # pylint: disable=broad-exception-caught
                    logger.warning("Failed to parse CSV file %s: %s", name, e)

            elif name.endswith(".parquet"):
                rel = name
                input_dir = case_data["options"].get("input_directory", "")
                if input_dir and rel.startswith(input_dir + "/"):
                    rel = rel[len(input_dir) + 1 :]
                if rel.endswith(".parquet"):
                    rel = rel[:-8]
                try:
                    raw_bytes = zf.read(name)
                    df = pd.read_parquet(io.BytesIO(raw_bytes))
                    case_data["data_files"][rel] = {
                        "columns": list(df.columns),
                        "data": _df_to_rows(df),
                        "_raw_parquet_b64": b64encode(raw_bytes).decode("ascii"),
                    }
                except Exception as e:  # pylint: disable=broad-exception-caught
                    logger.warning("Failed to parse Parquet file %s: %s", name, e)

        # Store the system file name for passthrough submission
        case_data["_system_file"] = main_json_name

    # Preserve the original ZIP so submit can forward it unmodified
    case_data["_uploaded_zip_b64"] = b64encode(zip_bytes).decode("ascii")

    return case_data


def _parse_results_zip(zip_bytes):
    """Parse a results ZIP or directory structure into viewable data."""
    results: dict[str, Any] = {"solution": {}, "outputs": {}, "terminal_output": ""}

    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        for name in zf.namelist():
            # Collect terminal / log files so they can be shown in the GUI
            base = os.path.basename(name)
            if base in ("gtopt_terminal.log", "stdout.log", "stderr.log"):
                try:
                    content = zf.read(name).decode("utf-8", errors="replace")
                    if content.strip():
                        if results["terminal_output"]:
                            results["terminal_output"] += "\n"
                        results["terminal_output"] += content
                except Exception:
                    pass
                continue

            if name.endswith(".csv"):
                try:
                    content = zf.read(name).decode("utf-8")
                    reader = csv.reader(io.StringIO(content))
                    rows = list(reader)
                    parent = os.path.basename(os.path.dirname(name))

                    if base == "solution.csv":
                        for row in rows:
                            if len(row) >= 2:
                                results["solution"][row[0]] = row[1]
                    else:
                        key = f"{parent}/{base}" if parent else base
                        if rows:
                            results["outputs"][key] = {
                                "columns": rows[0],
                                "data": rows[1:],
                            }
                except Exception:
                    pass

            elif name.endswith(".csv.gz"):
                try:
                    raw = zf.read(name)
                    content = gzip.decompress(raw).decode("utf-8")
                    reader = csv.reader(io.StringIO(content))
                    rows = list(reader)
                    # Strip the .csv.gz suffix for display key
                    base_no_ext = base[: -len(".csv.gz")]
                    parent = os.path.basename(os.path.dirname(name))

                    if base_no_ext == "solution":
                        for row in rows:
                            if len(row) >= 2:
                                results["solution"][row[0]] = row[1]
                    else:
                        key = f"{parent}/{base_no_ext}" if parent else base_no_ext
                        if rows:
                            results["outputs"][key] = {
                                "columns": rows[0],
                                "data": rows[1:],
                            }
                except Exception:
                    pass

            elif name.endswith(".parquet"):
                try:
                    raw = zf.read(name)
                    df = pd.read_parquet(io.BytesIO(raw))
                    base_no_ext = os.path.splitext(os.path.basename(name))[0]
                    parent = os.path.basename(os.path.dirname(name))
                    key = f"{parent}/{base_no_ext}" if parent else base_no_ext
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": _df_to_rows(df),
                    }
                except Exception:
                    pass

    return results
