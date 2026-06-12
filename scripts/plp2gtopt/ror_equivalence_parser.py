# -*- coding: utf-8 -*-

"""Parser for the RoR-as-reservoirs equivalence CSV file.

The reusable CSV parsing core (:class:`RorSpec`,
:func:`parse_ror_equivalence_file`, :func:`parse_ror_selection`) now lives
in ``gtopt_expand.ror_expand``.  This module re-exports those symbols for
backward compatibility and adds the PLP-specific helpers that depend on
parsed central data:

* :func:`resolve_ror_reservoir_spec` — resolves a selection against PLP
  central data (validates type, bus, efficiency, pmax cross-check).
* :func:`pasada_unscale_map` — builds the ``{name: 1/pf}`` unscale map
  for promoted pasada centrals.
"""

from __future__ import annotations

import logging
from typing import Any, Dict, List, Mapping

from gtopt_expand.ror_expand import (  # noqa: F401
    PMAX_MATCH_TOL,
    RorSpec,
    parse_ror_equivalence_file,
    parse_ror_selection,
)

_ELIGIBLE_TYPES: frozenset[str] = frozenset({"pasada", "serie"})

_logger = logging.getLogger(__name__)


def resolve_ror_reservoir_spec(
    options: Mapping[str, Any],
    central_items: List[Dict[str, Any]],
) -> Dict[str, RorSpec]:
    """Resolve the ``--ror-as-reservoirs`` selection against the PLP case.

    Pure helper shared by ``JunctionWriter`` (to emit daily-cycle
    reservoirs) and ``AflceWriter`` (to un-scale the discharge inflow
    of promoted pasada centrals).

    Args:
        options: The gtopt writer options dict.  Reads
            ``ror_as_reservoirs`` (selection string) and
            ``ror_as_reservoirs_file`` (CSV path).
        central_items: List of central dicts from the active PLP case.

    Returns:
        ``{name: RorSpec}`` for every eligible promoted central.
        Empty dict when the feature is disabled.

    Raises:
        ValueError: on any validation failure.
    """
    raw_selection = options.get("ror_as_reservoirs")
    selection = parse_ror_selection(raw_selection)
    csv_path = options.get("ror_as_reservoirs_file")

    if selection is None:
        return {}

    if csv_path is None:
        from ._parsers import DEFAULT_ROR_RESERVOIRS_FILE

        csv_path = DEFAULT_ROR_RESERVOIRS_FILE

    from pathlib import Path

    whitelist = parse_ror_equivalence_file(Path(csv_path))

    if selection:
        unknown = sorted(selection - whitelist.keys())
        if unknown:
            raise ValueError(
                f"--ror-as-reservoirs: central(s) not in whitelist "
                f"CSV '{csv_path}': {unknown}"
            )
        requested = {name: whitelist[name] for name in selection}
    else:
        requested = dict(whitelist)

    items_by_name = {c["name"]: c for c in central_items}
    resolved: Dict[str, RorSpec] = {}
    for name, spec in requested.items():
        central = items_by_name.get(name)
        if central is None:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' listed in the "
                f"whitelist is not present in the current PLP case"
            )
        ctype = central.get("type", "")
        if ctype not in _ELIGIBLE_TYPES:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has type "
                f"'{ctype}', expected one of {sorted(_ELIGIBLE_TYPES)}"
            )
        if central.get("bus", 0) <= 0:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has bus<=0 "
                f"so no turbine would drain the reservoir"
            )
        if float(central.get("efficiency", 0.0) or 0.0) <= 0.0:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has "
                f"efficiency<=0; cannot drain a daily-cycle reservoir"
            )
        if spec.pmax_mw is not None:
            plp_pmax = float(central.get("pmax", 0.0) or 0.0)
            if plp_pmax > 0.0:
                rel = abs(spec.pmax_mw - plp_pmax) / plp_pmax
                if rel > PMAX_MATCH_TOL:
                    _logger.warning(
                        "--ror-as-reservoirs: pmax mismatch for '%s' — "
                        "CSV=%.3f MW vs PLP=%.3f MW (relative gap %.1f%% "
                        "> %.0f%% tolerance); check for name collision "
                        "or stale worksheet",
                        name,
                        spec.pmax_mw,
                        plp_pmax,
                        rel * 100.0,
                        PMAX_MATCH_TOL * 100.0,
                    )
            else:
                _logger.warning(
                    "--ror-as-reservoirs: cannot cross-check pmax for "
                    "'%s' — PLP pmax is missing or zero; CSV says "
                    "pmax_mw=%.3f MW",
                    name,
                    spec.pmax_mw,
                )
        resolved[name] = spec

    _logger.info(
        "RoR-as-reservoirs: promoting %d central(s) to daily-cycle reservoirs: %s",
        len(resolved),
        sorted(resolved),
    )
    return resolved


def pasada_unscale_map(
    resolved: Mapping[str, RorSpec],
    central_items: List[Dict[str, Any]],
) -> Dict[str, float]:
    """Build the ``{name: 1/production_factor}`` map for promoted pasadas.

    PLP stores pasada discharge as ``physical_flow × real_production_factor``
    in MW-equivalent m³/s.  To promote a pasada to a daily-cycle
    volume-reservoir, the aflce inflow must be divided by the real
    production factor so the reservoir balance is in physical m³/s.

    Serie centrals are **not** included: PLP stores serie efficiency as
    the real MW/(m³/s) and the aflce flow is already physical.
    """
    if not resolved:
        return {}
    items_by_name = {c["name"]: c for c in central_items}
    scale: Dict[str, float] = {}
    for name, spec in resolved.items():
        central = items_by_name.get(name)
        if central is None:
            continue
        if central.get("type", "") != "pasada":
            continue
        scale[name] = 1.0 / spec.production_factor
    return scale
