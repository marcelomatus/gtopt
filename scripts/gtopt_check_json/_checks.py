# SPDX-License-Identifier: BSD-3-Clause
"""Library of validation checks for gtopt JSON planning files.

Each public ``check_*`` function accepts a *planning* dict and returns a list
of :class:`Finding` objects.  The :func:`run_all_checks` function orchestrates
every check and returns a combined list.

Implementation is split across domain-specific sub-modules:

- :mod:`._checks_common`      -- Finding data model and shared helpers
- :mod:`._general_checks`     -- Uniqueness, SDDP options, boundary cuts, AI
- :mod:`._reference_checks`   -- Element reference validation
- :mod:`._topology_checks`    -- Bus connectivity and topology
- :mod:`._hydro_checks`       -- Hydro, cascade, battery, capacity adequacy

This module re-exports every public name so that existing imports from
``gtopt_check_json._checks`` continue to work unchanged.
"""

from typing import Any

# -- Re-export data model ---------------------------------------------------
from gtopt_check_json._checks_common import (
    Finding,
    Severity,
    _ELEMENT_ARRAYS,
    _build_uid_name_maps,
    _extract_scalar_values,
    _get_name_set,
    _get_uid_set,
    _resolve_bus_lookup,
    _resolve_bus_ref,
    _resolve_uid_ref,
)

# -- Re-export general checks -----------------------------------------------
from gtopt_check_json._general_checks import (
    _build_state_variable_names,
    check_ai_system_analysis,
    check_boundary_cuts,
    check_cascade_solver_type,
    check_demand_lmax_nonneg,
    check_name_uniqueness,
    check_sddp_options,
    check_simulation_mode,
    check_uid_uniqueness,
)

# -- Re-export hydro / cascade checks ---------------------------------------
from gtopt_check_json._hydro_checks import (
    check_affluent_nonneg,
    check_battery_efficiency,
    check_capacity_adequacy,
    check_cascade_levels,
    check_seepage_at_vmin,
)

# -- Re-export reference checks ---------------------------------------------
from gtopt_check_json._reference_checks import (
    check_element_references,
)

# -- Re-export topology checks ----------------------------------------------
from gtopt_check_json._topology_checks import (
    IslandInfo,
    _BusAnalysis,
    _bus_connectivity_analysis,
    analyse_bus_islands,
    check_bus_connectivity,
    check_unreferenced_elements,
)

# ---------------------------------------------------------------------------
# Registry and runner
# ---------------------------------------------------------------------------

# Registry of all checks: (check_id, function, needs_ai)
_CHECK_REGISTRY: list[tuple[str, Any, bool]] = [
    ("uid_uniqueness", check_uid_uniqueness, False),
    ("name_uniqueness", check_name_uniqueness, False),
    ("demand_lmax_nonneg", check_demand_lmax_nonneg, False),
    ("affluent_nonneg", check_affluent_nonneg, False),
    ("element_references", check_element_references, False),
    ("bus_connectivity", check_bus_connectivity, False),
    ("unreferenced_elements", check_unreferenced_elements, False),
    ("capacity_adequacy", check_capacity_adequacy, False),
    ("battery_efficiency", check_battery_efficiency, False),
    ("seepage_at_vmin", check_seepage_at_vmin, False),
    ("cascade_levels", check_cascade_levels, False),
    ("simulation_mode", check_simulation_mode, False),
    ("sddp_options", check_sddp_options, False),
    ("cascade_solver_type", check_cascade_solver_type, False),
    ("boundary_cuts", check_boundary_cuts, False),
    ("ai_system_analysis", check_ai_system_analysis, True),
]


def run_all_checks(
    planning: dict[str, Any],
    enabled_checks: set[str] | None = None,
    ai_options: Any = None,
    base_dir: str = "",
) -> list[Finding]:
    """Run all enabled checks and return combined findings.

    Parameters
    ----------
    planning:
        The merged planning dict (loaded from JSON).
    enabled_checks:
        Set of check IDs to run.  ``None`` means all non-AI checks.
    ai_options:
        AI options for the AI system analysis check.  ``None`` disables it.
    base_dir:
        Case directory for resolving relative file paths.
    """
    findings: list[Finding] = []

    # Checks that accept a base_dir keyword argument.
    _NEEDS_BASE_DIR = {"boundary_cuts"}

    for check_id, check_fn, needs_ai in _CHECK_REGISTRY:
        if enabled_checks is not None and check_id not in enabled_checks:
            continue
        if needs_ai:
            findings.extend(check_fn(planning, ai_options=ai_options))
        elif check_id in _NEEDS_BASE_DIR:
            findings.extend(check_fn(planning, base_dir=base_dir))
        else:
            findings.extend(check_fn(planning))

    return findings


# Ensure all re-exported names are visible to static analysis and wildcard
# imports.
__all__ = [
    "Finding",
    "IslandInfo",
    "Severity",
    "_BusAnalysis",
    "_CHECK_REGISTRY",
    "_ELEMENT_ARRAYS",
    "_build_state_variable_names",
    "_build_uid_name_maps",
    "_bus_connectivity_analysis",
    "_extract_scalar_values",
    "_get_name_set",
    "_get_uid_set",
    "_resolve_bus_lookup",
    "_resolve_bus_ref",
    "_resolve_uid_ref",
    "analyse_bus_islands",
    "check_affluent_nonneg",
    "check_ai_system_analysis",
    "check_battery_efficiency",
    "check_boundary_cuts",
    "check_bus_connectivity",
    "check_capacity_adequacy",
    "check_cascade_levels",
    "check_cascade_solver_type",
    "check_demand_lmax_nonneg",
    "check_element_references",
    "check_name_uniqueness",
    "check_sddp_options",
    "check_seepage_at_vmin",
    "check_simulation_mode",
    "check_uid_uniqueness",
    "check_unreferenced_elements",
    "run_all_checks",
]
