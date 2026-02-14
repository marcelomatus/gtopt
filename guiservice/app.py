"""
gtopt GUI Service - Web application for creating, editing, and visualizing
gtopt optimization cases.

This module provides the Flask application with REST API endpoints for
case management and a browser-based GUI for interactive case creation.
"""

import csv
import io
import json
import os
import tempfile
import zipfile

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
    """Build a ZIP file containing the full case configuration."""
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
    """Parse an uploaded ZIP file into case data structure."""
    case_data = {
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
        case_data["case_name"] = os.path.splitext(
            os.path.basename(main_json_name)
        )[0]

        raw = json.loads(zf.read(main_json_name))
        case_data["options"] = raw.get("options", {})
        case_data["simulation"] = raw.get("simulation", {})

        # Parse system elements
        system_raw = raw.get("system", {})
        for elem_type, array_key in ELEMENT_TO_ARRAY_KEY.items():
            if array_key in system_raw:
                case_data["system"][elem_type] = system_raw[array_key]

        # Parse data files (CSV and Parquet)
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
                except Exception:
                    pass

            elif name.endswith(".parquet"):
                rel = name
                input_dir = case_data["options"].get("input_directory", "")
                if input_dir and rel.startswith(input_dir + "/"):
                    rel = rel[len(input_dir) + 1 :]
                if rel.endswith(".parquet"):
                    rel = rel[:-8]
                try:
                    df = pd.read_parquet(io.BytesIO(zf.read(name)))
                    case_data["data_files"][rel] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
                    }
                except Exception:
                    pass

    return case_data


def _parse_results_zip(zip_bytes):
    """Parse a results ZIP or directory structure into viewable data."""
    results = {"solution": {}, "outputs": {}}

    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        for name in zf.namelist():
            if name.endswith(".csv"):
                try:
                    content = zf.read(name).decode("utf-8")
                    reader = csv.reader(io.StringIO(content))
                    rows = list(reader)
                    base = os.path.basename(name)
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

            elif name.endswith(".parquet"):
                try:
                    df = pd.read_parquet(io.BytesIO(zf.read(name)))
                    base = os.path.splitext(os.path.basename(name))[0]
                    parent = os.path.basename(os.path.dirname(name))
                    key = f"{parent}/{base}" if parent else base
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
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
    return jsonify(ELEMENT_SCHEMAS)


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
    global _webservice_url
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
    """Build case ZIP and submit it to the gtopt webservice for solving.

    The webservice expects POST /api/jobs with multipart form data:
      - file: the ZIP archive
      - systemFile: name of the system JSON inside the archive
    It returns {"token": "...", "status": "pending", "message": "..."}.
    """
    global _webservice_url
    case_data = request.get_json()
    if not case_data:
        return jsonify({"error": "No case data provided"}), 400

    # Build the ZIP
    zip_buf = _build_zip(case_data)
    case_name = case_data.get("case_name", "case")
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
        return jsonify(
            {"error": f"Cannot connect to webservice at {_webservice_url}"}
        ), 502
    except http_requests.Timeout:
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        body = ""
        if e.response is not None:
            try:
                body = e.response.json().get("error", e.response.text)
            except Exception:
                body = e.response.text
        return jsonify(
            {"error": f"Webservice error ({e.response.status_code}): {body}"}
        ), 502
    except Exception as e:
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/status/<token>", methods=["GET"])
def get_solve_status(token):
    """Poll the webservice for the status of a submitted job.

    Proxies to GET /api/jobs/:token which returns:
    {"token", "status", "createdAt", "completedAt", "systemFile", "error"}
    where status is one of: pending, running, completed, failed.
    """
    global _webservice_url
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs/{token}",
            timeout=30,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        return jsonify(
            {"error": f"Cannot connect to webservice at {_webservice_url}"}
        ), 502
    except http_requests.Timeout:
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        return jsonify(
            {"error": f"Webservice error: {status}"}
        ), 502
    except Exception as e:
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/results/<token>", methods=["GET"])
def get_solve_results(token):
    """Retrieve and parse results from the webservice for a completed job.

    Proxies to GET /api/jobs/:token/download which returns a ZIP containing:
      - output/  (solver output CSV/Parquet files)
      - stdout.log, stderr.log, job.json
    """
    global _webservice_url
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
        return jsonify(
            {"error": f"Cannot connect to webservice at {_webservice_url}"}
        ), 502
    except http_requests.Timeout:
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        return jsonify(
            {"error": f"Webservice error: {status}"}
        ), 502
    except Exception as e:
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/jobs", methods=["GET"])
def list_solve_jobs():
    """List all jobs from the webservice.

    Proxies to GET /api/jobs which returns {"jobs": [...]}.
    """
    global _webservice_url
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs",
            timeout=30,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        return jsonify(
            {"error": f"Cannot connect to webservice at {_webservice_url}"}
        ), 502
    except http_requests.Timeout:
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        return jsonify(
            {"error": f"Webservice error: {status}"}
        ), 502
    except Exception as e:
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


def main():
    """Run the guiservice development server."""
    debug = os.environ.get("FLASK_DEBUG", "0") == "1"
    port = int(os.environ.get("GTOPT_GUI_PORT", "5001"))
    app.run(host="0.0.0.0", port=port, debug=debug)


if __name__ == "__main__":
    main()
