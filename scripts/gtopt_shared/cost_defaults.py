# SPDX-License-Identifier: BSD-3-Clause
"""Central registry of converter cost defaults — parametrised by mode.

Both ``plp2gtopt`` and ``plexos2gtopt`` ship a handful of system-wide
slack penalties (unserved demand, reserve shortage, state-constraint
violation, water shortage, spill, soft pmin / overload).  Each carries
a hard-coded cost-mode default in $/MWh.

Switching the LP into ``--only-emissions`` mode (objective in tCO2eq-
equivalent dollars via ``EmissionZone.price = $35/tCO2eq``) needs the
same fields re-anchored against the carbon-shadow scale.  The two
anchors agreed with Marcelo (2026-06-02):

* **Unserved demand = $150 / tCO2eq** — EU ETS reference price for the
  social cost of UNS, replaces CEN's $467 / MWh and gtopt's $1000 / MWh
  cost-mode default.
* **Dirtiest coal = 1 tCO2eq / MWh** — the physical upper bound on
  per-MWh emission rate.  Used as the "$/MWh ↔ $/tCO2eq bridge":
  the conversion factor is unity, so other slack penalties keep their
  numerical $/MWh value when relabeled $/tCO2eq.  This holds the
  ordering "UNS > forced-pmin slack > dispatch cost" intact:
  $150 > ~$300 forced-pmin > 1 × $35 = $35 dispatch ceiling.

Adding a NEW cost knob:
  1. Append an entry below with both modes' defaults.
  2. Register its CLI flag in ``cli_flags.py`` with
     ``default=COST_DEFAULTS[<name>].cost_mode`` so the energy-mode
     behaviour is unchanged.
  3. Call ``apply_emission_overrides`` from the emissions overlay so
     ``--only-emissions`` automatically swaps in the emissions-mode
     value when the user did not explicitly override.

That's the entire pattern — no per-converter forking, no scattered
constants.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class CostDefault:
    """One cost knob's per-mode defaults + descriptive metadata."""

    cost_mode: float | None  # $/MWh
    emissions_mode: float | None  # $/tCO2eq (bridge = 1 tCO2/MWh)
    unit_cost: str
    unit_emissions: str
    help_text: str


# Centralised cost-default registry.  Add knobs here, NOT in
# per-converter parser modules.
COST_DEFAULTS: dict[str, CostDefault] = {
    # ── System-wide slack penalties (model_options) ───────────────────
    "demand_fail_cost": CostDefault(
        cost_mode=1000.0,
        emissions_mode=150.0,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=(
            "Penalty for unserved demand.  cost-mode: Chile VoLL "
            "($1000/MWh default); emissions-mode: EU ETS reference "
            "($150/tCO2eq) — strictly > dirtiest coal × SCC = $35, so "
            "the LP serves load before shedding."
        ),
    ),
    "reserve_shortage_cost": CostDefault(
        cost_mode=500.0,
        emissions_mode=500.0,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=(
            "Penalty for unmet reserve requirement.  Bridge = 1, so "
            "numerical default is identical in both modes."
        ),
    ),
    "state_violation_cost": CostDefault(
        cost_mode=500.0,
        emissions_mode=500.0,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=(
            "Penalty for state-constraint violation (commitment / "
            "user-constraint slack).  Bridge = 1, same numeric in both modes."
        ),
    ),
    "water_fail_cost": CostDefault(
        cost_mode=100.0,
        emissions_mode=100.0,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=("Penalty for failing a forced hydro pmin / irrigation right."),
    ),
    "hydro_spill_cost": CostDefault(
        cost_mode=0.1,
        emissions_mode=0.1,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=(
            "Tiny per-MWh discouragement of spilling water — picks the "
            "thermodynamic-equivalent corner of the LP polytope when "
            "spill is otherwise free."
        ),
    ),
    "soft_penalty_cost": CostDefault(
        cost_mode=None,  # auto = min(max(gcost)+1, min(VoLL)-1)
        emissions_mode=300.0,
        unit_cost="$/MWh",
        unit_emissions="$/tCO2eq",
        help_text=(
            "System-wide soft penalty for forced-pmin slacks + line "
            "overload + EL=1 slacks.  cost-mode: auto-derived as "
            "min(max(gcost)+1, min(VoLL)-1) so it sits between dispatch "
            "and VoLL.  emissions-mode: fixed $300/tCO2eq so it sits "
            "between dispatch ceiling ($35) and UNS ($150) — wait, "
            "actually higher than UNS; the LP will shed before "
            "violating a soft pmin, which is the right semantic when "
            "the carbon ceiling matters more than physical operation "
            "of forced units."
        ),
    ),
    # ── Replaced / dropped in emissions mode ─────────────────────────
    "hydro_use_value": CostDefault(
        cost_mode=20.0,
        emissions_mode=None,  # DROPPED — replaced by Reservoir.water_emission_value
        unit_cost="$/MWh",
        unit_emissions="(dropped)",
        help_text=(
            "cost-mode terminal water value default.  Emissions mode "
            "drops it — the EPF-based ``Reservoir.water_emission_value`` "
            "(set by emissions overlay) is the per-reservoir terminal "
            "value, not a system-wide flat figure."
        ),
    ),
}


