"""
gtopt GUI Service - Web application for creating, editing, and visualizing
gtopt optimization cases.

This module provides the Flask application with REST API endpoints for
case management and a browser-based GUI for interactive case creation.
"""

import csv
import gzip
import io
import json
import logging
import os
import signal
import tempfile
import zipfile
from base64 import b64decode, b64encode
from collections import deque
from typing import Any

import pandas as pd
import requests as http_requests
from flask import (
    Flask,
    jsonify,
    render_template,
    request,
    send_file,
)

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 100 * 1024 * 1024  # 100 MB

MAX_LOG_ENTRIES = 500
DEFAULT_LOG_LINES = 200
_recent_logs: deque[str] = deque(maxlen=MAX_LOG_ENTRIES)


class _RecentLogHandler(logging.Handler):
    """Keep recent logs in memory for GUI log viewer."""

    def emit(self, record):
        _recent_logs.append(self.format(record))


def _configure_logging():
    """Configure guiservice logging for file + in-memory viewing."""
    log_file = os.environ.get(
        "GTOPT_GUI_LOG_FILE",
        os.path.join(tempfile.gettempdir(), "gtopt_guiservice.log"),
    )
    formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    app.logger.setLevel(logging.INFO)

    if not any(isinstance(h, _RecentLogHandler) for h in app.logger.handlers):
        recent_handler = _RecentLogHandler()
        recent_handler.setFormatter(formatter)
        app.logger.addHandler(recent_handler)

    if not any(isinstance(h, logging.FileHandler) for h in app.logger.handlers):
        file_handler = logging.FileHandler(log_file)
        file_handler.setFormatter(formatter)
        app.logger.addHandler(file_handler)

    app.logger.info("guiservice logging initialized (log_file=%s)", log_file)


_configure_logging()

# Default webservice URL (can be overridden via env or API)
_webservice_url = os.environ.get("GTOPT_WEBSERVICE_URL", "http://localhost:3000")


# ---------------------------------------------------------------------------
# Schema definitions – mirrors the C++ JSON interfaces in include/gtopt/json/
# ---------------------------------------------------------------------------

