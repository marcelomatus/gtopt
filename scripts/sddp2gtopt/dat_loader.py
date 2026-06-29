"""Orchestrate the PSR SDDP / NCP ``.dat`` parsers into the entity IR.

This is the ``.dat`` counterpart of :mod:`sddp2gtopt.psrclasses_loader` +
:mod:`sddp2gtopt.parsers`: it detects a raw ``.dat`` case and turns the
flat file collection into the same ``(study, systems, thermals, hydros,
demands)`` tuple the JSON front-end produces, so
:func:`sddp2gtopt.gtopt_writer.build_planning` consumes either source
unchanged.

Time model (v0): the NCP demand file is hourly over the dispatch horizon,
so the converted planning is **single-stage** with one block per demand
hour (block duration = stage hours / N).  Multi-stage SDDP horizons,
multi-bus network (``dbus``/``dcirc``) and reservoir hydro
(``htopol``/``hinflw``) are the documented follow-ons.
"""

from __future__ import annotations

import logging
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from gtopt_shared.compressed_open import find_compressed_path

from .dat_parsers import (
    BusDemandParser,
    BusParser,
    CircuitParser,
    ControlParser,
    DemandParser,
    FuelParser,
    HydroParser,
    SystemParser,
    ThermalParser,
    VoltParser,
    find_dat,
    parse_commitment,
    parse_final_volumes,
    parse_gen_constraints,
    parse_inflows,
    parse_max_generation,
    parse_renewable_profiles,
    parse_renewables,
    parse_unit_prices,
    parse_water_values,
)
from .entities import (
    BusSpec,
    CircuitSpec,
    DemandSpec,
    HydroSpec,
    StudySpec,
    SystemSpec,
    ThermalSpec,
)


logger = logging.getLogger(__name__)


# Glob patterns for each PSR ``.dat`` (study-specific suffixes like the
# ``GU`` country tag vary, so we match by prefix).
_CONTROL = ("sddp.dat",)
_SYSTEM = ("sistem.dat",)
_FUEL = ("ccombu*.dat", "ccomb*.dat")
_THERMAL = ("ctermi*.dat",)
_HYDRO = ("chidro*.dat",)


@dataclass
class DatCase:
    """The entity lists parsed from a PSR ``.dat`` case directory."""

    study: StudySpec
    systems: list[SystemSpec] = field(default_factory=list)
    thermals: list[ThermalSpec] = field(default_factory=list)
    hydros: list[HydroSpec] = field(default_factory=list)
    demands: list[DemandSpec] = field(default_factory=list)
    buses: list[BusSpec] = field(default_factory=list)
    circuits: list[CircuitSpec] = field(default_factory=list)
    multi_bus: bool = False
    hydro_topology: bool = False


# Per-unit reactance floor/cap (on a 100 MVA base).  Zero-impedance ties
# are floored so the DC flow equation stays finite; tiny distribution
# transformers (very high p.u. X on a 100 MVA base) are capped so the LP
# coefficient spread stays well-conditioned — both are near-radial links
# whose flow is set by the load, not the reactance, so the clamp is
# physically harmless.
_MIN_PU = 1.0e-4
_MAX_PU = 20.0


def _ohm_to_pu(x_ohm: float, kv: float) -> float:
    """Convert an ohm reactance to per-unit on a 100 MVA base.

    ``X_pu = X_ohm · S_base / V²`` with ``S_base = 100`` MVA.  For a
    transformer (different end voltages) the HV side is used as the
    reference, the standard convention.  Falls back to the raw value when
    the bus base voltage is unknown.
    """
    return x_ohm * 100.0 / (kv * kv) if kv > 0 else x_ohm


def _clamp_reactance(x_pu: float) -> float:
    """Floor/cap a per-unit reactance (see ``_MIN_PU`` / ``_MAX_PU``)."""
    sign = -1.0 if x_pu < 0 else 1.0
    mag = min(max(abs(x_pu), _MIN_PU), _MAX_PU)
    return sign * mag