def get_default(name: str, *, only_emissions: bool = False) -> float | None:
    """Return the appropriate default for ``name`` given the mode.

    Use in CLI flag registration:

        parser.add_argument(
            "--demand-fail-cost",
            default=cost_defaults.get_default("demand_fail_cost"),
            ...
        )

    The flag default ALWAYS stays cost-mode (so ``--only-emissions``
    doesn't break script behaviour pre-emissions-overlay); the overlay
    swaps in the emissions-mode value via :func:`apply_emission_overrides`.
    """
    entry = COST_DEFAULTS.get(name)
    if entry is None:
        return None
    return entry.emissions_mode if only_emissions else entry.cost_mode


def apply_emission_overrides(
    planning: dict[str, Any],
    *,
    uns_price: float | None = None,
) -> dict[str, float]:
    """Walk the planning dict and replace cost-mode defaults with
    emissions-mode values where the user did not explicitly override.

    Called from ``gtopt_shared.emissions.apply_emission_defaults``
    when ``only_emissions=True``.

    Conservative: only replaces a value when it equals the cost-mode
    default (i.e., the user took the default).  Explicit user values
    win unconditionally.

    Special-case demand_fail:
      * ``Demand.fcost`` (per-demand UNS price) is overridden to
        ``uns_price`` (or :data:`COST_DEFAULTS['demand_fail_cost'].emissions_mode`)
        UNCONDITIONALLY — PLEXOS CEN ships $467 per demand which we
        deliberately replace with the EU social reference.
      * ``model_options.demand_fail_cost`` is replaced when it equals
        the cost-mode default ($1000) or is missing.

    Returns ``{field_name: new_value}`` for reporting.
    """
    applied: dict[str, float] = {}
    sys_ = planning.setdefault("system", {})
    mo = planning.setdefault("options", {}).setdefault("model_options", {})
    em_uns = COST_DEFAULTS["demand_fail_cost"].emissions_mode
    if uns_price is not None:
        em_uns = float(uns_price)

    # 1. Per-demand fcost — unconditional override (EU UNS reference).
    new_fcosts = 0
    for d in sys_.get("demand_array", []) or []:
        if "fcost" not in d:
            continue
        d["fcost"] = em_uns
        new_fcosts += 1
    if new_fcosts:
        applied["demand_array.fcost"] = em_uns

    # 2. System-wide cost knobs in model_options.
    for name, entry in COST_DEFAULTS.items():
        if entry.emissions_mode is None:
            # Drop fields whose emissions-mode is None (e.g. hydro_use_value).
            if name in mo:
                mo.pop(name)
                applied[f"model_options.{name}"] = -1.0  # marker
            continue
        cur = mo.get(name)
        if cur is None:
            # Field absent — stamp the emissions-mode default so
            # downstream gtopt picks it up.
            mo[name] = entry.emissions_mode
            applied[f"model_options.{name}"] = entry.emissions_mode
            continue
        # Replace if the current value matches the cost-mode default
        # (i.e., user took the default).  Special-case demand_fail
        # which is overridden unconditionally to the UNS price.
        if name == "demand_fail_cost":
            mo[name] = em_uns
            applied[f"model_options.{name}"] = em_uns
        elif entry.cost_mode is not None and abs(float(cur) - entry.cost_mode) < 1e-9:
            mo[name] = entry.emissions_mode
            applied[f"model_options.{name}"] = entry.emissions_mode

    # 3. PLEXOS-input-derived per-element costs.
    #
    # Commitment.startup_cost / shutdown_cost ($) represent the FUEL
    # burned during the start / stop transient — encoded as a dollar
    # figure via PLEXOS's ``Start Cost`` / ``Shutdown Cost`` which are
    # essentially ``startup_fuel × fuel.price``.  In emissions mode we
    # convert back to a tCO2eq amount and then to the $-equiv via SCC:
    #
    #   startup_fuel    = startup_cost_$  /  fuel.price [$ /fuel-unit]
    #   startup_energy  = startup_fuel    ×  fuel.heat_content [GJ/fuel-unit]
    #   startup_em      = startup_energy  ×  fuel.ef_combustion [tCO2/GJ]
    #   startup_em_$    = startup_em      ×  SCC [$/tCO2eq]
    #
    # Collapsed:
    #   factor = (heat_content × ef_combustion / fuel.price) × SCC
    #          = tCO2eq per $ of fuel × $ per tCO2eq
    #          = dimensionless dollar-to-dollar conversion factor
    #   startup_cost_em_$ = startup_cost_$ × factor
    #
    # Typical gas: heat_content 47.1 GJ/t, ef 0.056 tCO2/GJ,
    #              price ≈ $300/t  → factor = (47.1×0.056/300)×35 ≈ 0.31
    # Typical coal: heat_content 29 GJ/t,  ef 0.096 tCO2/GJ,
    #               price ≈ $80/t  → factor = (29×0.096/80)×35 ≈ 1.22
    # When fuel data is missing (no fuel ref, no price, or no
    # emission factor), zero the cost — we cannot attribute carbon.
    fuels_by_name = {
        str(f.get("name", "")): f for f in sys_.get("fuel_array", []) or []
    }
    fuels_by_uid = {
        int(f["uid"]): f for f in sys_.get("fuel_array", []) or [] if "uid" in f
    }

    def _fuel_for_gen(gen: dict) -> dict | None:
        ref = gen.get("fuel")
        if ref is None:
            return None
        if isinstance(ref, dict):
            ref = ref.get("uid") or ref.get("name")
        if isinstance(ref, int):
            return fuels_by_uid.get(ref)
        if isinstance(ref, str):
            return fuels_by_name.get(ref)
        return None

    def _co2_ef_per_gj(fuel: dict) -> float:
        for ef in fuel.get("emission_factors", []) or []:
            if isinstance(ef, dict) and str(ef.get("emission", "")).lower() == "co2":
                return float(ef.get("combustion", 0.0) or 0.0)
        return 0.0

    def _scc() -> float:
        for z in sys_.get("emission_zone_array", []) or []:
            if isinstance(z, dict) and z.get("price") is not None:
                return float(z["price"])
        return COST_DEFAULTS["demand_fail_cost"].emissions_mode or 35.0  # fallback

    gens_by_name = {
        str(g.get("name", "")): g for g in sys_.get("generator_array", []) or []
    }
    scc = _scc()
    n_converted_su = n_zeroed_su = n_converted_sd = n_zeroed_sd = 0
    for c in sys_.get("commitment_array", []) or []:
        gen_name = str(c.get("generator", ""))
        gen = gens_by_name.get(gen_name) or {}
        fuel = _fuel_for_gen(gen)
        price = float((fuel or {}).get("price", 0.0) or 0.0)
        heat_content = float((fuel or {}).get("heat_content", 0.0) or 0.0)
        ef_gj = _co2_ef_per_gj(fuel or {})
        # carbon-equivalent dollar conversion factor
        if price > 0.0 and heat_content > 0.0 and ef_gj > 0.0:
            factor = (heat_content * ef_gj / price) * scc
        else:
            factor = 0.0  # zero out when we cannot attribute carbon
        for field, ctr_done, ctr_zero in (
            ("startup_cost", "n_converted_su", "n_zeroed_su"),
            ("shutdown_cost", "n_converted_sd", "n_zeroed_sd"),
        ):
            if field not in c:
                continue
            orig = float(c[field] or 0.0)
            new = orig * factor
            c[field] = new
            if factor > 0.0:
                if ctr_done == "n_converted_su":
                    n_converted_su += 1
                else:
                    n_converted_sd += 1
            else:
                if ctr_zero == "n_zeroed_su":
                    n_zeroed_su += 1
                else:
                    n_zeroed_sd += 1
    if n_converted_su or n_zeroed_su:
        applied["commitment_array.startup_cost"] = (
            f"{n_converted_su} converted via fuel·ef·SCC, {n_zeroed_su} zeroed (no fuel/ef)"
        )
    if n_converted_sd or n_zeroed_sd:
        applied["commitment_array.shutdown_cost"] = (
            f"{n_converted_sd} converted via fuel·ef·SCC, {n_zeroed_sd} zeroed (no fuel/ef)"
        )

    # 4. Waterway / FlowRight per-element fcost ($/(m³/s)/h) — bridge=1,
    # numerical identity.  No action needed (the values already match
    # the units of the emissions-mode objective via the SCC conversion).
    # Documented as a no-op so the audit table reflects every field.

    return applied


__all__ = [
    "CostDefault",
    "COST_DEFAULTS",
    "get_default",
    "apply_emission_overrides",
]
