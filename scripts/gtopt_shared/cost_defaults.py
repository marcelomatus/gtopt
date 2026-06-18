# SPDX-License-Identifier: BSD-3-Clause
"""Central registry of converter cost defaults — parametrised by mode.

Both ``plp2gtopt`` and ``plexos2gtopt`` ship a handful of system-wide
slack penalties (unserved demand, reserve shortage, state-constraint
violation, water shortage, spill, soft pmin / overload, soft UC).
Each carries a hard-coded cost-mode default in $/MWh.

**Architecture** — all-tCO2 LP objective when ``--only-emissions``:

* ``EmissionZone.price = SCC`` (default 35.0 $/tCO2eq, Chile CNE) is
  kept as the SINGLE point where carbon is priced in $.  Every other
  cost field is stamped in **tCO2 / MWh** (or per natural unit) so
  the LP objective is dimensionally homogeneous in tCO2.  The C++
  side multiplies non-emission slacks by ``EmissionZone.price`` at
  LP-build time when ``is_emissions_objective()`` to yield $.

* The two physical anchors:

  - **Unserved demand = $150 / tCO2eq** (EU ETS social-cost reference)
    → in tCO2/MWh terms: ``150 / 35 ≈ 4.286 tCO2/MWh``.  Physical
    interpretation: 1 MWh of UNS forces ~4.286 tCO2 of off-grid
    backup-generator emissions — matches a "very bad house genset".

  - **Dirtiest coal = 1 tCO2 / MWh** — physical upper bound on per-MWh
    emission rate.  Used as the "$/MWh ↔ tCO2/MWh bridge" via SCC:
    ``cost_emissions = cost_dollar / SCC`` (numerically `/35` for
    Chile CNE default).

* The conversion preserves the strict ordering required for sane LP:

    UNS         (4.286 tCO2/MWh)     >  dispatch ceiling (dirtiest coal, 1)
    soft UC     (~285 tCO2/MWh)      >  UNS                              <—  shed-before-violate
    pmin slack  (~8.5 tCO2/MWh)      >  dispatch ceiling
    spill       (~0.003 tCO2/MWh)    <  dispatch ceiling                <—  small nudge

Adding a NEW cost knob: append an entry below with both modes'
defaults.  Both converters pick it up automatically — no per-converter
forking, no scattered hard-coded constants.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


# Default Chile CNE social cost of carbon (USD per tCO2eq).  Used as
# the bridge factor for converting $/MWh slack values to tCO2/MWh.
# Stamped on ``EmissionZone.price`` by the emissions overlay so the
# LP applies the conversion once at the EmissionZone level.
DEFAULT_SCC: float = 35.0


@dataclass(frozen=True)
class CostDefault:
    """One cost knob's per-mode defaults + descriptive metadata."""

    cost_mode: float | None  # $/MWh (or natural unit)
    emissions_mode: float | None  # tCO2 / MWh (or natural unit / SCC)
    unit_cost: str
    unit_emissions: str
    help_text: str


def _div_by_scc(x: float, scc: float = DEFAULT_SCC) -> float:
    """Convenience: $/MWh → tCO2/MWh via the SCC bridge."""
    return x / scc


