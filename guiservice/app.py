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
import math
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

# Optional: gtopt_diagram may not be installed; guiservice works without it
# but the /api/diagram/topology endpoint will return a 503.
# The module-level import with try/except is preferred over deferred imports
# so that the optional dependency is resolved once at startup.
try:
    import sys as _sys

    _scripts_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "scripts")
    if _scripts_dir not in _sys.path:
        _sys.path.insert(0, _scripts_dir)
    from gtopt_diagram import FilterOptions as _FilterOptions
    from gtopt_diagram import TopologyBuilder as _TopologyBuilder
    from gtopt_diagram import model_to_reactflow as _model_to_reactflow
    from gtopt_diagram import model_to_visjs as _model_to_visjs

    _DIAGRAM_AVAILABLE = True
except ImportError:
    _FilterOptions = None
    _TopologyBuilder = None
    _model_to_visjs = None
    _model_to_reactflow = None
    _DIAGRAM_AVAILABLE = False

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

# Options schema – mirrors PlanningOptions in include/gtopt/planning_options.hpp
OPTIONS_SCHEMA = {
    # ── Input/Output ──────────────────────────────────────────────────────────
    "input_directory": {"type": "string", "label": "Input Directory"},
    "input_format": {
        "type": "select",
        "label": "Input Format",
        "options": ["", "parquet", "csv"],
    },
    "output_directory": {"type": "string", "label": "Output Directory"},
    "output_format": {
        "type": "select",
        "label": "Output Format",
        "options": ["parquet", "csv"],
    },
    "output_compression": {
        "type": "select",
        "label": "Output Compression",
        "options": ["", "gzip", "zstd", "lzo", "uncompressed"],
    },
    "use_uid_fname": {"type": "boolean", "label": "Use UID Filename"},
    # ── Solver method ─────────────────────────────────────────────────────────
    "method": {
        "type": "select",
        "label": "Method",
        "options": ["", "monolithic", "sddp", "cascade"],
    },
    # ── Model parameters ──────────────────────────────────────────────────────
    "annual_discount_rate": {"type": "number", "label": "Annual Discount Rate"},
    "demand_fail_cost": {"type": "number", "label": "Demand Fail Cost [$/MWh]"},
    "reserve_fail_cost": {"type": "number", "label": "Reserve Fail Cost [$/MWh]"},
    "hydro_fail_cost": {"type": "number", "label": "Hydro Fail Cost [$/m³]"},
    "hydro_use_value": {"type": "number", "label": "Hydro Use Value [$/m³]"},
    "scale_objective": {"type": "number", "label": "Scale Objective"},
    "scale_theta": {"type": "number", "label": "Scale Theta"},
    "kirchhoff_threshold": {"type": "number", "label": "Kirchhoff Threshold [kV]"},
    "use_line_losses": {"type": "boolean", "label": "Use Line Losses"},
    "use_kirchhoff": {"type": "boolean", "label": "Use Kirchhoff"},
    "use_single_bus": {"type": "boolean", "label": "Use Single Bus"},
    "loss_segments": {"type": "integer", "label": "Loss Segments"},
    # ── Logging ───────────────────────────────────────────────────────────────
    "log_directory": {"type": "string", "label": "Log Directory"},
    "lp_debug": {"type": "boolean", "label": "LP Debug"},
    "lp_compression": {
        "type": "select",
        "label": "LP Compression",
        "options": ["", "zstd", "gzip", "lz4", "bzip2", "xz", "uncompressed"],
    },
    "lp_only": {"type": "boolean", "label": "LP Only (build without solving)"},
    "lp_debug_scene_min": {"type": "integer", "label": "LP Debug Scene Min"},
    "lp_debug_scene_max": {"type": "integer", "label": "LP Debug Scene Max"},
    "lp_debug_phase_min": {"type": "integer", "label": "LP Debug Phase Min"},
    "lp_debug_phase_max": {"type": "integer", "label": "LP Debug Phase Max"},
    # ── Constraint handling ───────────────────────────────────────────────────
    "constraint_mode": {
        "type": "select",
        "label": "Constraint Mode",
        "options": ["", "strict", "normal"],
    },
}

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
            {"name": "ecost", "type": "number_or_file", "required": False},
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
            {
                "name": "generator",
                "type": "string",
                "required": True,
                "ref": "generator",
            },
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
            {
                "name": "junction_a",
                "type": "string",
                "required": True,
                "ref": "junction",
            },
            {
                "name": "junction_b",
                "type": "string",
                "required": True,
                "ref": "junction",
            },
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
            {"name": "ecost", "type": "number_or_file", "required": False},
            {"name": "vini", "type": "number", "required": False},
            {"name": "vfin", "type": "number", "required": False},
            {"name": "fmin", "type": "number", "required": False},
            {"name": "fmax", "type": "number", "required": False},
            {"name": "energy_scale", "type": "number", "required": False},
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
            {
                "name": "generator",
                "type": "string",
                "required": True,
                "ref": "generator",
            },
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
    "seepage": {
        "label": "ReservoirSeepage",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "waterway", "type": "string", "required": True, "ref": "waterway"},
            {
                "name": "reservoir",
                "type": "string",
                "required": True,
                "ref": "reservoir",
            },
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
            {
                "name": "generator",
                "type": "string",
                "required": True,
                "ref": "generator",
            },
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
            {
                "name": "generator",
                "type": "string",
                "required": True,
                "ref": "generator",
            },
            {"name": "reserve_zones", "type": "string", "required": True},
            {"name": "urmax", "type": "number_or_file", "required": False},
            {"name": "drmax", "type": "number_or_file", "required": False},
            {"name": "ur_capacity_factor", "type": "number_or_file", "required": False},
            {"name": "dr_capacity_factor", "type": "number_or_file", "required": False},
            {
                "name": "ur_provision_factor",
                "type": "number_or_file",
                "required": False,
            },
            {
                "name": "dr_provision_factor",
                "type": "number_or_file",
                "required": False,
            },
            {"name": "urcost", "type": "number_or_file", "required": False},
            {"name": "drcost", "type": "number_or_file", "required": False},
        ],
    },
    "reservoir_discharge_limit": {
        "label": "Reservoir Discharge Limit",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {
                "name": "waterway",
                "type": "string",
                "required": True,
                "ref": "waterway",
            },
            {
                "name": "reservoir",
                "type": "string",
                "required": True,
                "ref": "reservoir",
            },
        ],
    },
    "reservoir_production_factor": {
        "label": "Reservoir Production Factor",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {
                "name": "turbine",
                "type": "string",
                "required": True,
                "ref": "turbine",
            },
            {
                "name": "reservoir",
                "type": "string",
                "required": True,
                "ref": "reservoir",
            },
            {"name": "mean_production_factor", "type": "number", "required": False},
        ],
    },
    "flow_right": {
        "label": "Flow Right",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "purpose", "type": "string", "required": False},
            {
                "name": "junction",
                "type": "string",
                "required": False,
                "ref": "junction",
            },
            {"name": "direction", "type": "integer", "required": False},
            {"name": "discharge", "type": "number_or_file", "required": False},
            {"name": "fmax", "type": "number_or_file", "required": False},
            {"name": "consumptive", "type": "boolean", "required": False},
            {"name": "use_average", "type": "boolean", "required": False},
            {"name": "fail_cost", "type": "number_or_file", "required": False},
            {"name": "use_value", "type": "number_or_file", "required": False},
            {"name": "priority", "type": "number", "required": False},
        ],
    },
    "volume_right": {
        "label": "Volume Right",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "purpose", "type": "string", "required": False},
            {
                "name": "reservoir",
                "type": "string",
                "required": False,
                "ref": "reservoir",
            },
            {"name": "emin", "type": "number_or_file", "required": False},
            {"name": "emax", "type": "number_or_file", "required": False},
            {"name": "ecost", "type": "number_or_file", "required": False},
            {"name": "eini", "type": "number", "required": False},
            {"name": "efin", "type": "number", "required": False},
            {"name": "demand", "type": "number_or_file", "required": False},
            {"name": "fmax", "type": "number_or_file", "required": False},
            {"name": "fail_cost", "type": "number", "required": False},
            {"name": "priority", "type": "number", "required": False},
            {"name": "flow_conversion_rate", "type": "number", "required": False},
            {"name": "energy_scale", "type": "number", "required": False},
            {"name": "consumptive", "type": "boolean", "required": False},
            {"name": "use_state_variable", "type": "boolean", "required": False},
        ],
    },
    "user_param": {
        "label": "User Parameter",
        "fields": [
            {"name": "name", "type": "string", "required": True},
            {"name": "value", "type": "number", "required": False},
        ],
    },
    "user_constraint": {
        "label": "User Constraint",
        "fields": [
            {"name": "uid", "type": "integer", "required": True},
            {"name": "name", "type": "string", "required": True},
            {"name": "active", "type": "boolean", "required": False},
            {"name": "expression", "type": "string", "required": True},
            {"name": "description", "type": "string", "required": False},
            {"name": "constraint_type", "type": "string", "required": False},
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
    "seepage": "reservoir_seepage_array",
    "reservoir_discharge_limit": "reservoir_discharge_limit_array",
    "reservoir_production_factor": "reservoir_production_factor_array",
    "generator_profile": "generator_profile_array",
    "demand_profile": "demand_profile_array",
    "reserve_zone": "reserve_zone_array",
    "reserve_provision": "reserve_provision_array",
    "flow_right": "flow_right_array",
    "volume_right": "volume_right_array",
    "user_param": "user_param_array",
    "user_constraint": "user_constraint_array",
}


# ---------------------------------------------------------------------------
# Helper functions
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


@app.route("/api/options_schema", methods=["GET"])
def get_options_schema():
    """Return the options schema for use by the GUI."""
    app.logger.debug("Serving options schema")
    return jsonify(OPTIONS_SCHEMA)


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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify({"error": f"Webservice error ({e.response.status_code}): {body}"}),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
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


@app.route("/api/solve/monitor/<token>", methods=["GET"])
def get_solve_monitor(token):
    """Poll the solver monitor status for a running job.

    Proxies to GET /api/jobs/:token/monitor on the webservice which reads
    the solver JSON status file (solver_status.json)
    written by the gtopt binary.
    Returns the parsed JSON or {"available": false} if not yet written.
    """
    try:
        resp = http_requests.get(
            f"{_webservice_url}/api/jobs/{token}/monitor",
            timeout=10,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on monitor token=%s", token)
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on monitor token=%s", token)
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on monitor token=%s status=%s", token, status)
        return jsonify({"error": f"Webservice error: {status}"}), 502
    except Exception as e:
        app.logger.exception("Unexpected error on monitor token=%s", token)
        return jsonify({"error": f"Unexpected error: {str(e)}"}), 500


@app.route("/api/solve/stop/<token>", methods=["POST"])
def stop_solve(token):
    """Stop a running solver job.

    Proxies to POST /api/jobs/:token/stop on the webservice.

    Query parameters forwarded to the webservice:
      mode=soft  (default) — Write the monitoring API stop-request file
                  (sddp_stop_request.json) so the SDDP solver finishes the
                  current iteration and saves cuts before stopping gracefully.
      mode=force — Send SIGTERM to the process immediately (hard stop).
    """
    mode = request.args.get("mode", "soft")
    try:
        resp = http_requests.post(
            f"{_webservice_url}/api/jobs/{token}/stop",
            params={"mode": mode},
            timeout=10,
        )
        resp.raise_for_status()
        return jsonify(resp.json())
    except http_requests.ConnectionError:
        app.logger.warning("Webservice connection error on stop token=%s", token)
        return (
            jsonify(
                {
                    "error": f"Cannot connect to webservice at {_webservice_url}. Is the webservice running?"
                }
            ),
            502,
        )
    except http_requests.Timeout:
        app.logger.warning("Webservice timeout on stop token=%s", token)
        return jsonify({"error": "Webservice request timed out"}), 504
    except http_requests.HTTPError as e:
        status = e.response.status_code if e.response is not None else 502
        app.logger.warning("Webservice HTTP error on stop token=%s status=%s", token, status)
        return jsonify({"error": f"Webservice error: {status}"}), status
    except Exception as e:
        app.logger.exception("Unexpected error on stop token=%s", token)
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


# ---------------------------------------------------------------------------
# Topology diagram API
# ---------------------------------------------------------------------------


@app.route("/api/diagram/topology", methods=["POST"])
def diagram_topology():
    """Generate a vis.js-compatible topology graph from case data.

    Accepts a JSON body with the same structure as ``/api/case/upload``
    (the GUI ``caseData`` object) and optional rendering parameters.

    Request body (JSON):
        caseData     (required) – the GUI case data object
        subsystem    (optional) – "full" | "electrical" | "hydro"  [default: "full"]
        aggregate    (optional) – "auto" | "none" | "bus" | "type" | "global"
        no_generators (optional) – true to hide all generator nodes
        compact      (optional) – true to suppress detail labels

    Response JSON:
        nodes  – list of vis.js node objects
        edges  – list of vis.js edge objects
        meta   – dict with aggregate, voltage_threshold, n_total, visible_buses
    """
    if not _DIAGRAM_AVAILABLE:
        return jsonify({"error": "gtopt_diagram not available"}), 503

    body = request.get_json(silent=True) or {}
    case_data = body.get("caseData", body)
    subsystem = body.get("subsystem", "full")
    aggregate = body.get("aggregate", "auto")
    no_gen = bool(body.get("no_generators", False))
    compact = bool(body.get("compact", False))
    vthresh = float(body.get("voltage_threshold", 0.0))
    fmt = str(body.get("format", "visjs")).lower()

    # Build the gtopt planning JSON from GUI case data
    try:
        planning = _build_case_json(case_data)
    except Exception as exc:
        return jsonify({"error": f"Failed to build case JSON: {exc}"}), 400

    opts = _FilterOptions(
        aggregate=aggregate,
        no_generators=no_gen,
        compact=compact,
        voltage_threshold=vthresh,
    )
    try:
        builder = _TopologyBuilder(planning, subsystem=subsystem, opts=opts)
        model = builder.build()
    except Exception as exc:
        app.logger.exception("Topology builder error")
        return jsonify({"error": str(exc)}), 500

    if fmt == "reactflow":
        graph = _model_to_reactflow(model)
    else:
        graph = _model_to_visjs(model)

    meta = {
        "aggregate": builder.eff_agg,
        "voltage_threshold": builder.eff_vthresh,
        "no_generators": no_gen,
        "n_nodes": len(graph["nodes"]),
        "n_edges": len(graph["edges"]),
        "format": fmt if fmt == "reactflow" else "visjs",
    }
    if builder.auto_info:
        meta["n_total"] = builder.auto_info[0]
        meta["auto_mode"] = True

    return jsonify({"nodes": graph["nodes"], "edges": graph["edges"], "meta": meta})


# ---------------------------------------------------------------------------
# GUI Plus: validation, templates, and results aggregation endpoints
# ---------------------------------------------------------------------------


# Templates shipped inside the repo.  Key = slug used in URLs, value = path
# relative to the repository root (two levels up from guiservice/app.py).
_REPO_ROOT = os.path.dirname(os.path.dirname(__file__))
_TEMPLATES_DIR = os.path.join(_REPO_ROOT, "cases")

# Curated list of templates; must be present on disk.  The `blank` template
# has a special in-memory definition below and therefore no file.
_CASE_TEMPLATES: list[dict[str, Any]] = [
    {
        "slug": "blank",
        "name": "Blank case",
        "description": "An empty case with a single block/stage/scenario scaffold.",
        "category": "starter",
    },
    {
        "slug": "c0",
        "name": "c0 — single-bus expansion",
        "description": "5-stage multi-stage capacity expansion of a single-bus case.",
        "category": "expansion",
        "path": os.path.join(_TEMPLATES_DIR, "c0", "system_c0.json"),
    },
    {
        "slug": "ieee_4b_ori",
        "name": "IEEE 4-bus",
        "description": "4-bus OPF test case with 2 thermal generators.",
        "category": "opf",
        "path": os.path.join(_TEMPLATES_DIR, "ieee_4b_ori", "ieee_4b_ori.json"),
    },
    {
        "slug": "ieee_9b_ori",
        "name": "IEEE 9-bus",
        "description": "Standard 9-bus OPF benchmark (single snapshot).",
        "category": "opf",
        "path": os.path.join(_TEMPLATES_DIR, "ieee_9b_ori", "ieee_9b_ori.json"),
    },
    {
        "slug": "ieee_14b_ori",
        "name": "IEEE 14-bus",
        "description": "Standard 14-bus OPF benchmark (24 hourly blocks).",
        "category": "opf",
        "path": os.path.join(_TEMPLATES_DIR, "ieee_14b_ori", "ieee_14b_ori.json"),
    },
]


def _blank_template() -> dict[str, Any]:
    """Return a minimal blank gtopt case."""
    return {
        "options": {
            "input_directory": "case",
            "input_format": "csv",
            "output_directory": "output",
            "output_format": "csv",
            "use_single_bus": True,
            "scale_objective": 1000.0,
            "demand_fail_cost": 1000.0,
        },
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [
                {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
            ],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "name": "blank",
            "bus_array": [{"uid": 1, "name": "b1"}],
        },
    }


def _planning_json_to_case_data(planning: dict[str, Any], name: str) -> dict[str, Any]:
    """Convert a raw planning JSON into the GUI case_data shape."""
    case_data: dict[str, Any] = {
        "case_name": name,
        "options": planning.get("options", {}),
        "simulation": planning.get("simulation", {}),
        "system": {},
        "data_files": {},
    }
    system_raw = planning.get("system", {})
    for elem_type, array_key in ELEMENT_TO_ARRAY_KEY.items():
        if array_key in system_raw:
            case_data["system"][elem_type] = system_raw[array_key]
    return case_data


@app.route("/api/templates", methods=["GET"])
def list_templates():
    """Return the curated list of bundled case templates."""
    out = []
    for tpl in _CASE_TEMPLATES:
        entry = {
            "slug": tpl["slug"],
            "name": tpl["name"],
            "description": tpl["description"],
            "category": tpl["category"],
        }
        # Only advertise templates whose file exists (blank is always available).
        if tpl["slug"] == "blank" or os.path.isfile(tpl.get("path", "")):
            out.append(entry)
    return jsonify({"templates": out})


@app.route("/api/templates/<slug>", methods=["GET"])
def get_template(slug: str):
    """Return the full case JSON for a given template slug."""
    tpl = next((t for t in _CASE_TEMPLATES if t["slug"] == slug), None)
    if tpl is None:
        return jsonify({"error": f"Unknown template: {slug}"}), 404

    if tpl["slug"] == "blank":
        planning = _blank_template()
        return jsonify(_planning_json_to_case_data(planning, "blank"))

    path = tpl.get("path")
    if not path or not os.path.isfile(path):
        return jsonify({"error": f"Template file missing: {slug}"}), 404

    try:
        with open(path, "r", encoding="utf-8") as fh:
            planning = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        return jsonify({"error": f"Failed to load template: {exc}"}), 500

    return jsonify(_planning_json_to_case_data(planning, slug))


def _validate_case(case_data: dict[str, Any]) -> dict[str, Any]:
    """Run structural checks and return a ``{errors, warnings}`` dict.

    Errors indicate that the case will fail to solve (duplicate UIDs,
    missing required refs).  Warnings indicate smells that do not
    necessarily cause a failure (buses with no connected element, empty
    arrays, etc.).
    """
    errors: list[dict[str, Any]] = []
    warnings: list[dict[str, Any]] = []

    system = case_data.get("system") or {}

    # Duplicate UIDs and name collection per element type
    by_type_names: dict[str, set[str]] = {}
    by_type_uids: dict[str, set[int]] = {}

    for elem_type, items in system.items():
        if not isinstance(items, list):
            continue
        names = set()
        uids: set[int] = set()
        for i, item in enumerate(items):
            if not isinstance(item, dict):
                continue
            uid = item.get("uid")
            if uid is not None:
                if uid in uids:
                    errors.append(
                        {
                            "type": "duplicate_uid",
                            "element_type": elem_type,
                            "index": i,
                            "uid": uid,
                            "message": (
                                f"Duplicate uid={uid} in {elem_type}_array"
                            ),
                        }
                    )
                uids.add(uid)
            name = item.get("name")
            if name in (None, ""):
                errors.append(
                    {
                        "type": "missing_name",
                        "element_type": elem_type,
                        "index": i,
                        "message": f"{elem_type}[{i}] is missing a name",
                    }
                )
            else:
                if name in names:
                    errors.append(
                        {
                            "type": "duplicate_name",
                            "element_type": elem_type,
                            "index": i,
                            "name": name,
                            "message": (
                                f"Duplicate name='{name}' in {elem_type}_array"
                            ),
                        }
                    )
                names.add(name)
        by_type_names[elem_type] = names
        by_type_uids[elem_type] = uids

    # Referential integrity: every field with a ``ref`` must resolve
    for elem_type, items in system.items():
        schema = ELEMENT_SCHEMAS.get(elem_type)
        if schema is None or not isinstance(items, list):
            continue
        for i, item in enumerate(items):
            if not isinstance(item, dict):
                continue
            for field in schema["fields"]:
                ref = field.get("ref")
                if not ref:
                    continue
                fname = field["name"]
                val = item.get(fname)
                if val in (None, "", []):
                    if field.get("required"):
                        errors.append(
                            {
                                "type": "missing_required_ref",
                                "element_type": elem_type,
                                "index": i,
                                "field": fname,
                                "ref": ref,
                                "message": (
                                    f"{elem_type}[{i}].{fname} is required "
                                    f"(ref={ref})"
                                ),
                            }
                        )
                    continue
                target_names = by_type_names.get(ref, set())
                target_uids = by_type_uids.get(ref, set())
                # The reference may be a name (string) or a uid (int).
                if isinstance(val, (int, float)):
                    ok = int(val) in target_uids
                else:
                    ok = str(val) in target_names
                if not ok:
                    errors.append(
                        {
                            "type": "dangling_ref",
                            "element_type": elem_type,
                            "index": i,
                            "field": fname,
                            "ref": ref,
                            "value": val,
                            "message": (
                                f"{elem_type}[{i}].{fname}={val!r} does not "
                                f"match any {ref}.name or {ref}.uid"
                            ),
                        }
                    )

    # Warning: buses with no connected element
    bus_names = by_type_names.get("bus", set())
    referenced_buses: set[str] = set()
    for elem_type, items in system.items():
        schema = ELEMENT_SCHEMAS.get(elem_type)
        if schema is None or not isinstance(items, list):
            continue
        bus_fields = [f["name"] for f in schema["fields"] if f.get("ref") == "bus"]
        if not bus_fields:
            continue
        for item in items:
            if not isinstance(item, dict):
                continue
            for bf in bus_fields:
                val = item.get(bf)
                if val not in (None, "", []):
                    referenced_buses.add(str(val))

    for name in bus_names - referenced_buses:
        warnings.append(
            {
                "type": "isolated_bus",
                "element_type": "bus",
                "name": name,
                "message": f"Bus '{name}' has no connected element",
            }
        )

    # Warning: simulation array empty
    sim = case_data.get("simulation") or {}
    for key, label in (
        ("block_array", "blocks"),
        ("stage_array", "stages"),
        ("scenario_array", "scenarios"),
    ):
        arr = sim.get(key) or []
        if not arr:
            warnings.append(
                {
                    "type": "empty_simulation_array",
                    "array": key,
                    "message": f"No {label} defined (simulation.{key} is empty)",
                }
            )

    return {
        "ok": not errors,
        "n_errors": len(errors),
        "n_warnings": len(warnings),
        "errors": errors,
        "warnings": warnings,
    }


@app.route("/api/case/validate", methods=["POST"])
def api_validate_case():
    """Validate a case structure; return errors and warnings."""
    case_data = request.get_json(silent=True) or {}
    return jsonify(_validate_case(case_data))


@app.route("/api/case/check_refs", methods=["POST"])
def api_check_refs():
    """Return only the cross-reference errors (subset of validate)."""
    case_data = request.get_json(silent=True) or {}
    full = _validate_case(case_data)
    ref_errors = [
        e
        for e in full["errors"]
        if e.get("type") in ("dangling_ref", "missing_required_ref")
    ]
    return jsonify(
        {
            "ok": not ref_errors,
            "n_errors": len(ref_errors),
            "errors": ref_errors,
        }
    )


@app.route("/api/results/summary", methods=["POST"])
def api_results_summary():
    """Compute KPI summary from a parsed results dict.

    Body JSON:
      results          — parsed results dict (same shape as /api/results/upload)
      scale_objective  — optional float (default 1.0)
      tech_map         — optional dict[uid_str, tech_str]
    """
    body = request.get_json(silent=True) or {}
    results = body.get("results") or {}
    scale_obj = float(body.get("scale_objective", 1.0))
    tech_map = body.get("tech_map")

    try:
        # Lazy import so guiservice still loads when scripts are not installed.
        from gtopt_results_summary.summary import summarize_output_dict

        summary = summarize_output_dict(
            results, tech_map=tech_map, scale_objective=scale_obj
        )
    except ImportError:
        return (
            jsonify({"error": "gtopt_results_summary is not installed"}),
            503,
        )
    except Exception as exc:  # pylint: disable=broad-exception-caught
        app.logger.exception("Results summary error")
        return jsonify({"error": str(exc)}), 500
    return jsonify(summary)


@app.route("/api/results/aggregate", methods=["POST"])
def api_results_aggregate():
    """Aggregate one output table along the (scenario, stage, block) dimensions.

    Body JSON:
      table            — {columns:[...], data:[[...]]} (required)
      group_by         — optional list of indexing columns to keep
                        (default: all of scenario/stage/block that exist)
      aggregation      — "sum" | "mean" | "max" | "min"  (default "sum")

    Response:
      rows             — aggregated rows in the same shape
      columns          — column order of the aggregated result
    """
    body = request.get_json(silent=True) or {}
    table = body.get("table") or {}
    cols = list(table.get("columns") or [])
    data = table.get("data") or []
    if not cols:
        return jsonify({"error": "Empty table"}), 400

    agg = str(body.get("aggregation", "sum")).lower()
    if agg not in ("sum", "mean", "max", "min"):
        return jsonify({"error": f"Unknown aggregation: {agg}"}), 400

    idx_candidates = [c for c in ("scenario", "stage", "block") if c in cols]
    group_by = body.get("group_by")
    if group_by is None:
        group_by = idx_candidates
    else:
        group_by = [c for c in group_by if c in cols]

    if not data:
        return jsonify({"rows": [], "columns": cols})

    try:
        df = pd.DataFrame(data, columns=cols)
        num_cols = [c for c in cols if c not in group_by]
        for c in num_cols:
            df[c] = pd.to_numeric(df[c], errors="coerce")
        if group_by:
            grouped = df.groupby(group_by, as_index=False)
            aggregated = getattr(grouped, agg)(numeric_only=True)
        else:
            aggregated = pd.DataFrame(
                [getattr(df[num_cols], agg)(numeric_only=True).to_dict()]
            )
    except Exception as exc:  # pylint: disable=broad-exception-caught
        return jsonify({"error": f"Aggregation failed: {exc}"}), 500

    rows = [[_sanitize_value(x) for x in row] for row in aggregated.itertuples(
        index=False, name=None
    )]
    return jsonify({"rows": rows, "columns": list(aggregated.columns)})


@app.route("/api/results/export/excel", methods=["POST"])
def api_results_export_excel():
    """Export a parsed results dict to a downloadable Excel workbook.

    Body JSON:
      results          — parsed results dict
      scale_objective  — optional float (default 1.0)
      tech_map         — optional per-uid technology mapping
      case_name        — optional download filename stem (default "results")
    """
    body = request.get_json(silent=True) or {}
    results = body.get("results") or {}
    scale_obj = float(body.get("scale_objective", 1.0))
    tech_map = body.get("tech_map")
    case_name = body.get("case_name") or "results"

    try:
        from gtopt_timeseries_export.exporter import export_from_dict
    except ImportError:
        return (
            jsonify({"error": "gtopt_timeseries_export is not installed"}),
            503,
        )

    with tempfile.NamedTemporaryFile(suffix=".xlsx", delete=False) as tmp:
        dest = tmp.name
    try:
        export_from_dict(
            results,
            dest,
            scale_objective=scale_obj,
            tech_map=tech_map,
        )
    except Exception as exc:  # pylint: disable=broad-exception-caught
        app.logger.exception("Excel export error")
        # Best-effort cleanup
        try:
            os.unlink(dest)
        except OSError:
            pass
        return jsonify({"error": str(exc)}), 500

    return send_file(
        dest,
        mimetype=(
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
        ),
        as_attachment=True,
        download_name=f"{case_name}.xlsx",
    )


def main():
    """Run the guiservice development server."""
    debug = os.environ.get("FLASK_DEBUG", "0") == "1"
    port = int(os.environ.get("GTOPT_GUI_PORT", "5001"))
    app.run(host="0.0.0.0", port=port, debug=debug)


if __name__ == "__main__":
    main()