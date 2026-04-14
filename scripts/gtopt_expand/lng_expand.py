# -*- coding: utf-8 -*-

"""LNG terminal expansion: ``lng.json`` → gtopt ``LngTerminal`` entities.

This module reads the canonical ``lng.json`` intermediate file produced by
``plp2gtopt`` (via :class:`~plp2gtopt.gnl_parser.GnlParser`) and emits a
gtopt-compatible system fragment containing ``lng_terminal_array`` entries.

Unlike the irrigation agreements, LNG expansion is a straightforward field
mapping — no Jinja2 templates are needed.

PLP field mapping
-----------------

.. list-table::
   :header-rows: 1

   * - PLP field
     - gtopt ``LngTerminal`` field
     - Notes
   * - VMax
     - ``emax``
     - m³
   * - Vini
     - ``eini``
     - m³
   * - CVer
     - ``spillway_cost``
     - $/m³
   * - CAlm
     - ``ecost``
     - $/m³ (PLP stores $/m³/day; the ``LngTerminalLP`` applies time scaling)
   * - GnlRen × Efficiency
     - ``generators[].heat_rate``
     - ``1 / (GnlRen × Efficiency × 3.6)`` [m³_LNG/MWh]
   * - Deliveries
     - ``delivery``
     - per-stage total volume [m³]
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

#: MWh → GJ conversion factor (1 MWh = 3.6 GJ).
_MWH_TO_GJ = 3.6


def _compute_heat_rate(gnlren: float, efficiency: float) -> float:
    """Compute the gtopt heat_rate [m³_LNG/MWh] from PLP efficiencies.

    PLP models fuel consumption as::

        V_consumed = P × Δt / (GnlRen × Efficiency × 3600)

    where the ``3600`` is really ``3.6 × 1000`` (MWh→GJ × m³→km³ scaling
    artifact).  gtopt uses a direct heat_rate [m³_LNG/MWh]::

        heat_rate = 1.0 / (GnlRen × Efficiency × 3.6)
    """
    product = gnlren * efficiency * _MWH_TO_GJ
    if product <= 0.0:
        raise ValueError(
            f"Invalid GnlRen×Efficiency product: {gnlren} × {efficiency}"
            f" = {gnlren * efficiency} (must be > 0)"
        )
    return 1.0 / product


def _build_delivery_schedule(
    deliveries: list[dict[str, Any]],
    num_stages: int,
) -> list[float]:
    """Build a per-stage delivery array from sparse (stage, volume) entries.

    Stages without deliveries get 0.0.  Stage indices in ``deliveries`` are
    1-based (PLP convention).
    """
    schedule = [0.0] * num_stages
    for entry in deliveries:
        stage_idx = int(entry["stage"])
        if 1 <= stage_idx <= num_stages:
            schedule[stage_idx - 1] = float(entry["volume"])
    return schedule


def expand_terminal(
    terminal: dict[str, Any],
    uid: int,
    num_stages: int,
) -> dict[str, Any]:
    """Convert a single PLP terminal dict to a gtopt ``LngTerminal`` entity.

    Parameters
    ----------
    terminal:
        One element of ``lng.json["terminals"]``.
    uid:
        Unique ID to assign to this terminal.
    num_stages:
        Total number of planning stages (for delivery schedule sizing).

    Returns
    -------
    dict
        A gtopt ``LngTerminal`` JSON-compatible dict.
    """
    gnlren = float(terminal["gnlren"])
    generators = []
    for gen in terminal.get("generators", []):
        efficiency = float(gen["efficiency"])
        generators.append(
            {
                "generator": gen["name"],
                "heat_rate": _compute_heat_rate(gnlren, efficiency),
            }
        )

    deliveries = terminal.get("deliveries", [])
    delivery: float | list[float] | None = None
    if deliveries:
        delivery = _build_delivery_schedule(deliveries, num_stages)

    result: dict[str, Any] = {
        "uid": uid,
        "name": terminal["name"],
        "emax": float(terminal["vmax"]),
        "eini": float(terminal["vini"]),
        "spillway_cost": float(terminal["cver"]),
        "use_state_variable": True,
    }

    # Optional costs — only emit when non-zero
    calm = float(terminal.get("calm", 0.0))
    if calm > 0.0:
        result["ecost"] = calm

    if delivery is not None:
        result["delivery"] = delivery

    if generators:
        result["generators"] = generators

    return result


def expand_lng(
    config: dict[str, Any],
    num_stages: int,
    uid_start: int = 1,
) -> dict[str, Any]:
    """Expand an ``lng.json`` config into a gtopt system fragment.

    Parameters
    ----------
    config:
        The full ``lng.json`` structure (``{"terminals": [...]}``).
    num_stages:
        Total number of planning stages.
    uid_start:
        First UID to assign to LNG terminals (auto-incremented).

    Returns
    -------
    dict
        A system fragment: ``{"lng_terminal_array": [...]}``.
    """
    terminals = config.get("terminals", [])
    lng_array = []
    for idx, terminal in enumerate(terminals):
        uid = uid_start + idx
        lng_array.append(expand_terminal(terminal, uid, num_stages))
    return {"lng_terminal_array": lng_array}


def expand_lng_from_file(
    input_path: str | Path,
    num_stages: int,
    uid_start: int = 1,
) -> dict[str, Any]:
    """Read ``lng.json`` from disk and expand into gtopt entities.

    Parameters
    ----------
    input_path:
        Path to the canonical ``lng.json`` file.
    num_stages:
        Total number of planning stages.
    uid_start:
        First UID to assign to LNG terminals.

    Returns
    -------
    dict
        A system fragment: ``{"lng_terminal_array": [...]}``.
    """
    with open(input_path, "r", encoding="utf-8") as fh:
        config = json.load(fh)
    return expand_lng(config, num_stages, uid_start)
