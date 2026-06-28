"""Build a gtopt JSON planning from a parsed PSS/E case.

The output schema matches the multi-bus reference cases under
``cases/bat_4b`` and ``cases/ieee_14b`` — three top-level keys
``options`` / ``simulation`` / ``system`` — so ``gtopt --lp-only``
ingests it directly.

A PSS/E ``.raw`` is a single power-flow snapshot, so the planning is a
**single stage / single block / single scenario** DC OPF:

* every in-service PSS/E bus becomes a gtopt ``Bus`` (referenced by the
  synthetic name ``b<number>``);
* branches and transformers become ``Line`` entries carrying the
  per-unit reactance (3-winding transformers expand to a synthetic star
  bus + three lines);
* generators become zero-information-cost ``Generator`` entries — PSS/E
  has no economic data, so a single configurable ``gcost`` is applied;
* loads become ``Demand`` entries with the active power ``PL`` as an
  inline ``lmax``.

``use_kirchhoff = true`` turns this into a proper DC OPF; gtopt
auto-pins one reference bus per electrical island, so no slack handling
is required here.
"""

from __future__ import annotations

import logging
from typing import Any

from gtopt_shared.json_planning import (  # noqa: F401  pylint: disable=unused-import
    write_planning,  # re-exported for back-compat
)

from .entities import BusSpec, PsseCase
from .raw_parser import floor_reactance, rating_to_tmax


logger = logging.getLogger(__name__)


# Synthetic-bus numbering for 3-winding transformer star points.  Real
# PSS/E bus numbers top out well below this offset, so star buses never
# collide with them.
_STAR_BUS_OFFSET = 9_000_000

# Single-snapshot block duration [h].  With one block, ``lmax`` in MW
# equals the energy over the block and the objective is $/h.
_BLOCK_HOURS = 1.0


def bus_ref(number: int) -> str:
    """Default gtopt bus reference name for a PSS/E bus number."""
    return f"b{number}"


def build_bus_names(
    buses: list[BusSpec], codes: dict[str, str] | None
) -> dict[int, str]:
    """Build the ``bus number -> gtopt reference name`` map.

    Without a Nomenclatura table every bus is named ``b<number>``.  With
    one, the PSS/E bus name's prefix is expanded to a human-readable form
    (``AGU-230`` -> ``Aguacapa-230``); collisions are disambiguated by
    appending the bus number so references stay unique.
    """
    if not codes:
        return {b.number: bus_ref(b.number) for b in buses}

    from .aux_data import expand_bus_name  # pylint: disable=import-outside-toplevel

    names: dict[int, str] = {}
    seen: dict[str, int] = {}
    for b in buses:
        label = expand_bus_name(b.name, codes)
        if label in seen:
            label = f"{label}_{b.number}"
        seen[label] = b.number
        names[b.number] = label
    return names


def _ref(names: dict[int, str], number: int) -> str:
    """Reference name for a bus number (falls back to ``b<number>``)."""
    return names.get(number, bus_ref(number))


def build_options(
    *,
    use_single_bus: bool,
    demand_fail_cost: float,
    scale_objective: float,
) -> dict[str, Any]:
    """Top-level ``options`` block (DC OPF defaults)."""
    return {
        "annual_discount_rate": 0.0,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": {
            "use_single_bus": use_single_bus,
            "use_kirchhoff": not use_single_bus,
            "demand_fail_cost": demand_fail_cost,
            "scale_objective": scale_objective,
        },
    }