def _infer_missing_kv(kv_by_bus: dict[int, float], circuits: list[CircuitSpec]) -> None:
    """Fill in ``kv == 0`` buses from their highest-voltage neighbour.

    Generator terminal buses are absent from ``Volt.dat`` and have no
    voltage in their name; they connect to the network through a single
    step-up transformer, so the highest-voltage neighbour is the right
    reference for the ohm→p.u. conversion.  Iterated a few times to
    propagate across short low-voltage chains.
    """
    adj: dict[int, list[int]] = {}
    for c in circuits:
        adj.setdefault(c.from_bus, []).append(c.to_bus)
        adj.setdefault(c.to_bus, []).append(c.from_bus)
    for _ in range(3):
        changed = False
        for bus, kv in list(kv_by_bus.items()):
            if kv > 0:
                continue
            neigh = [kv_by_bus.get(n, 0.0) for n in adj.get(bus, [])]
            best = max(neigh, default=0.0)
            if best > 0:
                kv_by_bus[bus] = best
                changed = True
        if not changed:
            break


def _infer_blocks_per_day(profiles: dict[str, list[float]]) -> int:
    """Infer the inflow's blocks-per-day from its run-length structure.

    PSR inflow is constant within a day, so each plant's series is a run of
    equal values per day; the GCD of all run lengths is the blocks-per-day
    (e.g. a 6-hour, 4-block/day forecast → 4).  Falls back to a ~24-day daily
    forecast when the GCD is degenerate.
    """
    from math import gcd  # pylint: disable=import-outside-toplevel

    g = 0
    n = 0
    for prof in profiles.values():
        n = max(n, len(prof))
        run = 0
        prev: float | None = None
        for v in prof:
            if prev is not None and abs(v - prev) > 1e-9:
                g = gcd(g, run)
                run = 0
            run += 1
            prev = v
        g = gcd(g, run)
    if g >= 2 and n % g == 0:
        return g
    return max(1, n // 24)


def _apply_max_gen(
    specs: list[Any],
    path: Path,
    installed: Callable[[Any], float],
) -> int:
    """Apply a PSR ``cprmx*`` max-generation cap to each spec's ``max_gen``.

    Resolves the raw profile against each plant's installed capacity:
    ``-1`` sentinels (no cap) and ``%G`` units become MW, and the cap is set
    only when it actually binds (below installed).  Returns the count capped.
    """
    profiles, is_percent = parse_max_generation(path)
    n_capped = 0
    for s in specs:
        prof = profiles.get(s.name)
        if not prof:
            continue
        inst = installed(s)
        if inst <= 0:
            continue
        cap = [
            (inst if v < 0 else (v * inst / 100.0 if is_percent else v)) for v in prof
        ]
        if cap and min(cap) < inst - 1e-6:
            s.max_gen = cap
            n_capped += 1
    return n_capped


def is_dat_case(case_dir: str | Path) -> bool:
    """True if ``case_dir`` looks like a raw PSR ``.dat`` case.

    Requires ``sddp.dat`` and the absence of a ``psrclasses.json`` (which
    the JSON front-end handles instead).
    """
    case_dir = Path(case_dir)
    if not case_dir.is_dir():
        return False
    if (case_dir / "psrclasses.json").is_file():
        return False
    return find_compressed_path(case_dir / "sddp.dat") is not None


def _demand_file(case_dir: Path) -> Path | None:
    """Pick the hourly demand file, preferring 60-min over 15-min.

    Matches plain and compressed names (``cpde*.dat`` / ``cpde*.dat.xz``).
    """
    # ``cpde15*`` is the 15-min file; prefer the plain hourly ``cpde*GU``.
    for p in sorted(case_dir.glob("cpde*.dat*")):
        if p.is_file() and "15" not in p.name and ".dat" in p.name:
            return p
    return None


def load_dat_case(
    case_dir: str | Path,
    *,
    import_limit: float | None = None,
    blocks_per_stage: int = 0,
) -> DatCase:
    """Parse a PSR ``.dat`` case directory into a :class:`DatCase`.

    Args:
        case_dir: PSR ``.dat`` case directory.
        import_limit: Optional cap [MW] on the aggregate interconnection
            import (Mexico/import-fuel generators) — the physical tie
            limit (GUA↔MEX ≈ 200 MW), used to stop the $0 import fuels
            flooding the dispatch.
        blocks_per_stage: Hourly blocks grouped into each gtopt stage on the
            multi-bus path.  Default ``0`` keeps a **single stage** — a
            one-week NCP dispatch is deterministic and solves monolithic (like
            the plexos2gtopt cases).  A positive value that divides the horizon
            (e.g. 24 = daily) splits it into ≥ 2 stages so the case runs under
            the SDDP / cascade methods, with reservoirs coupling storage across
            stages — intended for genuine multi-week horizons.

    Raises:
        FileNotFoundError: If the mandatory ``sddp.dat`` is missing.
    """
    case_dir = Path(case_dir)
    control_path = find_dat(case_dir, *_CONTROL)
    if control_path is None:
        raise FileNotFoundError(f"{case_dir}: no sddp.dat (not a PSR .dat case)")

    study = ControlParser(control_path).parse()

    systems: list[SystemSpec] = []
    sys_path = find_dat(case_dir, *_SYSTEM)
    if sys_path:
        systems = SystemParser(sys_path).parse()
    if not systems:
        systems = [SystemSpec(code=1, name="SYSTEM", reference_id=1)]

    fuel_cost: dict[int, float] = {}
    import_fuels: set[int] = set()
    fuel_path = find_dat(case_dir, *_FUEL)
    if fuel_path:
        fuels = FuelParser(fuel_path).parse()
        fuel_cost = {f.code: f.cost for f in fuels}
        # Interconnection import pseudo-fuels (Mexico / generic import).
        import_fuels = {
            f.code for f in fuels if "MEX" in f.name.upper() or "IMP" in f.name.upper()
        }

    thermals: list[ThermalSpec] = []
    therm_path = find_dat(case_dir, *_THERMAL)
    if therm_path:
        thermals = ThermalParser(therm_path).parse(fuels=fuel_cost)
        # Interconnection imports: a MEX/IMP-priced fuel, OR a unit named for a
        # tie (MEX/ESA/HON/IMP/EDC).  The fuel test alone misses ESA-IMP /
        # HON-IMP (their $0 fuels aren't MEX/IMP-named), so they'd flood uncapped.
        for t in thermals:
            t.is_import = (
                bool(t.fuel_refs) and t.fuel_refs[0] in import_fuels
            ) or t.name.upper().startswith(("MEX", "ESA", "HON", "IMP", "EDC"))

    # ── interconnection import cap ───────────────────────────────────────
    # The PSR fuel table prices Mexico/import fuels at ~$0 with large plant
    # capacities (the external equivalent), which would flood the dispatch.
    # The physical tie limit is far smaller (GUA↔MEX ≈ 200 MW import); scale
    # every import generator's pmax so their aggregate matches the limit.
    if import_limit is not None and import_limit > 0:
        imports = [t for t in thermals if t.is_import and t.pmax > 0]
        total = sum(t.pmax for t in imports)
        if total > import_limit:
            scale = import_limit / total
            for t in imports:
                t.pmax *= scale
            logger.info(
                "import cap: scaled %d import generators %.0f → %.0f MW (×%.3f)",
                len(imports),
                total,
                import_limit,
                scale,
            )

    # ── non-dispatchable renewables (cgndgu.dat + cpgndgu.dat) ───────────
    # Wind (``-E``) / solar (``-F``) with a fixed hourly availability forecast.
    # Modelled as generators capped at the forecast (``max_gen``) carrying the
    # plant O&M cost, so PSR's must-take renewable energy (notably midday solar)
    # enters the dispatch instead of being silently displaced by free imports.
    gnd_path = find_compressed_path(case_dir / "cgndgu.dat")
    if gnd_path is not None:
        renewables = parse_renewables(gnd_path)
        prof_path = find_compressed_path(case_dir / "cpgndgu.dat")
        profiles = parse_renewable_profiles(prof_path) if prof_path else {}
        for r in renewables:
            prof = profiles.get(r.code)
            if prof:
                r.max_gen = prof
        thermals.extend(renewables)
        logger.info(
            "loaded %d renewables (%d with hourly forecast)",
            len(renewables),
            sum(1 for r in renewables if r.max_gen),
        )

    # ── interconnection import prices (PRECIOSMEX.csv) ───────────────────
    # The AMM's per-unit hourly bid is the authoritative import cost: only the
    # contracted MEX-I tie is free, the other ties bid 250 $/MWh on the billed
    # day (≈0 on look-ahead days).  The $0 fuel table makes them all free and
    # they flood the dispatch — so override the interconnection gcost with the
    # PRECIOSMEX series (ties not listed there inherit the regional INTER-SAL
    # profile).  Domestic units keep their fuel-based cost (PRECIOSMEX ≈ fuel).
    px_path = find_compressed_path(case_dir / "PRECIOSMEX.csv")
    if px_path is not None:
        prices = parse_unit_prices(px_path)
        regional = next(
            (v for k, v in prices.items() if k.upper() == "INTER-SAL"), None
        )
        n_px = 0
        for t in thermals:
            if not t.is_import:
                continue
            prof = prices.get(t.name) or prices.get(t.name.upper())
            if prof is None and regional is not None:
                prof = regional
            if prof:
                t.gcost_profile = prof
                n_px += 1
        logger.info("applied PRECIOSMEX import prices to %d interconnections", n_px)

    # ── AMM operational constraints (RESTMEX.csv) ────────────────────────
    # The market operator's per-unit generation limits (< pmax / > pmin / = fix,
    # hourly).  Most are small self-supply units absent from this case, but the
    # in-model ones (ORT-G, ZUN-G, TER-B, …) are bound faithfully.
    rm_path = find_compressed_path(case_dir / "RESTMEX.csv")
    if rm_path is not None:
        cons = parse_gen_constraints(rm_path)
        by_name = {spec.name.upper(): spec for spec in thermals}
        n_rm = 0
        for unit, (tp, rhs) in cons.items():
            spec = by_name.get(unit.upper())
            if spec is not None:
                spec.amm_tipo = tp
                spec.amm_profile = rhs
                n_rm += 1
        logger.info("applied %d/%d AMM constraints to in-model units", n_rm, len(cons))

    hydros: list[HydroSpec] = []
    hydro_path = find_dat(case_dir, *_HYDRO)
    if hydro_path:
        hydros = HydroParser(hydro_path).parse()

    # Full water-network mode kicks in when the ``chidro`` record carries real
    # storage (``VMax`` > 0): reservoirs + turbines + junctions + inflow are
    # emitted and the per-plant water value rides a single boundary cut on the
    # reservoir end-volumes.  Otherwise hydros stay flattened to generators and
    # the legacy gcost / inflow-cap stand-ins apply.
    hydro_topology = any(h.vmax > 0.0 for h in hydros)

    # ── hydro water values (watervcp.csv) ───────────────────────────────
    # PSR water value is k$/hm³.  Two consumers:
    #   * full topology → ``water_value`` [$/hm³] = WV·1000 on the boundary cut;
    #   * legacy generators → ``gcost`` [$/MWh] = WV·3.6/FPMed (capped at the
    #     deficit cost — no resource prices above unserved energy).
    wv_path = find_compressed_path(case_dir / "watervcp.csv")
    if wv_path is not None:
        wv = parse_water_values(wv_path)
        n_priced = 0
        for h in hydros:
            value = wv.get(h.name)
            if value and value > 0:
                h.water_value = value * 1000.0
                if h.fp_med > 0:
                    h.gcost = min(value * 3.6 / h.fp_med, study.deficit_cost)
                n_priced += 1
        logger.info("applied water values to %d hydro plant(s)", n_priced)

    # ── natural inflow (inflow.csv) ─────────────────────────────────────
    # The forecast is a time-varying daily series; keep it as a per-day profile
    # (NOT the horizon mean, which over-states dispatch-period water and floods
    # run-of-river hydro).  The writer maps the profile onto the model's
    # day/block grid; ``h.inflow`` keeps the representative mean for the legacy
    # generator path and the topology emit-guard.
    inflow_path = find_compressed_path(case_dir / "inflow.csv")
    if inflow_path is not None:
        inflow = parse_inflows(inflow_path)
        bpd = _infer_blocks_per_day(inflow)
        for h in hydros:
            prof = inflow.get(h.name)
            if prof:
                daily = [
                    sum(prof[i : i + bpd]) / len(prof[i : i + bpd])
                    for i in range(0, len(prof), bpd)
                ]
                h.inflow_profile = daily
                h.inflow = sum(daily) / len(daily)

    # ── expected end-of-horizon volume (volfincp.csv) → efin ─────────────
    # PSR's solved end storage is the boundary-cut linearisation point.  SDDP
    # does NOT assume vfin = vini: when no end-volume is shipped the final volume
    # is left free and valued only by the future cost (the cut).  So efin is set
    # ONLY from volfincp; reservoirs without it keep efin = 0 (no soft target)
    # and are still priced via their water-value coefficient in the cut.
    vfin_path = find_compressed_path(case_dir / "volfincp.csv")
    if vfin_path is not None:
        vfin = parse_final_volumes(vfin_path)
        n_efin = 0
        for h in hydros:
            v = vfin.get(h.name)
            if v is not None:
                h.efin = v
                n_efin += 1
        logger.info("applied PSR end-volumes (efin) to %d hydro plant(s)", n_efin)

    # ── max generation caps (cprmxhgu / cprmxtgu) → generator pmax ───────
    # PSR's per-plant, per-hour maximum-generation limit (operational
    # derating / availability) holds plants below installed capacity — the
    # reason PSR's hydro dispatch (~498 MW) is far under installed.  Applied
    # to ``pmax`` (NOT ``capacity`` — a distinct expansion concept).
    mxh_path = find_compressed_path(case_dir / "cprmxhgu_u.dat")
    if mxh_path is not None:
        n = _apply_max_gen(hydros, mxh_path, lambda h: h.p_inst)
        logger.info("max-gen cap (cprmxhgu): capped %d hydro plant(s)", n)
    mxt_path = find_compressed_path(case_dir / "cprmxtgu_u.dat")
    if mxt_path is not None:
        n = _apply_max_gen(thermals, mxt_path, lambda t: t.pmax)
        logger.info("max-gen cap (cprmxtgu): capped %d thermal plant(s)", n)

    # ── hydro commitment (commith.dat) ──────────────────────────────────
    # Units committed off this dispatch must not generate; gtopt pins their
    # generator pmax to 0 and lets water bypass their turbine (drain) so the
    # cascade still feeds downstream plants.
    commit_path = find_compressed_path(case_dir / "commith.dat")
    if commit_path is not None:
        commit = parse_commitment(commit_path)
        n_off = 0
        for h in hydros:
            if commit.get(h.name) is False:
                h.committed = False
                n_off += 1
        logger.info("commitment (commith): %d hydro plant(s) committed OFF", n_off)

    if hydro_topology:
        # The water network governs hydro dispatch physically (turbine flow ≤
        # inflow + storage release, valued by the boundary cut), so the
        # generators carry no fuel cost and keep their installed capacity.
        for h in hydros:
            h.gcost = 0.0
    else:
        # Legacy generator model: cap free run-of-river hydro (water value ≈ 0)
        # at the available water so it can't dispatch its full installed `Pot`
        # and flood the market — pmax = min(Pot, inflow · FPMed).
        n_capped = 0
        for h in hydros:
            if h.gcost > 0.0 or h.inflow <= 0.0 or h.fp_med <= 0.0:
                continue
            avail = h.inflow * h.fp_med
            if avail < h.p_inst:
                h.p_inst = avail
                n_capped += 1
        logger.info("inflow-capped %d run-of-river hydro plant(s)", n_capped)

    # ── network (multi-bus) — dbus.dat + dcirc.dat ───────────────────────
    buses: list[BusSpec] = []
    circuits: list[CircuitSpec] = []
    gen2bus: dict[str, int] = {}
    bus_path = find_compressed_path(case_dir / "dbus.dat")
    circ_path = find_compressed_path(case_dir / "dcirc.dat")
    if bus_path is not None and circ_path is not None:
        bp = BusParser(bus_path)
        buses = bp.parse()
        gen2bus = bp.gen2bus
        raw_circuits = CircuitParser(circ_path).parse()

        # Bus base voltage: Volt.dat (authoritative) → name suffix →
        # neighbour inference (generator terminal buses absent from
        # Volt.dat inherit the highest-voltage neighbour).
        kv_by_bus = {b.number: b.base_kv for b in buses}
        volt_path = find_compressed_path(case_dir / "Volt.dat")
        if volt_path is not None:
            for bus, kv in VoltParser(volt_path).parse().items():
                kv_by_bus[bus] = kv
        for b in buses:
            b.base_kv = kv_by_bus.get(b.number, b.base_kv)
        _infer_missing_kv(kv_by_bus, raw_circuits)
        for b in buses:
            b.base_kv = kv_by_bus.get(b.number, b.base_kv)

        for c in raw_circuits:
            kv = max(kv_by_bus.get(c.from_bus, 0.0), kv_by_bus.get(c.to_bus, 0.0))
            c.reactance_pu = _clamp_reactance(_ohm_to_pu(c.reactance_pu, kv))
            circuits.append(c)
        # Route generators to their bus by name (keeping a spec's own bus —
        # e.g. a renewable's cgndgu bus — when dbus has no name mapping).
        for t in thermals:
            t.bus_number = gen2bus.get(t.name, t.bus_number)
        for h in hydros:
            h.bus_number = gen2bus.get(h.name, h.bus_number)

    multi_bus = bool(buses and circuits)

    # ── demand ───────────────────────────────────────────────────────────
    demands: list[DemandSpec] = []
    cpx_path = find_compressed_path(case_dir / "cpdexbus.dat")
    if multi_bus and cpx_path is not None:
        series = BusDemandParser(cpx_path).parse()
        live = {b.number for b in buses}
        uid = 1
        n_blocks = 0
        for bus, vals in series.items():
            if bus not in live or sum(vals) <= 0.0:
                continue
            demands.append(
                DemandSpec(
                    code=uid,
                    name=f"dem_{bus}",
                    reference_id=uid,
                    bus_number=bus,
                    block_values=vals,
                )
            )
            n_blocks = max(n_blocks, len(vals))
            uid += 1
        if n_blocks:
            # The NCP demand is a flat hourly horizon (1 h per block).  Group it
            # into daily stages so SDDP / cascade have ≥ 2 phases to decompose;
            # only when the horizon divides evenly (else keep a single stage).
            study.block_hours = 1.0
            if (
                blocks_per_stage
                and 0 < blocks_per_stage < n_blocks
                and (n_blocks % blocks_per_stage == 0)
            ):
                study.num_blocks = blocks_per_stage
                study.num_stages = n_blocks // blocks_per_stage
            else:
                study.num_stages = 1
                study.num_blocks = n_blocks
            logger.info(
                "time model: %d hourly blocks → %d stage(s) × %d block(s)",
                n_blocks,
                study.num_stages,
                study.num_blocks,
            )
    else:
        dem_path = _demand_file(case_dir)
        if dem_path:
            blocks = DemandParser(dem_path).parse()
            if blocks:
                demands = [
                    DemandSpec(
                        code=1,
                        name="demand",
                        reference_id=1,
                        system_ref=systems[0].reference_id,
                        block_values=blocks,
                    )
                ]
                study.num_stages = 1
                study.num_blocks = len(blocks)

    logger.info(
        "loaded PSR .dat case %s: %d systems, %d buses, %d circuits, "
        "%d thermals, %d hydros, %d demands (multi_bus=%s, %d blocks)",
        case_dir.name,
        len(systems),
        len(buses),
        len(circuits),
        len(thermals),
        len(hydros),
        len(demands),
        multi_bus,
        study.num_blocks,
    )
    return DatCase(
        study=study,
        systems=systems,
        thermals=thermals,
        hydros=hydros,
        demands=demands,
        buses=buses,
        circuits=circuits,
        multi_bus=multi_bus,
        hydro_topology=hydro_topology,
    )