ELEMENT_SCHEMAS = {
    "bus": {
        "label": "Bus",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "voltage", "type": "number", "required": False},
            {"name": "reference_theta", "type": "number", "required": False},
            {"name": "use_kirchhoff", "type": "boolean", "required": False},
        ],
    },
    "generator": {
        "label": "Generator",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "bus", "type": "string", "required": True, "ref": "bus"},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "pmin", "type": "number_or_file", "required": False},
            {"name": "pmax", "type": "number_or_file", "required": False},
            {"name": "gcost", "type": "number_or_file", "required": False},
            {"name": "lossfactor", "type": "number_or_file", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "expcap", "type": "number_or_file", "required": False},
            {"name": "expmod", "type": "number_or_file", "required": False},
            {"name": "capmax", "type": "number_or_file", "required": False},
            {"name": "annual_capcost", "type": "number_or_file", "required": False},
            {"name": "annual_derating", "type": "number_or_file", "required": False},
        ],
    },
    "demand": {
        "label": "Demand",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "bus", "type": "string", "required": True, "ref": "bus"},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "lmax", "type": "number_or_file", "required": False},
            {"name": "lossfactor", "type": "number_or_file", "required": False},
            {"name": "fcost", "type": "number_or_file", "required": False},
            {"name": "emin", "type": "number_or_file", "required": False},
            {"name": "ecost", "type": "number_or_file", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "expcap", "type": "number_or_file", "required": False},
            {"name": "expmod", "type": "number_or_file", "required": False},
            {"name": "capmax", "type": "number_or_file", "required": False},
            {"name": "annual_capcost", "type": "number_or_file", "required": False},
            {"name": "annual_derating", "type": "number_or_file", "required": False},
        ],
    },
    "line": {
        "label": "Line",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "bus_a", "type": "string", "required": True, "ref": "bus"},
            {"name": "bus_b", "type": "string", "required": True, "ref": "bus"},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "voltage", "type": "number_or_file", "required": False},
            {"name": "resistance", "type": "number_or_file", "required": False},
            {"name": "reactance", "type": "number_or_file", "required": False},
            {"name": "lossfactor", "type": "number_or_file", "required": False},
            {"name": "tmax_ab", "type": "number_or_file", "required": False},
            {"name": "tmax_ba", "type": "number_or_file", "required": False},
            {"name": "tcost", "type": "number_or_file", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "expcap", "type": "number_or_file", "required": False},
            {"name": "expmod", "type": "number_or_file", "required": False},
            {"name": "capmax", "type": "number_or_file", "required": False},
            {"name": "annual_capcost", "type": "number_or_file", "required": False},
            {"name": "annual_derating", "type": "number_or_file", "required": False},
        ],
    },
    "battery": {
        "label": "Battery",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "input_efficiency", "type": "number_or_file", "required": False},
            {"name": "output_efficiency", "type": "number_or_file", "required": False},
            {"name": "annual_loss", "type": "number_or_file", "required": False},
            {"name": "vmin", "type": "number_or_file", "required": False},
            {"name": "vmax", "type": "number_or_file", "required": False},
            {"name": "vcost", "type": "number_or_file", "required": False},
            {"name": "vini", "type": "number", "required": False},
            {"name": "vfin", "type": "number", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "expcap", "type": "number_or_file", "required": False},
            {"name": "expmod", "type": "number_or_file", "required": False},
            {"name": "capmax", "type": "number_or_file", "required": False},
            {"name": "annual_capcost", "type": "number_or_file", "required": False},
            {"name": "annual_derating", "type": "number_or_file", "required": False},
        ],
    },
    "converter": {
        "label": "Converter",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "battery", "type": "string", "required": True, "ref": "battery"},
            {"name": "generator", "type": "string", "required": True, "ref": "generator"},
            {"name": "demand", "type": "string", "required": True, "ref": "demand"},
            {"name": "conversion_rate", "type": "number_or_file", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "expcap", "type": "number_or_file", "required": False},
            {"name": "expmod", "type": "number_or_file", "required": False},
            {"name": "capmax", "type": "number_or_file", "required": False},
            {"name": "annual_capcost", "type": "number_or_file", "required": False},
            {"name": "annual_derating", "type": "number_or_file", "required": False},
        ],
    },
    "junction": {
        "label": "Junction",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "drain", "type": "boolean", "required": False},
        ],
    },
    "waterway": {
        "label": "Waterway",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "junction_a", "type": "string", "required": True, "ref": "junction"},
            {"name": "junction_b", "type": "string", "required": True, "ref": "junction"},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "lossfactor", "type": "number_or_file", "required": False},
            {"name": "fmin", "type": "number_or_file", "required": False},
            {"name": "fmax", "type": "number_or_file", "required": False},
        ],
    },
    "reservoir": {
        "label": "Reservoir",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "junction", "type": "string", "required": True, "ref": "junction"},
            {"name": "spillway_capacity", "type": "number", "required": False},
            {"name": "spillway_cost", "type": "number", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
            {"name": "annual_loss", "type": "number_or_file", "required": False},
            {"name": "vmin", "type": "number_or_file", "required": False},
            {"name": "vmax", "type": "number_or_file", "required": False},
            {"name": "vcost", "type": "number_or_file", "required": False},
            {"name": "vini", "type": "number", "required": False},
            {"name": "vfin", "type": "number", "required": False},
            {"name": "fmin", "type": "number", "required": False},
            {"name": "fmax", "type": "number", "required": False},
            {"name": "vol_scale", "type": "number", "required": False},
            {"name": "flow_conversion_rate", "type": "number", "required": False},
        ],
    },
    "turbine": {
        "label": "Turbine",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "waterway", "type": "string", "required": True, "ref": "waterway"},
            {"name": "generator", "type": "string", "required": True, "ref": "generator"},
            {"name": "drain", "type": "boolean", "required": False},
            {"name": "conversion_rate", "type": "number_or_file", "required": False},
            {"name": "capacity", "type": "number_or_file", "required": False},
        ],
    },
    "flow": {
        "label": "Flow (Inflow)",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "direction", "type": "integer", "required": False},
            {"name": "junction", "type": "string", "required": True, "ref": "junction"},
            {"name": "discharge", "type": "number_or_file", "required": True},
        ],
    },
    "filtration": {
        "label": "Filtration",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "waterway", "type": "string", "required": True, "ref": "waterway"},
            {"name": "reservoir", "type": "string", "required": True, "ref": "reservoir"},
            {"name": "slope", "type": "number", "required": False},
            {"name": "constant", "type": "number", "required": False},
        ],
    },
    "generator_profile": {
        "label": "Generator Profile",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "generator", "type": "string", "required": True, "ref": "generator"},
            {"name": "profile", "type": "number_or_file", "required": True},
            {"name": "scost", "type": "number_or_file", "required": False},
        ],
    },
    "demand_profile": {
        "label": "Demand Profile",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "demand", "type": "string", "required": True, "ref": "demand"},
            {"name": "profile", "type": "number_or_file", "required": True},
            {"name": "scost", "type": "number_or_file", "required": False},
        ],
    },
    "reserve_zone": {
        "label": "Reserve Zone",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "urreq", "type": "number_or_file", "required": False},
            {"name": "drreq", "type": "number_or_file", "required": False},
            {"name": "urcost", "type": "number_or_file", "required": False},
            {"name": "drcost", "type": "number_or_file", "required": False},
        ],
    },
    "reserve_provision": {
        "label": "Reserve Provision",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "generator", "type": "string", "required": True, "ref": "generator"},
            {"name": "reserve_zones", "type": "string", "required": True},
            {"name": "urmax", "type": "number_or_file", "required": False},
            {"name": "drmax", "type": "number_or_file", "required": False},
            {"name": "ur_capacity_factor", "type": "number_or_file", "required": False},
            {"name": "dr_capacity_factor", "type": "number_or_file", "required": False},
            {"name": "ur_provision_factor", "type": "number_or_file", "required": False},
            {"name": "dr_provision_factor", "type": "number_or_file", "required": False},
            {"name": "urcost", "type": "number_or_file", "required": False},
            {"name": "drcost", "type": "number_or_file", "required": False},
        ],
    },
}