# Centralised cost-default registry.  Add knobs here, NOT in
# per-converter parser modules.
COST_DEFAULTS: dict[str, CostDefault] = {
    # ── System-wide slack penalties (model_options) ───────────────────
    # All ``emissions_mode`` values are in tCO2 / MWh (the
    # "$ / MWh ÷ SCC" image of the cost-mode value).  The LP multiplies
    # by ``EmissionZone.price`` (default 35 $/tCO2eq) at build time to
    # bring everything back to $-equivalent units.
    "demand_fail_cost": CostDefault(
        cost_mode=1000.0,
        emissions_mode=150.0 / DEFAULT_SCC,  # ≈ 4.286 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text=(
            "Penalty for unserved demand.  cost-mode: Chile VoLL "
            "($1000/MWh default); emissions-mode: $150 EU social cost / "
            "$35 SCC = 4.286 tCO2eq/MWh — matches the emission rate of "
            "an inefficient off-grid 'house' backup genset, so UNS is "
            "strictly worse than dispatching the dirtiest utility coal "
            "(1.07 tCO2eq/MWh)."
        ),
    ),
    "reserve_shortage_cost": CostDefault(
        cost_mode=500.0,
        emissions_mode=_div_by_scc(500.0),  # ≈ 14.286 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text="Penalty for unmet reserve requirement.",
    ),
    "state_violation_cost": CostDefault(
        cost_mode=500.0,
        emissions_mode=_div_by_scc(500.0),  # ≈ 14.286 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text=(
            "Penalty for state-constraint violation (commitment / "
            "user-constraint slack)."
        ),
    ),
    "water_fail_cost": CostDefault(
        cost_mode=100.0,
        emissions_mode=_div_by_scc(100.0),  # ≈ 2.857 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text="Penalty for failing a forced hydro pmin / irrigation right.",
    ),
    "hydro_spill_cost": CostDefault(
        cost_mode=0.1,
        emissions_mode=_div_by_scc(0.1),  # ≈ 0.00286 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text=(
            "Tiny per-MWh discouragement of spilling water — picks the "
            "thermodynamic-equivalent corner of the LP polytope when "
            "spill is otherwise free."
        ),
    ),
    "soft_penalty_cost": CostDefault(
        cost_mode=None,  # auto = min(max(gcost)+1, min(VoLL)-1)
        emissions_mode=_div_by_scc(300.0),  # ≈ 8.571 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text=(
            "System-wide soft penalty for forced-pmin slacks + line "
            "overload + EL=1 slacks.  cost-mode: auto-derived; "
            "emissions-mode: ~8.6 tCO2eq/MWh — strictly > UNS (4.3) so "
            "LP sheds before violating a forced-pmin floor."
        ),
    ),
    "default_uc_penalty": CostDefault(
        cost_mode=10000.0,  # _SOFT_UC_DEFAULT_PENALTY (plexos2gtopt)
        emissions_mode=_div_by_scc(10000.0),  # ≈ 285.7 tCO2/MWh
        unit_cost="$/MWh",
        unit_emissions="tCO2eq/MWh",
        help_text=(
            "Default per-row penalty stamped on every UC under "
            "``--pampl-uc-mode soft`` when the PLEXOS UC carries no "
            "explicit Penalty property.  Sized to keep load-serving "
            "optimal vs UC violation (much > UNS)."
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
    """Return the appropriate default for ``name`` given the mode."""
    entry = COST_DEFAULTS.get(name)
    if entry is None:
        return None
    return entry.emissions_mode if only_emissions else entry.cost_mode


def apply_emission_overrides(
    planning: dict[str, Any],
    *,
    uns_price_dollar: float | None = None,
    scc: float = DEFAULT_SCC,
) -> dict[str, float | str]:
    """Walk the planning dict and replace cost-mode defaults with
    emissions-mode values (in tCO2 / unit).

    Called from ``gtopt_shared.emissions.apply_emission_defaults``
    when ``only_emissions=True``.

    ``uns_price_dollar`` overrides the EU UNS reference ($150).  ``scc``
    is the bridge factor (= ``EmissionZone.price`` stamped by the
    overlay; default 35 USD/tCO2eq Chile CNE).

    Conservative: only replaces a value when it equals the cost-mode
    default (i.e., the user took the default).  Special-cases:

    * ``Demand.fcost`` — unconditional override to ``uns_price_dollar
      / scc`` (PLEXOS CEN ships $467 per demand which we deliberately
      replace with the EU social reference).
    * ``model_options.demand_fail_cost`` — same unconditional override.
    * ``Commitment.startup/shutdown_cost`` — converted via fuel chain
      to tCO2 (NO SCC multiplier — the LP applies SCC at the
      EmissionZone level).
    * ``Reservoir.water_value`` / ``efin_cost`` — replaced with
      ``EPF · 0.5 · 0.95 · 277.78`` (tCO2/hm³, no SCC).

    Returns ``{field_name: new_value}`` for reporting.
    """
    applied: dict[str, float | str] = {}
    sys_ = planning.setdefault("system", {})
    mo = planning.setdefault("options", {}).setdefault("model_options", {})
    em_uns_dollar = uns_price_dollar
    if em_uns_dollar is None:
        # Recover from the registry's tCO2 value × SCC
        em_uns_dollar = (
            COST_DEFAULTS["demand_fail_cost"].emissions_mode or 4.286
        ) * scc
    em_uns_tco2 = em_uns_dollar / scc

    # 1. Per-demand fcost — unconditional override (EU UNS reference).
    n_fcost = 0
    for d in sys_.get("demand_array", []) or []:
        if "fcost" not in d:
            continue
        d["fcost"] = em_uns_tco2
        n_fcost += 1
    if n_fcost:
        applied["demand_array.fcost"] = em_uns_tco2

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
            mo[name] = entry.emissions_mode
            applied[f"model_options.{name}"] = entry.emissions_mode
            continue
        if name == "demand_fail_cost":
            mo[name] = em_uns_tco2
            applied[f"model_options.{name}"] = em_uns_tco2
        elif entry.cost_mode is not None and abs(float(cur) - entry.cost_mode) < 1e-9:
            mo[name] = entry.emissions_mode
            applied[f"model_options.{name}"] = entry.emissions_mode

    # 3. Commitment startup/shutdown via fuel-chain → tCO2.
    #
    # The $ cost encodes fuel burned during the transient:
    #   startup_em_tCO2 = startup_$  ×  heat_content [GJ/fu]
    #                     × ef_combustion [tCO2/GJ]
    #                     / fuel.price [$/fu]
    # NO SCC multiplier — the LP applies SCC at the EmissionZone level
    # (single point of carbon pricing).  Falls back to zero when fuel
    # data is missing (cannot attribute carbon).
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

    gens_by_name = {
        str(g.get("name", "")): g for g in sys_.get("generator_array", []) or []
    }
    n_su_conv = n_su_zero = n_sd_conv = n_sd_zero = 0
    for c in sys_.get("commitment_array", []) or []:
        gen_name = str(c.get("generator", ""))
        gen = gens_by_name.get(gen_name) or {}
        fuel = _fuel_for_gen(gen)
        price = float((fuel or {}).get("price", 0.0) or 0.0)
        heat_content = float((fuel or {}).get("heat_content", 0.0) or 0.0)
        ef_gj = _co2_ef_per_gj(fuel or {})
        # tCO2 per $ of fuel (no SCC — LP applies it at EmissionZone)
        if price > 0.0 and heat_content > 0.0 and ef_gj > 0.0:
            factor = heat_content * ef_gj / price
        else:
            factor = 0.0
        for field, conv_attr, zero_attr in (
            ("startup_cost", "n_su_conv", "n_su_zero"),
            ("shutdown_cost", "n_sd_conv", "n_sd_zero"),
        ):
            if field not in c:
                continue
            orig = float(c[field] or 0.0)
            c[field] = orig * factor  # tCO2 per startup/shutdown
            if factor > 0.0:
                if conv_attr == "n_su_conv":
                    n_su_conv += 1
                else:
                    n_sd_conv += 1
            else:
                if zero_attr == "n_su_zero":
                    n_su_zero += 1
                else:
                    n_sd_zero += 1
    if n_su_conv or n_su_zero:
        applied["commitment_array.startup_cost"] = (
            f"{n_su_conv} converted to tCO2 via fuel·ef, {n_su_zero} zeroed (no fuel/ef)"
        )
    if n_sd_conv or n_sd_zero:
        applied["commitment_array.shutdown_cost"] = (
            f"{n_sd_conv} converted to tCO2 via fuel·ef, {n_sd_zero} zeroed (no fuel/ef)"
        )

    # 4. Per-element $/MWh slacks — divide by SCC to land in tCO2/MWh.
    # Conservative: only touch fields whose value is plausibly a slack
    # cost.  The actual LP-build-time multiplication by SCC is a C++-side
    # follow-up (see Issue tracker) so today's LP would treat these as
    # $/MWh values an order of magnitude smaller than intended.
    def _scale_fcost(elem: dict) -> None:
        v = elem.get("fcost")
        if isinstance(v, (int, float)) and v > 0:
            elem["fcost"] = float(v) / scc

    n_ww = n_fl = n_fr = 0
    for w in sys_.get("waterway_array", []) or []:
        before = w.get("fcost")
        _scale_fcost(w)
        if w.get("fcost") != before:
            n_ww += 1
    for f in sys_.get("flow_array", []) or []:
        before = f.get("fcost")
        _scale_fcost(f)
        if f.get("fcost") != before:
            n_fl += 1
    for fr in sys_.get("flow_right_array", []) or []:
        v = fr.get("fcost")
        # FlowRight.fcost may be a list-of-lists (TB schedule); skip unless scalar
        if isinstance(v, (int, float)) and v > 0:
            fr["fcost"] = float(v) / scc
            n_fr += 1
    if n_ww:
        applied["waterway_array.fcost"] = f"{n_ww} scaled by 1/SCC"
    if n_fl:
        applied["flow_array.fcost"] = f"{n_fl} scaled by 1/SCC"
    if n_fr:
        applied["flow_right_array.fcost"] = f"{n_fr} scaled by 1/SCC"

    # 5. Junction.drain_cost ($/(m³/s)/h) — divide by SCC.
    n_drain = 0
    for j in sys_.get("junction_array", []) or []:
        v = j.get("drain_cost")
        if isinstance(v, (int, float)) and v > 0:
            j["drain_cost"] = float(v) / scc
            n_drain += 1
    if n_drain:
        applied["junction_array.drain_cost"] = f"{n_drain} scaled by 1/SCC"

    # 6. Per-UC penalty (PLEXOS soft UC) — divide by SCC.
    n_uc = 0
    for uc in sys_.get("user_constraint_array", []) or []:
        v = uc.get("penalty")
        if isinstance(v, (int, float)) and v > 0:
            uc["penalty"] = float(v) / scc
            n_uc += 1
    if n_uc:
        applied["user_constraint_array.penalty"] = f"{n_uc} scaled by 1/SCC"

    # 7. Fuel.max_offtake_cost / min_offtake_cost — divide by SCC.
    n_fmax = n_fmin = 0
    for f in sys_.get("fuel_array", []) or []:
        if (
            isinstance(f.get("max_offtake_cost"), (int, float))
            and f["max_offtake_cost"] > 0
        ):
            f["max_offtake_cost"] = float(f["max_offtake_cost"]) / scc
            n_fmax += 1
        if (
            isinstance(f.get("min_offtake_cost"), (int, float))
            and f["min_offtake_cost"] > 0
        ):
            f["min_offtake_cost"] = float(f["min_offtake_cost"]) / scc
            n_fmin += 1
    if n_fmax:
        applied["fuel_array.max_offtake_cost"] = f"{n_fmax} scaled by 1/SCC"
    if n_fmin:
        applied["fuel_array.min_offtake_cost"] = f"{n_fmin} scaled by 1/SCC"

    # 8. Per-Generator pmin_fcost ($/MWh) — divide by SCC.
    n_pmin = 0
    for g in sys_.get("generator_array", []) or []:
        v = g.get("pmin_fcost")
        if isinstance(v, (int, float)) and v > 0:
            g["pmin_fcost"] = float(v) / scc
            n_pmin += 1
    if n_pmin:
        applied["generator_array.pmin_fcost"] = f"{n_pmin} scaled by 1/SCC"

    # 9. Per-Line overload_penalty ($/MWh) — divide by SCC.
    n_ovr = 0
    for ln in sys_.get("line_array", []) or []:
        v = ln.get("overload_penalty")
        if isinstance(v, (int, float)) and v > 0:
            ln["overload_penalty"] = float(v) / scc
            n_ovr += 1
    if n_ovr:
        applied["line_array.overload_penalty"] = f"{n_ovr} scaled by 1/SCC"

    # 10. Battery.charge_cost / discharge_cost ($/MWh) — divide by SCC.
    n_bch = n_bdis = 0
    for b in sys_.get("battery_array", []) or []:
        if isinstance(b.get("charge_cost"), (int, float)) and b["charge_cost"] > 0:
            b["charge_cost"] = float(b["charge_cost"]) / scc
            n_bch += 1
        if (
            isinstance(b.get("discharge_cost"), (int, float))
            and b["discharge_cost"] > 0
        ):
            b["discharge_cost"] = float(b["discharge_cost"]) / scc
            n_bdis += 1
    if n_bch:
        applied["battery_array.charge_cost"] = f"{n_bch} scaled by 1/SCC"
    if n_bdis:
        applied["battery_array.discharge_cost"] = f"{n_bdis} scaled by 1/SCC"

    # 11. DecisionVariable.cost ($/unit) — divide by SCC.
    n_dv = 0
    for dv in sys_.get("decision_variable_array", []) or []:
        v = dv.get("cost")
        if isinstance(v, (int, float)) and v != 0:
            dv["cost"] = float(v) / scc
            n_dv += 1
    if n_dv:
        applied["decision_variable_array.cost"] = f"{n_dv} scaled by 1/SCC"

    # 12. PLEXOS-input-derived per-reservoir terminal water value (#521).
    #
    # The cost-mode ``Reservoir.water_value`` and ``Reservoir.efin_cost``
    # are $-per-hm³ values shipped by plexos2gtopt / plp2gtopt from
    # ``Hydro_StoWaterValues.csv`` / PLP ``efin_cost``.  In emissions
    # mode they need to be replaced by the per-reservoir EPF-based
    # terminal carbon value already stamped as ``water_emission_value``
    # (tCO2eq per (m³/s)·h) by commit 7b36f3728.
    #
    # Unit conversion: 1 hm³ released over 1 hour = 277.78 m³/s for
    # 1 h, so the per-hm³ figure is ``water_emission_value × 277.78``
    # (tCO2eq per hm³).  Unlike the slack costs above, the LP applies
    # SCC at the EmissionZone — so NO multiplication by SCC here.
    _HM3_PER_CUMEC_HOUR = 277.78
    n_wv = n_efc = 0
    for r in sys_.get("reservoir_array", []) or []:
        wev = r.get("water_emission_value")
        if not isinstance(wev, (int, float)) or wev <= 0:
            continue
        # Replace dollar-denominated water_value / efin_cost with the
        # emission-equivalent per-hm³ terminal value.
        new_terminal = float(wev) * _HM3_PER_CUMEC_HOUR
        if "water_value" in r and isinstance(r["water_value"], (int, float)):
            r["water_value"] = new_terminal
            n_wv += 1
        if "efin_cost" in r and isinstance(r["efin_cost"], (int, float)):
            r["efin_cost"] = new_terminal
            n_efc += 1
    if n_wv:
        applied["reservoir_array.water_value"] = (
            f"{n_wv} replaced with EPF·gas·loss×{_HM3_PER_CUMEC_HOUR} tCO2eq/hm³"
        )
    if n_efc:
        applied["reservoir_array.efin_cost"] = (
            f"{n_efc} replaced with EPF·gas·loss×{_HM3_PER_CUMEC_HOUR} tCO2eq/hm³"
        )

    # 13. Generator.fuel_price_override (#521).
    #
    # 118-bus / GenX-style schemas ship a per-Generator scalar
    # ``fuel_price_override`` ($/MWh-equivalent of fuel cost) that
    # plexos2gtopt passes through to ``gcost``.  In emissions mode
    # the LP's GeneratorLP zeros the gcost slope (commit d230082ba),
    # but the override field is still present in the JSON and could
    # leak into reporting tools.  Zero it explicitly to match the
    # LP-side zeroing — keeps the JSON self-consistent.
    n_fpo = 0
    for g in sys_.get("generator_array", []) or []:
        v = g.get("fuel_price_override")
        if isinstance(v, (int, float)) and v > 0:
            g["fuel_price_override"] = 0.0
            n_fpo += 1
    if n_fpo:
        applied["generator_array.fuel_price_override"] = f"{n_fpo} zeroed"

    return applied


__all__ = [
    "CostDefault",
    "COST_DEFAULTS",
    "DEFAULT_SCC",
    "get_default",
    "apply_emission_overrides",
]