def build_simulation() -> dict[str, Any]:
    """Single stage / block / scenario ``simulation`` block."""
    return {
        "block_array": [{"uid": 1, "duration": _BLOCK_HOURS}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    }


def build_buses(
    buses: list[BusSpec], names: dict[int, str]
) -> tuple[list[dict[str, Any]], set[int]]:
    """Map in-service buses to gtopt ``bus_array`` entries.

    Returns ``(bus_array, live_bus_numbers)`` where ``live_bus_numbers``
    is the set of bus numbers that survived (isolated buses dropped) —
    used to filter dangling generator / load / line references.
    """
    bus_array: list[dict[str, Any]] = []
    live: set[int] = set()
    for bus in buses:
        if bus.is_isolated:
            continue
        bus_array.append({"uid": bus.number, "name": _ref(names, bus.number)})
        live.add(bus.number)
    return bus_array, live


def build_generators(
    case: PsseCase,
    live: set[int],
    names: dict[int, str],
    *,
    gcost: float,
    pss_names: dict[int, str] | None = None,
    merit_rank: dict[str, int] | None = None,
    gcost_step: float = 1.0,
) -> tuple[list[dict[str, Any]], int, int]:
    """Map in-service generators to gtopt ``generator_array`` entries.

    Generators with ``pmax <= 0`` (e.g. synchronous condensers) or on a
    dropped bus are skipped.

    When an LDM merit order is supplied (``merit_rank`` keyed by
    upper-cased PSS/E bus name), a matched generator gets
    ``gcost + rank * gcost_step`` so the DC OPF dispatches in merit
    order; unmatched generators are placed after the whole merit list
    (``gcost + len(merit) * gcost_step``).  Without a merit order every
    generator gets the uniform ``gcost``.

    Returns ``(generator_array, n_merit_matched, n_total)``.
    """
    merit_rank = merit_rank or {}
    pss_names = pss_names or {}
    unmatched_cost = gcost + len(merit_rank) * gcost_step

    out: list[dict[str, Any]] = []
    matched = 0
    uid = 1
    for gen in case.gens:
        if gen.status != 1 or gen.bus not in live or gen.pmax <= 0.0:
            continue
        if merit_rank:
            rank = merit_rank.get(pss_names.get(gen.bus, "").upper())
            if rank is None:
                g_cost = unmatched_cost
            else:
                g_cost = gcost + rank * gcost_step
                matched += 1
        else:
            g_cost = gcost
        pmin = max(0.0, min(gen.pmin, gen.pmax))
        out.append(
            {
                "uid": uid,
                "name": f"g{gen.bus}_{gen.ident}".strip(),
                "bus": _ref(names, gen.bus),
                "pmin": pmin,
                "pmax": gen.pmax,
                "gcost": g_cost,
                "capacity": gen.pmax,
            }
        )
        uid += 1
    return out, matched, len(out)


def build_demands(
    case: PsseCase, live: set[int], names: dict[int, str]
) -> list[dict[str, Any]]:
    """Map in-service positive loads to gtopt ``demand_array`` entries."""
    out: list[dict[str, Any]] = []
    uid = 1
    for load in case.loads:
        if load.status != 1 or load.bus not in live or load.pl <= 0.0:
            continue
        out.append(
            {
                "uid": uid,
                "name": f"d{load.bus}_{load.ident}".strip(),
                "bus": _ref(names, load.bus),
                "lmax": [[load.pl]],
            }
        )
        uid += 1
    return out


def _line(
    uid: int,
    name: str,
    bus_a: int,
    bus_b: int,
    reactance: float,
    tmax: float,
    names: dict[int, str],
) -> dict[str, Any]:
    """Build one gtopt ``line_array`` entry (symmetric tmax)."""
    return {
        "uid": uid,
        "name": name,
        "bus_a": _ref(names, bus_a),
        "bus_b": _ref(names, bus_b),
        "reactance": floor_reactance(reactance),
        "tmax_ab": tmax,
        "tmax_ba": tmax,
    }


def build_lines(
    case: PsseCase, live: set[int], names: dict[int, str], *, rating_set: str = "A"
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Map branches + transformers to gtopt ``line_array`` entries.

    3-winding transformers expand to a synthetic star bus plus three
    lines using the star-equivalent reactances
    ``Z1 = ½(X12 + X31 − X23)`` etc.

    Returns ``(line_array, extra_bus_array)`` — ``extra_bus_array``
    holds the synthetic star buses created for 3-winding transformers.
    """
    lines: list[dict[str, Any]] = []
    extra_buses: list[dict[str, Any]] = []
    uid = 1

    for br in case.branches:
        if br.status != 1 or br.from_bus not in live or br.to_bus not in live:
            continue
        lines.append(
            _line(
                uid,
                f"l{br.from_bus}_{br.to_bus}_{br.ckt}".strip(),
                br.from_bus,
                br.to_bus,
                br.x,
                rating_to_tmax(br.rating(rating_set)),
                names,
            )
        )
        uid += 1

    star_idx = 0
    for tr in case.transformers:
        if tr.status == 0:
            continue
        if tr.windings == 2:
            if tr.bus_i not in live or tr.bus_j not in live:
                continue
            lines.append(
                _line(
                    uid,
                    f"t{tr.bus_i}_{tr.bus_j}_{tr.ckt}".strip(),
                    tr.bus_i,
                    tr.bus_j,
                    tr.x12,
                    rating_to_tmax(tr.winding_rating(1, rating_set)),
                    names,
                )
            )
            uid += 1
            continue

        # 3-winding: build a star equivalent.
        if tr.bus_i not in live or tr.bus_j not in live or tr.bus_k not in live:
            continue
        z1 = 0.5 * (tr.x12 + tr.x31 - tr.x23)
        z2 = 0.5 * (tr.x12 + tr.x23 - tr.x31)
        z3 = 0.5 * (tr.x23 + tr.x31 - tr.x12)
        star_num = _STAR_BUS_OFFSET + star_idx
        star_idx += 1
        extra_buses.append({"uid": star_num, "name": _ref(names, star_num)})
        for end_bus, zx, winding in (
            (tr.bus_i, z1, 1),
            (tr.bus_j, z2, 2),
            (tr.bus_k, z3, 3),
        ):
            lines.append(
                _line(
                    uid,
                    f"t{tr.bus_i}_{tr.bus_j}_{tr.bus_k}_{tr.ckt}_w{end_bus}".strip(),
                    end_bus,
                    star_num,
                    zx,
                    rating_to_tmax(tr.winding_rating(winding, rating_set)),
                    names,
                )
            )
            uid += 1

    return lines, extra_buses


def build_planning(
    case: PsseCase,
    *,
    name: str,
    gcost: float = 10.0,
    gcost_step: float = 1.0,
    demand_fail_cost: float = 1000.0,
    scale_objective: float = 1000.0,
    single_bus: bool = False,
    rating_set: str = "A",
    nomenclatura: dict[str, str] | None = None,
    ldm_order: list[str] | None = None,
) -> dict[str, Any]:
    """Assemble the full gtopt planning dict from a parsed PSS/E case.

    Args:
        case: The parsed PSS/E case.
        name: Planning / system name.
        gcost: Base generation cost [$/MWh].  Uniform without an LDM
            merit order; the rank-0 cost with one (see ``ldm_order``).
        gcost_step: Per-merit-rank cost increment [$/MWh] (LDM mode).
        demand_fail_cost: Unserved-energy penalty [$/MWh].
        scale_objective: Objective scaling divisor for numerics.
        single_bus: When True, emit a copper-plate single-bus planning
            (no lines / reactances) — useful as a feasibility fallback.
        rating_set: Line/transformer rating set ``A`` (normal, default),
            ``B`` or ``C`` (emergency).
        nomenclatura: Optional ``code -> description`` map for
            human-readable bus names.
        ldm_order: Optional merit-ordered list of PSS/E unit codes (bus
            names) driving rank-based generation costs.

    Returns:
        The planning dict ready for :func:`write_planning`.
    """
    names = build_bus_names(case.buses, nomenclatura)
    pss_names = {b.number: b.name.strip() for b in case.buses}
    merit_rank = {code.upper(): i for i, code in enumerate(ldm_order or [])}

    bus_array, live = build_buses(case.buses, names)
    gen_array, n_matched, n_gen = build_generators(
        case,
        live,
        names,
        gcost=gcost,
        pss_names=pss_names,
        merit_rank=merit_rank,
        gcost_step=gcost_step,
    )
    demand_array = build_demands(case, live, names)
    if merit_rank:
        logger.info(
            "LDM merit costs: matched %d of %d generators (%d unmatched -> placed last)",
            n_matched,
            n_gen,
            n_gen - n_matched,
        )

    system: dict[str, Any] = {
        "name": name,
        "bus_array": bus_array,
        "generator_array": gen_array,
        "demand_array": demand_array,
    }

    if not single_bus:
        line_array, extra_buses = build_lines(case, live, names, rating_set=rating_set)
        bus_array.extend(extra_buses)
        system["line_array"] = line_array

    logger.info(
        "built planning '%s': %d buses, %d generators, %d demands, %d lines "
        "(single_bus=%s, rating_set=%s)",
        name,
        len(bus_array),
        len(gen_array),
        len(demand_array),
        len(system.get("line_array", [])),
        single_bus,
        rating_set,
    )

    return {
        "options": build_options(
            use_single_bus=single_bus,
            demand_fail_cost=demand_fail_cost,
            scale_objective=scale_objective,
        ),
        "simulation": build_simulation(),
        "system": system,
    }