# Map element type → JSON array key in the system section
ELEMENT_TO_ARRAY_KEY = {
    "bus": "bus_array",
    "generator": "generator_array",
    "demand": "demand_array",
    "line": "line_array",
    "battery": "battery_array",
    "converter": "converter_array",
    "junction": "junction_array",
    "waterway": "waterway_array",
    "reservoir": "reservoir_array",
    "turbine": "turbine_array",
    "flow": "flow_array",
    "filtration": "filtration_array",
    "generator_profile": "generator_profile_array",
    "demand_profile": "demand_profile_array",
    "reserve_zone": "reserve_zone_array",
    "reserve_provision": "reserve_provision_array",
}


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------


def _df_to_rows(df):
    """Convert a DataFrame to a list of rows preserving column types.

    Using ``df.values.tolist()`` coerces all columns to a single numpy
    dtype (e.g. float64 when int and float columns are mixed), which
    turns integer values into floats.  Converting per-row via
    ``itertuples`` keeps each value in its native Python type.
    """
    return [
        [x.item() if hasattr(x, "item") else x for x in row]
        for row in df.itertuples(index=False, name=None)
    ]


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
                    app.logger.warning("Failed to parse CSV file %s: %s", name, e)

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
                    app.logger.warning("Failed to parse Parquet file %s: %s", name, e)

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


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------


@app.route("/")
def index():
    """Serve the main GUI page."""
    return render_template("index.html")


@app.route("/api/schemas", methods=["GET"])
def get_schemas():
    """Return the element schemas for use by the GUI."""
    app.logger.debug("Serving schemas")
    return jsonify(ELEMENT_SCHEMAS)


@app.route("/api/logs", methods=["GET"])
def get_logs():
    """Return recent backend logs for GUI diagnostics."""
    lines = request.args.get("lines", default=DEFAULT_LOG_LINES, type=int)
    lines = max(1, min(lines, MAX_LOG_ENTRIES))
    return jsonify({"logs": list(_recent_logs)[-lines:]})


@app.route("/api/case/download", methods=["POST"])
def download_case():
    """Generate and download a ZIP file of the case configuration."""
    case_data = request.get_json()
    if not case_data:
        return jsonify({"error": "No case data provided"}), 400

    buf = _build_zip(case_data)
    case_name = case_data.get("case_name", "case")
    return send_file(
        buf,
        mimetype="application/zip",
        as_attachment=True,
        download_name=f"{case_name}.zip",
    )


