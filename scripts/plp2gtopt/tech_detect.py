# SPDX-License-Identifier: BSD-3-Clause
"""Auto-detect generator technology from PLP type and central name.

PLP classifies generators into broad categories (termica, pasada,
embalse, serie, bateria, falla).  The ``pasada`` and ``termica``
categories are catch-all buckets that include solar, wind, geothermal,
biomass, and other technologies.  This module refines those types by
analysing the central name for technology-specific keywords and
explicit overrides (user-provided or from ``centipo.csv``).

Note: profile presence (``plpmance.dat``) is NOT used as a type
indicator — all pasada centrals have profiles.

Usage::

    from plp2gtopt.tech_detect import detect_technology, load_overrides

    # Auto-detect from PLP type + name
    tech = detect_technology("pasada", "SolarAlmeyda")  # → "solar"
    tech = detect_technology("pasada", "ParqueEolico")  # → "wind"
    tech = detect_technology("termica", "GeotermicaCerro")  # → "geothermal"
    tech = detect_technology("embalse", "RAPEL")  # → "hydro_reservoir"

    # Override specific centrals
    overrides = load_overrides("SolarAlmeyda:solar,Canela:wind")
"""

from __future__ import annotations

import logging
import re

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# PLP type → gtopt base type mapping
# ---------------------------------------------------------------------------

_PLP_BASE_TYPE: dict[str, str] = {
    "embalse": "hydro_reservoir",
    "serie": "hydro_ror",
    "pasada": "hydro_ror",
    "termica": "thermal",
    "bateria": "battery",
    "falla": "curtailment",
}

# ---------------------------------------------------------------------------
# Name-based keyword patterns (case-insensitive)
#
# Each entry is (compiled_regex, technology_type).
# Order matters: first match wins.  More specific patterns come first.
# ---------------------------------------------------------------------------

_NAME_PATTERNS: list[tuple[re.Pattern, str]] = [
    # Solar
    (
        re.compile(
            r"solar|fotovolt|[\b_]pv[\b_]|[\b_]fv[\b_]|^pv[_]|^fv[_]|_fv$|_pv$",
            re.IGNORECASE,
        ),
        "solar",
    ),
    # Wind
    (
        re.compile(
            r"eolic|eol[_\s]|wind|parque.*eol|[\b_]eo[\b_]|_eo$",
            re.IGNORECASE,
        ),
        "wind",
    ),
    # Geothermal
    (re.compile(r"geoterm|geotherm", re.IGNORECASE), "geothermal"),
    # Biomass / biogas
    (re.compile(r"biomas|biogas|bioenerg", re.IGNORECASE), "biomass"),
    # Diesel / fuel oil
    (re.compile(r"diesel|gasoil|fuel\s*oil", re.IGNORECASE), "diesel"),
    # Natural gas / combined cycle / open cycle
    (
        re.compile(
            r"gnl[_\b]|^gnl$|gas\s*nat|ccombinado|ciclo\s*comb|turbogas",
            re.IGNORECASE,
        ),
        "gas",
    ),
    # Coal
    (re.compile(r"carbon|coal|carbo\b", re.IGNORECASE), "coal"),
    # Nuclear
    (re.compile(r"nuclear|nucleo", re.IGNORECASE), "nuclear"),
    # Concentrated solar power (CSP)
    (re.compile(r"csp|concentr.*solar|torre\s*solar", re.IGNORECASE), "csp"),
    # Small hydro / mini hydro
    (re.compile(r"mini.*hidr|micro.*hidr|pch\b", re.IGNORECASE), "hydro_small"),
    # Pumped hydro storage
    (re.compile(r"bomb|pump.*stor|reversib", re.IGNORECASE), "hydro_pumped"),
    # Generic hydro (for pasada names that are truly run-of-river)
    (re.compile(r"hidr|hydro|agua|rio\b|river", re.IGNORECASE), "hydro_ror"),
]


_SUSPECT_TYPES = frozenset({"solar", "wind"})


def suspect_technology(
    plp_type: str,
    central_name: str,
) -> str | None:
    """Return suspected technology if the name matches solar/wind patterns.

    Only checks ambiguous PLP types (termica, pasada).  Returns ``None``
    when no solar/wind pattern matches.
    """
    if plp_type not in ("termica", "pasada"):
        return None
    for pattern, tech in _NAME_PATTERNS:
        if tech in _SUSPECT_TYPES and pattern.search(central_name):
            return tech
    return None


def detect_technology(
    plp_type: str,
    central_name: str,
    *,
    overrides: dict[str, str] | None = None,
    auto_detect: bool = True,
) -> str:
    """Determine the generator technology type.

    Resolution order:
    1. Explicit override (if ``central_name`` is in *overrides*).
    2. ``centipo.csv`` overrides (loaded into *overrides* by the caller).
    3. Name-based keyword detection (if *auto_detect* is True).
    4. PLP base-type mapping.

    Args:
        plp_type: PLP type string (e.g. "termica", "pasada").
        central_name: Name of the central from plpcnfce.dat.
        overrides: Optional {name: type} dict for explicit assignments.
            This includes both user ``--tech-overrides`` and any entries
            loaded from ``centipo.csv``.
        auto_detect: If True, try name-based keyword detection.

    Returns:
        A refined technology string (e.g. "solar", "wind", "thermal").
    """
    # 1. Explicit override (user + centipo.csv)
    if overrides and central_name in overrides:
        return overrides[central_name]

    # 2. Name-based detection (only for ambiguous PLP types)
    if auto_detect and plp_type in ("termica", "pasada"):
        for pattern, tech in _NAME_PATTERNS:
            if pattern.search(central_name):
                return tech

    # 3. PLP base type
    return _PLP_BASE_TYPE.get(plp_type, plp_type)


