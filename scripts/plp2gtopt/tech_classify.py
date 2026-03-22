# SPDX-License-Identifier: BSD-3-Clause
"""Shared generator technology classification constants.

Used by plp2gtopt, gtopt_check_json, gtopt_check_output, and
gtopt_diagram to consistently group generator types into categories
for indicators and reporting.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Category → type string sets
# ---------------------------------------------------------------------------

#: Hydro types (reservoir + run-of-river + small + pumped)
HYDRO_TYPES: frozenset[str] = frozenset(
    {
        # PLP raw types
        "embalse",
        "serie",
        "pasada",
        # Refined tech types
        "hydro_reservoir",
        "hydro_ror",
        "hydro_small",
        "hydro_pumped",
    }
)

#: Thermal / fossil types
THERMAL_TYPES: frozenset[str] = frozenset(
    {
        "termica",
        "thermal",
        "gas",
        "coal",
        "diesel",
        "nuclear",
        "biomass",
        "geothermal",
    }
)

#: Renewable types (non-hydro)
RENEWABLE_TYPES: frozenset[str] = frozenset(
    {
        "solar",
        "wind",
        "csp",
        "renewable",
    }
)

#: Storage types
STORAGE_TYPES: frozenset[str] = frozenset(
    {
        "bateria",
        "battery",
    }
)

#: Curtailment / virtual demand
CURTAILMENT_TYPES: frozenset[str] = frozenset(
    {
        "falla",
        "curtailment",
    }
)

#: All known types (for validation)
ALL_KNOWN_TYPES: frozenset[str] = (
    HYDRO_TYPES
    | THERMAL_TYPES
    | RENEWABLE_TYPES
    | STORAGE_TYPES
    | CURTAILMENT_TYPES
    | frozenset({"unknown", "modular"})
)

# ---------------------------------------------------------------------------
# PLP raw type → broad category (for comparison tables)
# ---------------------------------------------------------------------------

#: Map PLP sub-type counts back to PLP categories.
#: Used by _comparison.py to match new tech types against PLP counts.
PLP_CATEGORY_MAP: dict[str, str] = {
    # Hydro reservoir
    "embalse": "embalse",
    "hydro_reservoir": "embalse",
    "hydro_pumped": "embalse",
    # Hydro series / run-of-river
    "serie": "serie",
    "hydro_ror": "serie",
    "hydro_small": "serie",
    # Pasada (only the raw PLP type — auto-detected pasada become other types)
    "pasada": "pasada",
    # Thermal (includes all fossil + renewables for PLP comparison)
    "termica": "termica",
    "thermal": "termica",
    "gas": "termica",
    "coal": "termica",
    "diesel": "termica",
    "nuclear": "termica",
    "biomass": "termica",
    "geothermal": "termica",
    "solar": "termica",
    "wind": "termica",
    "csp": "termica",
    "renewable": "termica",
}


def classify_type(gtype: str) -> str:
    """Return the broad category for a generator type string.

    Categories: ``"hydro"``, ``"thermal"``, ``"renewable"``,
    ``"storage"``, ``"curtailment"``, or ``"other"``.
    """
    gtype = gtype.lower()
    if gtype in HYDRO_TYPES:
        return "hydro"
    if gtype in THERMAL_TYPES:
        return "thermal"
    if gtype in RENEWABLE_TYPES:
        return "renewable"
    if gtype in STORAGE_TYPES:
        return "storage"
    if gtype in CURTAILMENT_TYPES:
        return "curtailment"
    return "other"


# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

#: Human-readable labels for diagram/report display
TYPE_DISPLAY_LABELS: dict[str, str] = {
    "hydro_reservoir": "Hydro (reservoir)",
    "hydro_ror": "Hydro (run-of-river)",
    "hydro_small": "Hydro (small/mini)",
    "hydro_pumped": "Hydro (pumped storage)",
    "thermal": "Thermal",
    "gas": "Gas (natural gas / CC)",
    "coal": "Coal",
    "diesel": "Diesel / fuel oil",
    "nuclear": "Nuclear",
    "biomass": "Biomass / biogas",
    "geothermal": "Geothermal",
    "solar": "Solar PV",
    "wind": "Wind",
    "csp": "Concentrated solar (CSP)",
    "renewable": "Renewable (unspecified)",
    "battery": "Battery storage",
    "curtailment": "Load shedding",
    # PLP raw types
    "embalse": "Hydro (reservoir)",
    "serie": "Hydro (series)",
    "pasada": "Hydro (run-of-river)",
    "termica": "Thermal",
    "bateria": "Battery",
    "falla": "Load shedding",
}


#: Color mapping for diagrams (type → CSS/hex color)
TYPE_COLORS: dict[str, str] = {
    "hydro_reservoir": "#1f77b4",  # blue
    "hydro_ror": "#17becf",  # cyan
    "hydro_small": "#9edae5",  # light cyan
    "hydro_pumped": "#aec7e8",  # light blue
    "embalse": "#1f77b4",
    "serie": "#17becf",
    "pasada": "#9edae5",
    "thermal": "#ff7f0e",  # orange
    "termica": "#ff7f0e",
    "gas": "#ffbb78",  # light orange
    "coal": "#8c564b",  # brown
    "diesel": "#d62728",  # red
    "nuclear": "#9467bd",  # purple
    "biomass": "#2ca02c",  # green
    "geothermal": "#bcbd22",  # olive
    "solar": "#f7b731",  # yellow/gold
    "wind": "#20bf6b",  # emerald
    "csp": "#f7b731",
    "renewable": "#98df8a",  # light green
    "battery": "#e377c2",  # pink
    "bateria": "#e377c2",
    "curtailment": "#7f7f7f",  # gray
    "falla": "#7f7f7f",
}


def type_label(gtype: str) -> str:
    """Return a human-readable label for a generator type."""
    return TYPE_DISPLAY_LABELS.get(gtype.lower(), gtype)


def type_color(gtype: str) -> str:
    """Return a CSS/hex color for a generator type."""
    return TYPE_COLORS.get(gtype.lower(), "#c7c7c7")