@app.route("/api/case/upload", methods=["POST"])
def upload_case():
    """Upload a case ZIP file for editing."""
    if "file" not in request.files:
        return jsonify({"error": "No file uploaded"}), 400

    f = request.files["file"]
    if not f.filename:
        return jsonify({"error": "No file selected"}), 400

    if not f.filename.lower().endswith(".zip"):
        return jsonify({"error": "Only ZIP files are accepted"}), 400

    case_data = _parse_uploaded_zip(f.read())
    return jsonify(case_data)


@app.route("/api/case/preview", methods=["POST"])
def preview_case():
    """Preview the JSON that would be generated for the case."""
    case_data = request.get_json()
    if not case_data:
        return jsonify({"error": "No case data provided"}), 400

    case_json = _build_case_json(case_data)
    return jsonify(case_json)


@app.route("/api/results/upload", methods=["POST"])
def upload_results():
    """Upload results ZIP for visualization."""
    if "file" not in request.files:
        return jsonify({"error": "No file uploaded"}), 400

    f = request.files["file"]
    if not f.filename:
        return jsonify({"error": "No file selected"}), 400

    results = _parse_results_zip(f.read())
    return jsonify(results)


# ---------------------------------------------------------------------------
# Webservice integration routes
# ---------------------------------------------------------------------------


@app.route("/api/solve/config", methods=["GET"])
def get_solve_config():
    """Return the current webservice URL configuration."""
    return jsonify({"webservice_url": _webservice_url})


@app.route("/api/solve/config", methods=["POST"])
def set_solve_config():
    """Update the webservice URL at runtime."""
    global _webservice_url
    data = request.get_json()
    if not data or "webservice_url" not in data:
        return jsonify({"error": "webservice_url is required"}), 400
    url = data["webservice_url"].rstrip("/")
    if not url:
        return jsonify({"error": "webservice_url cannot be empty"}), 400
    _webservice_url = url
    return jsonify({"webservice_url": _webservice_url})