# ---------------------------------------------------------------------------
# centipo.csv reader (PLP's own technology override file)
# ---------------------------------------------------------------------------

# PLP centipo.csv label → gtopt technology mapping
_CENTIPO_LABEL_MAP: dict[str, str] = {
    "E": "hydro_reservoir",
    "A": "hydro_reservoir",
    "S": "hydro_ror",
    "R": "hydro_ror",
    "P": "hydro_ror",
    "T": "thermal",
    "B": "battery",
    "F": "curtailment",
    "M": "modular",
    # Common 3-char labels used in Chilean CEN system
    "SOL": "solar",
    "FV": "solar",
    "EO": "wind",
    "EOL": "wind",
    "GEO": "geothermal",
    "BIO": "biomass",
    "GNL": "gas",
    "CAR": "coal",
    "DIE": "diesel",
    "NUC": "nuclear",
    "CSP": "csp",
    "HID": "hydro_ror",
    "MIN": "hydro_small",
    "BOM": "hydro_pumped",
}


def load_centipo_csv(input_dir) -> dict[str, str]:
    """Read PLP's ``centipo.csv`` file if present.

    Returns a {central_name: technology} dict.  The file format is::

        # header line (skipped)
        CentralName  Label

    Where Label is a 1-3 character code mapped to a technology string.
    """
    from pathlib import Path  # noqa: PLC0415

    from .compressed_open import find_compressed_path, compressed_open

    centipo_resolved = find_compressed_path(Path(input_dir) / "centipo.csv")
    if centipo_resolved is None:
        return {}

    result: dict[str, str] = {}
    try:
        with compressed_open(
            centipo_resolved, encoding="latin-1", errors="strict"
        ) as f:
            # Skip header
            next(f, None)
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    name = parts[0].strip("'\"")
                    label = parts[1].strip("'\"").upper()
                    tech = _CENTIPO_LABEL_MAP.get(label, label.lower())
                    result[name] = tech
    except OSError as exc:
        log.warning("failed to read centipo.csv: %s", exc)

    if result:
        log.info("loaded %d type overrides from centipo.csv", len(result))

    return result


def load_overrides(spec: str | None) -> dict[str, str]:
    """Parse a comma-separated ``name:type`` specification.

    Example: ``"SolarAlmeyda:solar,Canela:wind"``
    returns ``{"SolarAlmeyda": "solar", "Canela": "wind"}``.

    If *spec* is a file path (ends with ``.json`` or ``.csv``), reads
    overrides from that file instead.
    """
    if not spec:
        return {}

    # Check if it's a file path
    if spec.endswith(".json"):
        return _load_json_overrides(spec)
    if spec.endswith(".csv"):
        return _load_csv_overrides(spec)

    # Inline spec
    result: dict[str, str] = {}
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if ":" not in token:
            log.warning("invalid override token '%s' (expected name:type)", token)
            continue
        name, tech = token.split(":", maxsplit=1)
        result[name.strip()] = tech.strip()
    return result


def _load_json_overrides(path: str) -> dict[str, str]:
    """Load overrides from a JSON file (dict or list of {name, type})."""
    import json  # noqa: PLC0415

    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    if isinstance(data, dict):
        return {str(k): str(v) for k, v in data.items()}
    if isinstance(data, list):
        return {
            str(item["name"]): str(item["type"])
            for item in data
            if "name" in item and "type" in item
        }
    return {}


def _load_csv_overrides(path: str) -> dict[str, str]:
    """Load overrides from a CSV file (name,type columns)."""
    result: dict[str, str] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", maxsplit=1)
            if len(parts) == 2:
                result[parts[0].strip()] = parts[1].strip()
    return result


# ---------------------------------------------------------------------------
# Bulk application
# ---------------------------------------------------------------------------


def classify_generators(
    centrals: list[dict],
    *,
    overrides: dict[str, str] | None = None,
    auto_detect: bool = True,
) -> dict[str, str]:
    """Classify all centrals and return a {name: technology} mapping.

    Also logs a summary of the detected types.
    """
    result: dict[str, str] = {}
    type_counts: dict[str, int] = {}

    for central in centrals:
        name = central.get("name", "")
        plp_type = central.get("type", "unknown")
        tech = detect_technology(
            plp_type, name, overrides=overrides, auto_detect=auto_detect
        )
        result[name] = tech
        type_counts[tech] = type_counts.get(tech, 0) + 1

    for tech, count in sorted(type_counts.items(), key=lambda x: -x[1]):
        log.info("  technology: %-20s %d generator(s)", tech, count)

    return result


def available_types() -> list[str]:
    """Return the list of all known technology type strings."""
    base = sorted(set(_PLP_BASE_TYPE.values()))
    pattern_types = sorted({tech for _, tech in _NAME_PATTERNS})
    return sorted(set(base + pattern_types))
