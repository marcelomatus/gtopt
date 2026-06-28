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
from dataclasses import dataclass, field
from pathlib import Path

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
    parse_inflows,
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
    return (case_dir / "sddp.dat").is_file()


def _demand_file(case_dir: Path) -> Path | None:
    """Pick the hourly demand file, preferring 60-min over 15-min."""
    # ``cpde15*`` is the 15-min file; prefer the plain hourly ``cpde*GU``.
    for p in sorted(case_dir.glob("cpde*.dat")):
        if "15" not in p.name:
            return p
    return None


def load_dat_case(
    case_dir: str | Path, *, import_limit: float | None = None
) -> DatCase:
    """Parse a PSR ``.dat`` case directory into a :class:`DatCase`.

    Args:
        case_dir: PSR ``.dat`` case directory.
        import_limit: Optional cap [MW] on the aggregate interconnection
            import (Mexico/import-fuel generators) — the physical tie
            limit (GUA↔MEX ≈ 200 MW), used to stop the $0 import fuels
            flooding the dispatch.

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
        for t in thermals:
            t.is_import = bool(t.fuel_refs) and t.fuel_refs[0] in import_fuels

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
    # (import_limit is threaded explicitly below via the function argument)

    hydros: list[HydroSpec] = []
    hydro_path = find_dat(case_dir, *_HYDRO)
    if hydro_path:
        hydros = HydroParser(hydro_path).parse()

    # ── hydro water values (watervcp.csv) → per-plant gcost [$/MWh] ──────
    # PSR water value is k$/hm³; convert to $/MWh via the production factor
    # (FPMed): $/MWh = WV·1000 / (FPMed·1e6/3600) = WV·3.6/FPMed.  Capped at
    # the deficit cost (no resource should price above unserved energy).
    wv_path = case_dir / "watervcp.csv"
    if wv_path.is_file():
        wv = parse_water_values(wv_path)
        n_priced = 0
        for h in hydros:
            value = wv.get(h.name)
            if value and value > 0 and h.fp_med > 0:
                h.gcost = min(value * 3.6 / h.fp_med, study.deficit_cost)
                n_priced += 1
        logger.info("applied water values to %d hydro plant(s)", n_priced)

    # ── inflow limit on run-of-river hydro (inflow.csv) ─────────────────
    # Free run-of-river hydro (water value ≈ 0) would otherwise dispatch at
    # full installed `Pot`, flooding the market.  Cap each such plant at the
    # available water: pmax = min(Pot, inflow[m³/s] · FPMed[MW/(m³/s)]).
    # Storage plants (priced > 0) keep `Pot`; their water value already
    # governs dispatch.
    inflow_path = case_dir / "inflow.csv"
    if inflow_path.is_file():
        inflow = parse_inflows(inflow_path)
        n_capped = 0
        for h in hydros:
            q = inflow.get(h.name)
            if h.gcost > 0.0 or q is None or h.fp_med <= 0.0:
                continue  # priced storage hydro, or no inflow/FPMed data
            avail = q * h.fp_med
            if avail < h.p_inst:
                h.p_inst = avail
                n_capped += 1
        logger.info("inflow-capped %d run-of-river hydro plant(s)", n_capped)

    # ── network (multi-bus) — dbus.dat + dcirc.dat ───────────────────────
    buses: list[BusSpec] = []
    circuits: list[CircuitSpec] = []
    gen2bus: dict[str, int] = {}
    bus_path = case_dir / "dbus.dat"
    circ_path = case_dir / "dcirc.dat"
    if bus_path.is_file() and circ_path.is_file():
        bp = BusParser(bus_path)
        buses = bp.parse()
        gen2bus = bp.gen2bus
        raw_circuits = CircuitParser(circ_path).parse()

        # Bus base voltage: Volt.dat (authoritative) → name suffix →
        # neighbour inference (generator terminal buses absent from
        # Volt.dat inherit the highest-voltage neighbour).
        kv_by_bus = {b.number: b.base_kv for b in buses}
        volt_path = case_dir / "Volt.dat"
        if volt_path.is_file():
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
        # Route generators to their bus by name.
        for t in thermals:
            t.bus_number = gen2bus.get(t.name)
        for h in hydros:
            h.bus_number = gen2bus.get(h.name)

    multi_bus = bool(buses and circuits)

    # ── demand ───────────────────────────────────────────────────────────
    demands: list[DemandSpec] = []
    if multi_bus and (case_dir / "cpdexbus.dat").is_file():
        series = BusDemandParser(case_dir / "cpdexbus.dat").parse()
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
            study.num_stages = 1
            study.num_blocks = n_blocks
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
    )