@app.route("/api/solve/submit", methods=["POST"])
def submit_solve():
    """Submit a case to the gtopt webservice for solving.

    When the case was loaded via ``/api/case/upload``, the original ZIP
    is forwarded unchanged (passthrough).  Otherwise a new ZIP is built
    from the JSON case data.

    The webservice expects POST /api/jobs with multipart form data:
      - file: the ZIP archive
      - systemFile: name of the system JSON inside the archive
    It returns {"token": "...", "status": "pending", "message": "..."}.
    """
    case_data = request.get_json()
    if not case_data:
        return jsonify({"error": "No case data provided"}), 400

    case_name = case_data.get("case_name", "case")

    # Prefer the original uploaded ZIP (passthrough, no modifications)
    uploaded_zip_b64 = case_data.get("_uploaded_zip_b64")
    if uploaded_zip_b64:
        zip_buf = io.BytesIO(b64decode(uploaded_zip_b64))
        system_file = case_data.get("_system_file", f"{case_name}.json")
    else:
        zip_buf = _build_zip(case_data)
        system_file = f"{case_name}.json"

    # Forward to webservice POST /api/jobs
    try:
        resp = http_requests.post(
            f"{_webservice_url}/api/jobs",
            files={"file": (f"{case_name}.zip", zip_buf, "application/zip")},
            data={"systemFile": system_file},
            timeout=60,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on submit: %s", _webservice_url)
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on submit")
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        body = ""
        if e.response is not None:
            try:
                body = e.response.json().get("error", e.response.text)
            except Exception:
                body = e.response.text
        app.logger.warning("Webservice HTTP error on submit: %s", body)
        return jsonify({"error": f"Webservice error ({e.response.status_code}): {body}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on submit")
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/status/<token>", methods=["GET"])
def get_solve_status(token):
    """Poll the webservice for the status of a submitted job.

    Proxies to GET /api/jobs/:token which returns:
    {"token", "status", "createdAt", "completedAt", "systemFile", "error"}
    where status is one of: pending, running, completed, failed.
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs/{token}",
            timeout=30,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on status token=%s", token)
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on status token=%s", token)
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on status token=%s status=%s", token, status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on status token=%s", token)
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/results/<token>", methods=["GET"])
def get_solve_results(token):
    """Retrieve and parse results from the webservice for a completed job.

    Proxies to GET /api/jobs/:token/download which returns a ZIP containing:
      - output/  (solver output CSV/Parquet files)
      - stdout.log, stderr.log, job.json
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs/{token}/download",
            timeout=120,
        )
        resp.raise_for_status()

        # The webservice returns a ZIP with results
        results = _parse_results_zip(resp.content)
        return jsonify(results)
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on results token=%s", token)
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on results token=%s", token)
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on results token=%s status=%s", token, status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on results token=%s", token)
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/jobs", methods=["GET"])
def list_solve_jobs():
    """List all jobs from the webservice.

    Proxies to GET /api/jobs which returns {"jobs": [...]}.
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs",
            timeout=30,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on jobs list")
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on jobs list")
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on jobs list status=%s", status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on jobs list")
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/ping", methods=["GET"])
def ping_webservice():
    """Ping the webservice to check connectivity and retrieve gtopt version.

    Proxies to GET /api/ping which returns service status and gtopt version info.
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/ping",
            timeout=10,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on ping")
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on ping")
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on ping status=%s", status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on ping")
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/logs", methods=["GET"])
def get_webservice_logs():
    """Retrieve logs from the webservice.

    Proxies to GET /api/logs on the webservice to retrieve its log content.
    """
    lines = request.args.get("lines", default=DEFAULT_LOG_LINES, type=int)
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/logs",
            params={"lines": lines},
            timeout=10,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on logs")
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on logs")
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on logs status=%s", status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on logs")
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/job_logs/<token>", methods=["GET"])
def get_job_logs(token):
    """Retrieve terminal output (stdout/stderr) for a specific job.

    Proxies to GET /api/jobs/:token/logs on the webservice.
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs/{token}/logs",
            timeout=30,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on job_logs token=%s", token)
        return jsonify(
            {
                "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
            }
        ), 502
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on job_logs token=%s", token)
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on job_logs token=%s status=%s", token, status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on job_logs token=%s", token)
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/shutdown", methods=["POST"])
def shutdown_service():
    """Shut down the guiservice (and its parent gtopt_gui launcher).

    Sends SIGTERM to the current process so that the gtopt_gui cleanup handler
    can terminate both the guiservice and the webservice.
    """
    app.logger.info("Shutdown requested via /api/shutdown")
    func = request.environ.get("werkzeug.server.shutdown")
    if func is not None:
        func()
        return jsonify({"status": "shutting_down"})
    # For newer Werkzeug / production servers, kill the process
    os.kill(os.getpid(), signal.SIGTERM)
    return jsonify({"status": "shutting_down"})


@app.route("/api/check_server", methods=["GET"])
def check_server():
    """Run comprehensive checks on the webservice and return aggregated results.

    Performs ping, log retrieval, and job listing against the configured
    webservice URL and returns all results in a single response.
    """
    results: dict[str, Any] = {
        "webservice_url": _webservice_url,
        "ping": None,
        "logs": None,
        "jobs": None,
    }

    # --- Ping ---
    try:
        resp = http_requests.get(f"{_webservice_url}/api/ping", timeout=10)
        resp.raise_for_status()
        results["ping"] = {"status": "ok", "data": resp.json()}
    except Exception as e:
        results["ping"] = {"status": "error", "error": str(e)}

    # --- Logs ---
    try:
        resp = http_requests.get(f"{_webservice_url}/api/logs", params={"lines": 20}, timeout=10)
        resp.raise_for_status()
        results["logs"] = {"status": "ok", "data": resp.json()}
    except Exception as e:
        results["logs"] = {"status": "error", "error": str(e)}

    # --- Jobs ---
    try:
        resp = http_requests.get(f"{_webservice_url}/api/jobs", timeout=10)
        resp.raise_for_status()
        results["jobs"] = {"status": "ok", "data": resp.json()}
    except Exception as e:
        results["jobs"] = {"status": "error", "error": str(e)}

    return jsonify(results)


def main():
    """Run the guiservice development server."""
    debug = os.environ.get("FLASK_DEBUG", "0") == "1"
    port = int(os.environ.get("GTOPT_GUI_PORT", "5001"))
    app.run(host="0.0.0.0", port=port, debug=debug)


if __name__ == "__main__":
    main()
