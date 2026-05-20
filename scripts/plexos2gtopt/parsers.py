"""Per-class extractors — turn a :class:`PlexosBundle` into entities.

Each ``extract_*`` function is a pure transformer from the PLEXOS DB
(XML topology + CSV schedules) to the typed dataclasses declared in
:mod:`plexos2gtopt.entities`. Wiring the writer to these specs keeps
unit conversion / dialect lookups in one place — the
:mod:`plexos2gtopt.gtopt_writer` module.

Each extractor accepts a per-class CSV (long or wide format), falls
back to ``t_data`` static properties on the matching ``System →
<Class>`` collection when the CSV is absent, and emits a frozen
dataclass keyed by the PLEXOS object name. :func:`extract_case`
orchestrates the full set and returns one :class:`PlexosCase`.
"""

from __future__ import annotations

import dataclasses
import logging
import math
import re
from pathlib import Path

from .entities import (
    BatterySpec,
    BundleSpec,
    CommitmentSpec,
    DecisionVariableSpec,
    DemandSpec,
    FlowRightSpec,
    FlowSpec,
    FuelSpec,
    GeneratorSpec,
    JunctionSpec,
    LineSpec,
    NodeSpec,
    PlexosCase,
    ReservoirSpec,
    ReserveProvisionSpec,
    ReserveSpec,
    TurbineSpec,
    UserConstraintSpec,
    WaterwaySpec,
)
from .plexos_csv import DEFAULT_PERIODS, read_long, read_wide
from .plexos_loader import PlexosBundle
from .plexos_xml import PlexosDb, PlexosObject, load_xml


logger = logging.getLogger(__name__)


# PLEXOS Sense → gtopt operator. CEN PCP uses {-1: LE, 1: GE, 0: EQ}.
_SENSE_OP = {-1.0: "<=", 1.0: ">=", 0.0: "="}


# Coefficient kinds that map 1:1 to a single gtopt LP variable.
# Each entry is (parent_class, coll_name, property, gtopt_class, accessor,
# name_template) — ``name_template`` builds the referenced element name
# from the PLEXOS parent name (``{name}``), letting us route generator
# reserve coefficients to ``reserve_provision("provision_<gen>")`` rather
# than directly to the parent name.
#
# Accessors must match the variable names registered via
# ``add_ampl_variable`` in the respective ``*_lp.cpp`` files:
#   - Generator.generation       (source/generator_lp.cpp:329)
#   - Line.flow (= flowp − flown) (source/line_lp.cpp + system_lp.cpp compound)
#   - Battery.charge / .discharge (source/battery_lp.cpp:150,152)
#   - ReserveProvision.up / .dn  (source/reserve_provision_lp.cpp:357,362)
#
# Commitment binaries (``Units Generating`` → ``commitment("uc_X").status``,
# ``Units Started`` → ``.startup``, ``Units Shutdown`` → ``.shutdown``)
# would naturally belong here but ``commitment_lp.cpp`` doesn't call
# ``add_ampl_variable`` for those columns today — the parser whitelist
# alone isn't enough to resolve them. Wire those once gtopt registers
# the commitment columns with the AMPL resolver.
_DIRECT_COEFFS: tuple[tuple[str, str, str, str, str, str], ...] = (
    (
        "Generator",
        "Constraints",
        "Generation Coefficient",
        "generator",
        "generation",
        "{name}",
    ),
    ("Line", "Constraints", "Flow Coefficient", "line", "flow", "{name}"),
    (
        "Battery",
        "Constraints",
        "Generation Coefficient",
        "battery",
        "discharge",
        "{name}",
    ),
    ("Battery", "Constraints", "Load Coefficient", "battery", "charge", "{name}"),
    # Reserve provision (per-generator / per-battery up/down provision MW).
    # PLEXOS auto-creates the provision variable for every (Reserve,
    # Generator) eligibility membership regardless of Risk/Requirement,
    # so Constraints can *force* reserve dispatch. gtopt registers
    # ``.up`` / ``.dn`` via ``add_ampl_variable`` only when the
    # zone has a positive urreq/drreq — :func:`build_reserve_zone_array`
    # emits a zero-vector profile when the source is empty so the
    # columns always exist.
    (
        "Generator",
        "Constraints",
        "Regulation Raise Reserve Provision Coefficient",
        "reserve_provision",
        "up",
        "provision_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Regulation Lower Reserve Provision Coefficient",
        "reserve_provision",
        "dn",
        "provision_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Replacement Reserve Provision Coefficient",
        "reserve_provision",
        "up",
        "provision_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Raise Reserve Provision Coefficient",
        "reserve_provision",
        "up",
        "provision_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Lower Reserve Provision Coefficient",
        "reserve_provision",
        "dn",
        "provision_{name}",
    ),
    (
        "Battery",
        "Constraints",
        "Reserve Provision Coefficient",
        "reserve_provision",
        "up",
        "provision_{name}",
    ),
    (
        "Battery",
        "Constraints",
        "Regulation Raise Reserve Provision Coefficient",
        "reserve_provision",
        "up",
        "provision_{name}",
    ),
    (
        "Battery",
        "Constraints",
        "Regulation Lower Reserve Provision Coefficient",
        "reserve_provision",
        "dn",
        "provision_{name}",
    ),
    # Storage state: battery("X").energy / reservoir("X").efin —
    # registered by StorageLP::add_to_lp.  PLEXOS keeps these on the
    # Battery / Storage → Constraint membership row.
    (
        "Battery",
        "Constraints",
        "Energy Coefficient",
        "battery",
        "energy",
        "{name}",
    ),
    (
        "Waterway",
        "Constraints",
        "Flow Coefficient",
        "waterway",
        "flow",
        "{name}",
    ),
    (
        "Storage",
        "Constraints",
        "End Volume Coefficient",
        "reservoir",
        "efin",
        "{name}",
    ),
    # Decision Variables: PLEXOS ``Value Coefficient`` on the
    # ``Decision Variable → Constraint`` collection. gtopt's
    # ``DecisionVariableLP`` registers the per-block column as
    # ``decision_variable("X").value``.
    (
        "Decision Variable",
        "Constraints",
        "Value Coefficient",
        "decision_variable",
        "value",
        "{name}",
    ),
    # Commitment binaries: gtopt registers status/startup/shutdown LP
    # columns via ``add_ampl_variable`` (commitment_lp.cpp end-of-add_to_lp),
    # so user constraints can reference them by name. Only emitted when
    # the generator has a matching Commitment row (``uc_<gen>``).
    (
        "Generator",
        "Constraints",
        "Units Generating Coefficient",
        "commitment",
        "status",
        "uc_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Units Started Coefficient",
        "commitment",
        "startup",
        "uc_{name}",
    ),
    (
        "Generator",
        "Constraints",
        "Units Shutdown Coefficient",
        "commitment",
        "shutdown",
        "uc_{name}",
    ),
    # PLEXOS ``Reserve Units Coefficient`` counts the integer number of
    # units providing spinning reserve. gtopt's single-unit-per-generator
    # commitment model has no separate ``reserve_units`` column, so we
    # alias to ``commitment.status`` — exact for 1-unit generators (the
    # CEN PCP norm) and a tight upper bound for multi-unit gens.
    (
        "Generator",
        "Constraints",
        "Reserve Units Coefficient",
        "commitment",
        "status",
        "uc_{name}",
    ),
)


# Fuel offtake is expanded to a sum of ``heat_rate(g) × generator(g).generation``
# over every Generator with that Fuel-membership. Defined as a separate
# kind because the LHS expansion depends on the Generator catalogue,
# not just the Constraint membership row.
_FUEL_OFFTAKE = (
    "Fuel",
    "Constraints",
    "Offtake Coefficient",
)


# Reserve.Provision Coefficient on a Reserve→Constraint membership
# expands to ``α × Σ_g reserve_provision("provision_<g>").<dir>`` over
# every generator g eligible for that Reserve. ``<dir>`` is ``up`` for
# reserve zones with non-empty urreq (or ``_RS`` suffix), ``dn`` for
# zones with non-empty drreq (or ``_LW`` suffix).  Same pattern as the
# Fuel.Offtake expansion — the LHS depends on the per-Reserve
# eligibility catalogue, not just the Reserve membership row.
_RESERVE_PROVISION_EXPANSION = (
    "Reserve",
    "Constraints",
    "Provision Coefficient",
)


# Coefficient kinds that are *derived expressions* over the generator's
# generation variable rather than independent LP columns. PLEXOS rewrites
# them at constraint-compile time; we mirror the rewrite in the
# converter rather than asking gtopt to add new LP variables.
#
# Each entry is (parent_class, coll_name, property_name, mode) where
# ``mode`` is one of:
#
#   - ``"sent_out"``: PLEXOS ``Generation Sent Out`` = ``generation -
#     auxiliary_use``. v0 approximates aux_use ≈ 0 (CEN PCP keeps aux
#     in a tiny CSV that we don't yet wire), so this maps 1:1 to
#     ``α × generator("X").generation``.
#   - ``"per_capacity"``: PLEXOS ``Capacity Factor`` = ``generation /
#     (Capacity × duration_h)``. The LP-equivalent coefficient is
#     ``α / (pmax)`` on the generation column.  ``duration_h`` is
#     absorbed when the user-constraint is per-block (the duration is
#     constant within the block).
#   - ``"unsupported_rhs_shift"``: PLEXOS ``Generation Curtailed`` =
#     ``(Capacity × cf) − generation`` and ``Available Capacity`` =
#     ``Capacity × cf`` rewrite both LHS and RHS. UserConstraint's
#     grammar accepts only scalar RHS, so we cannot honour the shift
#     without per-block RHS support (a small gtopt-side feature gap).
#     Coefficients of these kinds are logged at WARNING and dropped
#     from the LHS; the constraint may still emit if it carries any
#     directly-supported coefficient.
_DERIVED_COEFFS: tuple[tuple[str, str, str, str], ...] = (
    ("Generator", "Constraints", "Generation Sent Out Coefficient", "sent_out"),
    ("Generator", "Constraints", "Capacity Factor Coefficient", "per_capacity"),
    (
        "Generator",
        "Constraints",
        "Generation Curtailed Coefficient",
        "unsupported_rhs_shift",
    ),
    (
        "Generator",
        "Constraints",
        "Available Capacity Coefficient",
        "unsupported_rhs_shift",
    ),
    # Ramp Up/Down Coefficient references the inter-block ramp delta
    # (gen(t) - gen(t-1)). gtopt models ramps via Commitment constraints
    # but does not expose ``ramp_up``/``ramp_down`` as an AMPL accessor.
    # Volume is tiny in CEN PCP (~9 rows total), so the v0 policy is the
    # same as the RHS-shift kinds: log a WARNING once per
    # (constraint, kind) and drop the term.
    (
        "Generator",
        "Constraints",
        "Ramp Up Coefficient",
        "unsupported_rhs_shift",
    ),
    (
        "Generator",
        "Constraints",
        "Ramp Down Coefficient",
        "unsupported_rhs_shift",
    ),
    # Battery Reserve Units Coefficient has no LP counterpart at all in
    # gtopt (no commitment binary on Battery) — drop with a WARNING.
    (
        "Battery",
        "Constraints",
        "Reserve Units Coefficient",
        "unsupported_rhs_shift",
    ),
)


# ---------------------------------------------------------------------------
# Bundle / horizon
# ---------------------------------------------------------------------------


def extract_bundle_spec(bundle: PlexosBundle) -> BundleSpec:
    """Read ``PLEXOS_Param.xml`` for the run horizon parameters.

    For v0 we honour ``Step Type = Hour`` / ``Step Count = 24`` only.
    Anything else rejects loudly so the caller can decide whether to
    extend the time-model writer (see DESIGN.md §5).
    """
    # PLEXOS_Param.xml is a thin wrapper; we only inspect what we need.
    # The PCP bundle keeps the schema invariant, so missing fields are
    # safe defaults rather than a hard error in v0.
    name = bundle.source.name
    bundle_date = ""
    step_count = DEFAULT_PERIODS
    step_type = "Hour"
    day_beginning = 0
    if bundle.param_xml_path.is_file():
        try:
            import xml.etree.ElementTree as ET  # pylint: disable=import-outside-toplevel

            tree = ET.parse(bundle.param_xml_path)
            root = tree.getroot()
            for elem in root.iter():
                tag = elem.tag.rsplit("}", 1)[-1]
                text = (elem.text or "").strip()
                if not text:
                    continue
                if tag in ("Step_Count", "StepCount"):
                    try:
                        step_count = int(float(text))
                    except ValueError:
                        pass
                elif tag in ("Step_Type", "StepType"):
                    step_type = text
                elif tag in ("Day_Beginning", "DayBeginning"):
                    try:
                        day_beginning = int(float(text))
                    except ValueError:
                        pass
                elif tag in ("Date_From", "DateFrom"):
                    bundle_date = text[:10]
        except (ET.ParseError, OSError) as exc:  # noqa: F821
            logger.warning("PLEXOS_Param.xml unreadable, using defaults: %s", exc)
    return BundleSpec(
        bundle_date=bundle_date,
        step_count=step_count,
        step_type=step_type,
        day_beginning=day_beginning,
        bundle_name=name,
    )


# ---------------------------------------------------------------------------
# Topology helpers (used by every extractor)
# ---------------------------------------------------------------------------


def _gen_bus_map(db: PlexosDb) -> dict[int, str]:
    """``{generator_object_id -> bus_name}`` via the ``Nodes`` collection."""
    coll = db.collection_for_named("Generator", "Node", "Nodes")
    if coll is None:
        return {}
    objs = db.object_by_id()
    p2c = db.parent_to_children(coll.collection_id)
    out: dict[int, str] = {}
    for parent, children in p2c.items():
        if not children:
            continue
        # PLEXOS allows multi-bus generators (split between nodes); v0
        # picks the first attachment and logs a warning for the rest.
        if len(children) > 1:
            extra = ", ".join(objs[c].name for c in children[1:] if c in objs)
            logger.debug(
                "generator %s split across multiple nodes (extras: %s); "
                "v0 uses the first attachment",
                objs.get(parent, "?"),
                extra,
            )
        child = objs.get(children[0])
        if child is not None:
            out[parent] = child.name
    return out


def _gen_fuel_map(db: PlexosDb) -> dict[int, tuple[str, ...]]:
    """``{generator_object_id -> (fuel_name, …)}`` via the ``Fuels`` collection."""
    coll = db.collection_for_named("Generator", "Fuel", "Fuels")
    if coll is None:
        return {}
    objs = db.object_by_id()
    p2c = db.parent_to_children(coll.collection_id)
    out: dict[int, tuple[str, ...]] = {}
    for parent, children in p2c.items():
        names = tuple(objs[c].name for c in children if c in objs)
        if names:
            out[parent] = names
    return out


def _line_bus_endpoints(db: PlexosDb) -> dict[int, tuple[str | None, str | None]]:
    """``{line_object_id -> (from_bus_name, to_bus_name)}``."""
    from_coll = db.collection_for_named("Line", "Node", "Node From")
    to_coll = db.collection_for_named("Line", "Node", "Node To")
    objs = db.object_by_id()
    p2c_from = db.parent_to_children(from_coll.collection_id) if from_coll else {}
    p2c_to = db.parent_to_children(to_coll.collection_id) if to_coll else {}
    out: dict[int, tuple[str | None, str | None]] = {}
    for line in db.objects_of_class("Line"):
        fs = p2c_from.get(line.object_id, [])
        ts = p2c_to.get(line.object_id, [])
        f_name = objs[fs[0]].name if fs and fs[0] in objs else None
        t_name = objs[ts[0]].name if ts and ts[0] in objs else None
        out[line.object_id] = (f_name, t_name)
    return out


def _battery_bus_map(db: PlexosDb) -> dict[int, str]:
    coll = db.collection_for_named("Battery", "Node", "Nodes")
    if coll is None:
        return {}
    objs = db.object_by_id()
    p2c = db.parent_to_children(coll.collection_id)
    out: dict[int, str] = {}
    for parent, children in p2c.items():
        if not children:
            continue
        child = objs.get(children[0])
        if child is not None:
            out[parent] = child.name
    return out


# ---------------------------------------------------------------------------
# Per-class extractors
# ---------------------------------------------------------------------------


def extract_nodes(db: PlexosDb) -> tuple[NodeSpec, ...]:
    """One :class:`NodeSpec` per ``t_object`` in class ``Node``.

    Region / Zone tags are pulled via the standard ``Region`` / ``Zone``
    child collections when present; missing memberships log at DEBUG
    and produce a NodeSpec with ``region=zone=None``.
    """
    objs = db.object_by_id()

    region_map: dict[int, str] = {}
    region_coll = db.collection_for_named("Node", "Region", "Region")
    if region_coll is not None:
        for parent, children in db.parent_to_children(
            region_coll.collection_id
        ).items():
            if children and children[0] in objs:
                region_map[parent] = objs[children[0]].name

    zone_map: dict[int, str] = {}
    zone_coll = db.collection_for_named("Node", "Zone", "Zone")
    if zone_coll is not None:
        for parent, children in db.parent_to_children(zone_coll.collection_id).items():
            if children and children[0] in objs:
                zone_map[parent] = objs[children[0]].name

    out: list[NodeSpec] = []
    for node in db.objects_of_class("Node"):
        out.append(
            NodeSpec(
                object_id=node.object_id,
                name=node.name,
                region=region_map.get(node.object_id),
                zone=zone_map.get(node.object_id),
            )
        )
    return tuple(out)


#: PLEXOS Fuel-class property names that some bundles use to ship the
#: CO₂ combustion emission factor as a direct property on the Fuel
#: object (System→Fuels collection).  Older schemas (RTS-96, 118-Bus)
#: occasionally surface them here; the CEN PCP daily bundle leaves
#: them empty and relies on the Emission→Fuel membership path instead.
_FUEL_CO2_DIRECT_PROPS: tuple[str, ...] = (
    "CO2 Production Rate",
    "Emission Production Rate",
    "Emissions Rate",
    "Emissions",
    "Production Rate",
)

#: Direct-property names for the upstream / well-to-tank component.
#: Most PLEXOS releases ship only the combustion factor on the Fuel
#: object — the upstream component, when set, travels exclusively on
#: the Emission→Fuel membership.  We probe a couple of plausible
#: spellings anyway for forward compatibility.
_FUEL_CO2_UPSTREAM_DIRECT_PROPS: tuple[str, ...] = (
    "Upstream Production Rate",
    "Upstream Emission Rate",
)


def _is_co2_emission(name: str) -> bool:
    """True when ``name`` identifies a CO₂ Emission object.

    PLEXOS Emission objects are user-named (no class-level CO₂ flag),
    so the converter matches by substring: ``CO2`` / ``CO₂`` /
    ``Carbon`` / ``Dióxido``.  Match is case-insensitive and
    diacritic-tolerant.
    """
    if not name:
        return False
    lo = name.lower()
    return (
        "co2" in lo
        or "co₂" in lo  # subscript-2 unicode
        or "carbon" in lo
        or "dióxido" in lo
        or "dioxido" in lo
    )


def _extract_fuel_co2_membership_rates(
    db: PlexosDb,
) -> dict[int, tuple[float, float]]:
    """Walk Emission→Fuel ("Fuels") memberships, return per-fuel CO₂ rates.

    Returns ``{fuel_object_id: (combustion_rate, upstream_rate)}`` —
    only fuels with a non-zero rate appear.  When multiple CO₂
    Emission objects target the same Fuel (rare; PLEXOS allows
    "CO2 (Combustion)" + "CO2 (Upstream)" split entries), the
    highest combustion + highest upstream win, matching PLEXOS' own
    "single Emission per component" convention.

    The convention here is:
      * ``Production Rate`` → combustion component (tank-to-stack).
      * Emission objects whose name contains ``upstream`` /
        ``pre-combustion`` / ``WTT`` / ``well-to-tank`` route their
        ``Production Rate`` into the upstream component instead.
    """
    coll = db.collection_for_named("Emission", "Fuel", "Fuels")
    if coll is None:
        return {}
    prop_id = db.property_by_name(coll.collection_id, "Production Rate")
    if prop_id is None:
        return {}
    objs = db.object_by_id()
    out: dict[int, tuple[float, float]] = {}
    for mem in db.memberships_of(coll.collection_id):
        emission = objs.get(mem.parent_object_id)
        if emission is None or not _is_co2_emission(emission.name):
            continue
        rows = db.data_for(mem.membership_id, prop_id)
        if not rows:
            continue
        rows.sort(key=lambda r: r.data_id)
        rate = rows[0].value
        # Drop PLEXOS ±1e+30 sentinel just like `static_property` does.
        if abs(rate) >= 1.0e20:
            continue
        if rate == 0.0:
            continue
        comb, up = out.get(mem.child_object_id, (0.0, 0.0))
        lo = emission.name.lower()
        is_upstream = (
            "upstream" in lo
            or "pre-combustion" in lo
            or "pre combustion" in lo
            or "wtt" in lo
            or "well-to-tank" in lo
            or "well to tank" in lo
        )
        if is_upstream:
            up = max(up, rate)
        else:
            comb = max(comb, rate)
        out[mem.child_object_id] = (comb, up)
    return out


def extract_fuels(db: PlexosDb, bundle: PlexosBundle) -> tuple[FuelSpec, ...]:
    """One :class:`FuelSpec` per ``t_object`` in class ``Fuel``.

    Reads the day-of-bundle scalar from ``Fuel_Price.csv`` (long
    format, no BAND column — period-1 value is the day's price; in a
    daily PCP run all 24 periods carry the same monthly value).

    Also resolves CO₂ emission factors via two PLEXOS lookup paths,
    in order:

    1. Direct property on the Fuel object (System→Fuels collection):
       try ``CO2 Production Rate`` then a list of historical synonyms
       (see :data:`_FUEL_CO2_DIRECT_PROPS`).  Used by older schemas.
    2. Emission→Fuel membership: walk the ``Emission → Fuels``
       collection, keep memberships whose parent Emission name
       matches CO₂ heuristics, and read ``Production Rate``.

    The membership-path rate wins when both are non-zero (PLEXOS'
    own resolution order: the Emission object is the authoritative
    pollutant carrier).  Rates are stored in :attr:`FuelSpec.co2_rate`
    (combustion) and :attr:`FuelSpec.co2_upstream_rate` (upstream).
    """
    prices: dict[str, list[float]] = (
        read_long(bundle.csv("Fuel_Price.csv")) if bundle.has("Fuel_Price.csv") else {}
    )
    membership_rates = _extract_fuel_co2_membership_rates(db)
    out: list[FuelSpec] = []
    for fuel in db.objects_of_class("Fuel"):
        series = prices.get(fuel.name, [])
        price = series[0] if series else 0.0
        if price == 0.0:
            # Fallback: XML-only schemas (RTS-96) ship the fuel
            # price as a static property on the System→Fuels collection.
            price = db.static_property("Fuel", fuel.object_id, "Price")

        # Direct property lookup — first non-zero hit wins.
        co2_direct = 0.0
        for prop_name in _FUEL_CO2_DIRECT_PROPS:
            co2_direct = db.static_property("Fuel", fuel.object_id, prop_name)
            if co2_direct != 0.0:
                break
        co2_up_direct = 0.0
        for prop_name in _FUEL_CO2_UPSTREAM_DIRECT_PROPS:
            co2_up_direct = db.static_property("Fuel", fuel.object_id, prop_name)
            if co2_up_direct != 0.0:
                break

        # Emission→Fuel membership path (preferred when present).
        co2_mem, co2_up_mem = membership_rates.get(fuel.object_id, (0.0, 0.0))
        co2_rate = co2_mem if co2_mem != 0.0 else co2_direct
        co2_upstream_rate = co2_up_mem if co2_up_mem != 0.0 else co2_up_direct

        out.append(
            FuelSpec(
                object_id=fuel.object_id,
                name=fuel.name,
                price=price,
                co2_rate=co2_rate,
                co2_upstream_rate=co2_upstream_rate,
            )
        )
    return tuple(out)


def extract_generators(db: PlexosDb, bundle: PlexosBundle) -> tuple[GeneratorSpec, ...]:
    """One :class:`GeneratorSpec` per Generator object.

    Bus resolved via ``Generator → Nodes`` collection. Fuel-memberships
    flag the unit as thermal (vs renewable). Per-unit per-hour
    availability comes from ``Gen_Rating.csv`` (long, band=1).
    Min stable level from ``Gen_MinStableLevel.csv`` (long, band=1,
    period 1 scalar since v0 collapses to first-period for static
    fields).  Fuel transport charge ($/MWh) comes from
    ``Gen_FuelTransportCharge.csv`` and is matched per-(generator, fuel)
    via the PLEXOS ``<gen_name><fuel_name>`` key convention.
    """
    bus_map = _gen_bus_map(db)
    fuel_map = _gen_fuel_map(db)

    pmax_profiles: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_Rating.csv")) if bundle.has("Gen_Rating.csv") else {}
    )
    pmin_profiles: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_MinStableLevel.csv"))
        if bundle.has("Gen_MinStableLevel.csv")
        else {}
    )
    # Cost inputs — long-format CSVs without a BAND column.
    heat_rate_csv: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_HeatRate.csv"))
        if bundle.has("Gen_HeatRate.csv")
        else {}
    )
    vom_csv: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_VOMCharge.csv"))
        if bundle.has("Gen_VOMCharge.csv")
        else {}
    )
    # Gen_FuelTransportCharge.csv is a long-format CSV
    # (``NAME, YEAR, MONTH, DAY, PERIOD, VALUE`` — no BAND column) where
    # ``NAME`` is the PLEXOS-style ``<generator_name><fuel_name>``
    # concatenation (one row per generator-fuel pair).  Only PERIOD=1 is
    # populated in CEN PCP daily bundles, so we use the period-1 scalar
    # — same collapse rule as Gen_HeatRate / Gen_VOMCharge.  USD/MWh.
    fuel_transport_csv: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_FuelTransportCharge.csv"))
        if bundle.has("Gen_FuelTransportCharge.csv")
        else {}
    )

    out: list[GeneratorSpec] = []
    for gen in db.objects_of_class("Generator"):
        bus_name = bus_map.get(gen.object_id)
        if bus_name is None:
            # Generators not attached to a node are PLEXOS-disabled
            # phantoms (pumps with only Storage memberships, dummy
            # objects, …). Drop them silently.
            continue
        profile = tuple(pmax_profiles.get(gen.name, ()))
        # Use the max of the per-hour profile as the static pmax (gtopt
        # will multiply by pmax_factor when we emit the profile).
        pmax = max(profile) if profile else 0.0
        if pmax == 0.0:
            # Fallback: XML-only schemas (118-Bus, RTS-96) carry
            # ``Max Capacity`` on the System→Generators collection.
            pmax = db.static_property("Generator", gen.object_id, "Max Capacity")
        # PLEXOS Min Stable Level is per-docs commitment-conditional
        # (Generator.MinStableLevel: applies only when the unit is
        # committed).  It travels via ``CommitmentSpec.pmin`` (set in
        # ``extract_commitments``), not here.  ``GeneratorSpec.pmin``
        # is the always-on floor and stays 0 for CEN PCP (no
        # corresponding PLEXOS property today).
        pmin = 0.0
        _ = pmin_profiles  # reserved for future always-on extraction
        # PLEXOS heat rate is in kJ/kWh (or kcal/kWh) and ships per-hour;
        # the daily PCP keeps a constant value, so first-period scalar
        # is the right collapse. VO&M is $/MWh, fuel-transport ditto.
        hr_series = heat_rate_csv.get(gen.name, [])
        heat_rate = hr_series[0] if hr_series else 0.0
        if heat_rate == 0.0:
            # PLEXOS heat-rate conventions, in order of preference:
            #  1. ``Heat Rate``       — single scalar (RTS-96, CEN PCP)
            #  2. ``Heat Rate Incr``  — linear marginal slope (118-Bus
            #     piecewise); the right scalar for gtopt's ``gcost``
            #     because it represents marginal cost per MW.
            #  3. ``Heat Rate Base``  — no-load intercept; faithfully
            #     honouring it needs a gtopt no-load-cost feature.
            heat_rate = (
                db.static_property("Generator", gen.object_id, "Heat Rate")
                or db.static_property("Generator", gen.object_id, "Heat Rate Incr")
                or db.static_property("Generator", gen.object_id, "Heat Rate Base")
            )
        vom_series = vom_csv.get(gen.name, [])
        vom = vom_series[0] if vom_series else 0.0
        if vom == 0.0:
            vom = db.static_property("Generator", gen.object_id, "VO&M Charge")
        # Fuel transport charge ($/MWh): PLEXOS keys each row by the
        # ``<generator_name><fuel_name>`` concatenation (one row per gen
        # × fuel attachment).  Use the primary-fuel match — same fuel
        # the writer picks for gcost (``fuel_names[0]``).  Fall back to
        # exact gen.name (XML-only schemas with no fuel suffix).
        fuel_transport = 0.0
        if fuel_transport_csv:
            gen_fuel_names = fuel_map.get(gen.object_id, ())
            for fname in gen_fuel_names:
                series = fuel_transport_csv.get(gen.name + fname)
                if series:
                    fuel_transport = series[0]
                    break
            if fuel_transport == 0.0:
                series = fuel_transport_csv.get(gen.name)
                if series:
                    fuel_transport = series[0]
        # 118-Bus prices fuel per-generator instead of per-fuel.
        fuel_price_override = db.static_property(
            "Generator", gen.object_id, "Fuel Price"
        )
        # Piecewise heat rate (PLEXOS quadratic form):
        #   H(P) = Base + Incr × P + Incr2 × P²
        # Marginal heat rate dH/dP = Incr + 2 × Incr2 × P.
        # We build a 2-segment approximation by evaluating the
        # marginal at the midpoint of each half of the [pmin, pmax]
        # interval. Mutually exclusive with the scalar ``heat_rate``
        # path (the writer prefers segments when both are present).
        hr_base = db.static_property("Generator", gen.object_id, "Heat Rate Base")
        hr_incr = db.static_property("Generator", gen.object_id, "Heat Rate Incr")
        hr_incr2 = db.static_property("Generator", gen.object_id, "Heat Rate Incr2")
        pmax_segments: tuple[float, ...] = ()
        hr_segments: tuple[float, ...] = ()
        if hr_incr2 > 0.0 and pmax > 0.0 and hr_incr > 0.0:
            p_mid = pmin + (pmax - pmin) * 0.5
            seg1_mid = pmin + (p_mid - pmin) * 0.5
            seg2_mid = p_mid + (pmax - p_mid) * 0.5
            pmax_segments = (p_mid, pmax)
            hr_segments = (
                hr_incr + 2.0 * hr_incr2 * seg1_mid,
                hr_incr + 2.0 * hr_incr2 * seg2_mid,
            )
        _ = hr_base  # currently routed through Commitment.noload_cost
        out.append(
            GeneratorSpec(
                object_id=gen.object_id,
                name=gen.name,
                bus_name=bus_name,
                pmin=pmin,
                pmax=pmax,
                heat_rate=heat_rate,
                vom_charge=vom,
                fuel_transport=fuel_transport,
                fuel_names=fuel_map.get(gen.object_id, ()),
                pmax_profile=profile,
                fuel_price_override=fuel_price_override,
                pmax_segments=pmax_segments,
                heat_rate_segments=hr_segments,
            )
        )
    return tuple(out)


def extract_lines(db: PlexosDb, bundle: PlexosBundle) -> tuple[LineSpec, ...]:
    """One :class:`LineSpec` per Line object with two valid endpoints.

    Endpoints come from ``Node From`` / ``Node To`` collections.
    Ratings come from ``Lin_MaxRating.csv`` / ``Lin_MinRating.csv``
    (long, band=1, period-1 scalar — daily PCP runs invariant ratings
    across periods, so taking period 1 keeps the daily LP a single LP).
    Parallel-line count from ``Lin_Units.csv`` (wide-with-band; first
    period dictates "any units online today"). When ``Lin_Units`` is
    all zero for a line, the line is treated as disconnected for the
    whole day and skipped — matches PLEXOS behaviour.
    """
    endpoints = _line_bus_endpoints(db)

    max_rating: dict[str, list[float]] = (
        read_long(bundle.csv("Lin_MaxRating.csv"))
        if bundle.has("Lin_MaxRating.csv")
        else {}
    )
    min_rating: dict[str, list[float]] = (
        read_long(bundle.csv("Lin_MinRating.csv"))
        if bundle.has("Lin_MinRating.csv")
        else {}
    )
    units_wide: dict[str, list[float]] = (
        read_wide(bundle.csv("Lin_Units.csv"), drop_zero_cols=False)
        if bundle.has("Lin_Units.csv")
        else {}
    )

    out: list[LineSpec] = []
    for line in db.objects_of_class("Line"):
        f_name, t_name = endpoints.get(line.object_id, (None, None))
        if f_name is None or t_name is None:
            logger.debug(
                "line %s missing endpoint(s) (from=%s to=%s); skipping",
                line.name,
                f_name,
                t_name,
            )
            continue
        units_profile = units_wide.get(line.name)
        if units_profile is not None and not any(u > 0.0 for u in units_profile):
            # All-zero across the day = mothballed; PLEXOS would drop it.
            continue
        raw_units = int(units_profile[0]) if units_profile else 1
        units = raw_units
        if units <= 0:
            # First-hour offline but back later — clamp to 1 so the
            # daily LP can still dispatch the line.  A per-block
            # tmax_ab matrix would honour the per-hour availability
            # exactly; not needed for the current daily PCP horizon.
            # Visible WARN so the clamp doesn't hide a fully-offline
            # line (PLEXOS would drop those at the top of this loop).
            logger.warning(
                "Line '%s' (uid=%s): hour-0 units=%d in profile — "
                "clamping to 1 (LP needs a dispatchable line for the "
                "daily horizon).  Inspect Units_Built / Commissioned "
                "schedule if this isn't a transient first-hour outage.",
                line.name,
                line.object_id,
                raw_units,
            )
            units = 1
        tmax_series = max_rating.get(line.name, [])
        tmin_series = min_rating.get(line.name, [])
        tmax = tmax_series[0] if tmax_series else 0.0
        tmin = tmin_series[0] if tmin_series else 0.0
        if tmax <= 0.0:
            # Fallback: XML-only schemas ship Max Flow on the
            # System→Lines collection.
            tmax = db.static_property("Line", line.object_id, "Max Flow")
        if tmin == 0.0:
            # PLEXOS stores Min Flow as a negative number; default 0
            # means "unset" — fall back to t_data.
            tmin = db.static_property("Line", line.object_id, "Min Flow")
        if tmax <= 0.0:
            logger.debug("line %s has tmax<=0; skipping", line.name)
            continue
        # pull static electrical parameters from t_data so the LP
        # can run under use_kirchhoff=true (DC OPF). Reactance is in
        # per-unit (system MVA base, gtopt-agnostic). Wheeling charge
        # is $/MWh.
        reactance = db.static_property("Line", line.object_id, "Reactance")
        wheeling = db.static_property("Line", line.object_id, "Wheeling Charge")
        # PLEXOS Enforce Limits: 0=Never, 1=Voltage, 2=Always.  Defaults
        # to 1 ("enforce") when the property is unset to stay safe on
        # legacy bundles where the property pre-dates the feature.
        enforce_raw = db.static_property("Line", line.object_id, "Enforce Limits")
        enforce_limits = int(enforce_raw) if enforce_raw is not None else 1
        out.append(
            LineSpec(
                object_id=line.object_id,
                name=line.name,
                bus_from=f_name,
                bus_to=t_name,
                tmax_ab=tmax,
                tmin_ab=tmin,
                units=units,
                reactance=reactance,
                wheeling_charge=wheeling,
                enforce_limits=enforce_limits,
            )
        )
    return tuple(out)


def extract_demands(db: PlexosDb, bundle: PlexosBundle) -> tuple[DemandSpec, ...]:
    """One :class:`DemandSpec` per non-zero bus load.

    Two sources, in priority order:

    1. ``Nod_Load.csv`` (wide format, per-hour) — the CEN PCP convention.
    2. ``t_data`` ``Load`` property on the System→Nodes collection —
       used by XML-only benchmarks like ``118-Bus.xml``. The value
       there is a single scalar that the writer broadcasts across the
       24-block horizon.
    """
    if bundle.has("Nod_Load.csv"):
        load_by_bus = read_wide(bundle.csv("Nod_Load.csv"))
        out: list[DemandSpec] = []
        for bus_name, profile in load_by_bus.items():
            out.append(
                DemandSpec(
                    name=f"load_{bus_name}",
                    bus_name=bus_name,
                    lmax_profile=tuple(profile),
                )
            )
        return tuple(out)
    # Fallback: pull scalar Load from t_data per Node object.
    out_fallback: list[DemandSpec] = []
    for node in db.objects_of_class("Node"):
        load = db.static_property("Node", node.object_id, "Load")
        if load <= 0.0:
            continue
        out_fallback.append(
            DemandSpec(
                name=f"load_{node.name}",
                bus_name=node.name,
                lmax_profile=(load,) * 24,
            )
        )
    return tuple(out_fallback)


def extract_batteries(db: PlexosDb, bundle: PlexosBundle) -> tuple[BatterySpec, ...]:
    """One :class:`BatterySpec` per Battery object attached to a Node.

    Initial SOC comes from ``BESS_IniValue.csv`` (long format, no BAND
    column).  **Important PLEXOS convention**: the CSV column reports
    ``Initial SoC`` as a *percentage* (0–100), not absolute MWh — the
    underlying PLEXOS property name is literally "Initial SoC" (State
    of Charge).  We multiply by ``Capacity / 100`` here to obtain MWh
    matching gtopt's ``Battery.eini`` semantics.  Skipping the scaling
    makes the LP infeasible whenever ``SoC% × < 100% × Capacity`` is
    interpreted as MWh > Capacity (e.g. BAT_ARICA: SoC=76.388,
    Capacity=2.0 MWh ⇒ unscaled eini = 76.388 MWh ≫ emax = 2.0 MWh).

    Capacity / Max Power / Charge & Discharge Efficiency are pulled
    from ``t_data`` on the System→Batteries collection. CEN PCP
    bundles ship a single ``Max Power`` (symmetric charge/discharge);
    efficiencies arrive in percent and are scaled to fractions here.
    """
    bus_map = _battery_bus_map(db)
    ini_soc_pct: dict[str, list[float]] = {}
    if bundle.has("BESS_IniValue.csv"):
        # BESS_IniValue has no BAND column — long format with NAME, YEAR,
        # MONTH, DAY, PERIOD, VALUE. Period 1 = day-start SOC (percent).
        ini_soc_pct = read_long(bundle.csv("BESS_IniValue.csv"))
    out: list[BatterySpec] = []
    for batt in db.objects_of_class("Battery"):
        bus_name = bus_map.get(batt.object_id)
        if bus_name is None:
            continue
        # pull SOC bounds + power rating from t_data. CEN PCP only
        # ships symmetric Max Power, so charge/discharge limits share
        # the value (DESIGN.md §9 resolved: 2026-04-22 bundle has no
        # separate Max Power Charge / Discharge properties).
        capacity = db.static_property("Battery", batt.object_id, "Capacity")
        max_power = db.static_property("Battery", batt.object_id, "Max Power")
        charge_eff_pct = db.static_property(
            "Battery", batt.object_id, "Charge Efficiency", default=100.0
        )
        discharge_eff_pct = db.static_property(
            "Battery", batt.object_id, "Discharge Efficiency", default=100.0
        )
        # PLEXOS "Initial SoC" is a percentage of Capacity, not MWh.
        soc_pct_series = ini_soc_pct.get(batt.name, [])
        soc_pct = soc_pct_series[0] if soc_pct_series else 0.0
        eini = (soc_pct / 100.0) * capacity if capacity > 0.0 else 0.0
        # Max SoC: same convention as Initial SoC — percentage of
        # Capacity.  CEN PCP ships 100% on every battery, so the
        # default behaviour is unchanged; the scaling is here for
        # forward-compat with bundles that derate Max SoC.
        max_soc_pct = (
            db.static_property("Battery", batt.object_id, "Max SoC", default=100.0)
            or 100.0
        )
        emax_mwh = (max_soc_pct / 100.0) * capacity if capacity > 0.0 else 0.0
        # Min Charge / Discharge Level (MW): minimum dispatch power
        # when the battery is actively charging / discharging.  Only 2
        # batteries carry these in CEN PCP.
        pmin_charge = (
            db.static_property("Battery", batt.object_id, "Min Charge Level") or 0.0
        )
        pmin_discharge = (
            db.static_property("Battery", batt.object_id, "Min Discharge Level") or 0.0
        )
        out.append(
            BatterySpec(
                object_id=batt.object_id,
                name=batt.name,
                bus_name=bus_name,
                emin=0.0,
                emax=emax_mwh,
                eini=eini,
                efin=0.0,
                pmax_charge=max_power,
                pmax_discharge=max_power,
                pmin_charge=pmin_charge,
                pmin_discharge=pmin_discharge,
                # PLEXOS reports efficiency as a percentage (97 → 0.97).
                input_efficiency=charge_eff_pct / 100.0,
                output_efficiency=discharge_eff_pct / 100.0,
            )
        )
    return tuple(out)


def extract_reservoirs(db: PlexosDb, bundle: PlexosBundle) -> tuple[ReservoirSpec, ...]:
    """One :class:`ReservoirSpec` per PLEXOS Storage object.

    Volume bounds come from the per-Storage hydro CSVs (long format,
    no BAND column). Each row carries the day's value at period 1;
    we collapse to that scalar — multi-day variation would need a
    per-stage bound matrix, which neither the daily PCP horizon nor
    the current writer exercises. Static ``t_data`` fallback on the
    System→Storages collection covers XML-only schemas that ship
    volumes inline.
    """
    emax_csv = (
        read_long(bundle.csv("Hydro_MaxVolume.csv"))
        if bundle.has("Hydro_MaxVolume.csv")
        else {}
    )
    emin_csv = (
        read_long(bundle.csv("Hydro_MinVolume.csv"))
        if bundle.has("Hydro_MinVolume.csv")
        else {}
    )
    eini_csv = (
        read_long(bundle.csv("Hydro_InitialVolume.csv"))
        if bundle.has("Hydro_InitialVolume.csv")
        else {}
    )
    out: list[ReservoirSpec] = []
    for storage in db.objects_of_class("Storage"):
        name = storage.name
        emax_series = emax_csv.get(name, [])
        emin_series = emin_csv.get(name, [])
        eini_series = eini_csv.get(name, [])
        emax = emax_series[0] if emax_series else 0.0
        emin = emin_series[0] if emin_series else 0.0
        eini = eini_series[0] if eini_series else 0.0
        if emax == 0.0:
            emax = db.static_property("Storage", storage.object_id, "Max Volume")
        if emin == 0.0:
            emin = db.static_property("Storage", storage.object_id, "Min Volume")
        if eini == 0.0:
            eini = db.static_property("Storage", storage.object_id, "Initial Volume")
        # PLEXOS Spill Penalty ($/MWh) — controlled-spill cost.  Per the
        # Energy Exemplar docs (Storage.MaxSpill page) ``Max Spill`` is
        # the per-storage spillway capacity to "the sea" with default
        # ``1E+30`` (unlimited); when undefined, spillage is restricted
        # to the explicit ``Waterway`` graph.  ``Spill Penalty`` adds a
        # cost on top.  CEN PCP ships all three Storage spill properties
        # (``Spill Penalty``, ``Non-physical Spill Penalty``, ``Max
        # Spill``) at 0 / unset across every reservoir, so this lookup
        # returns 0 today — kept for forward-compat with bundles that
        # do carry a per-reservoir penalty.
        spill_penalty = (
            db.static_property("Storage", storage.object_id, "Spill Penalty") or 0.0
        )
        # PLEXOS ``Water Value`` ($/GWh) — end-of-horizon opportunity
        # cost of stored water.  Per the Energy Exemplar docs
        # (Storage.WaterValue page) it functions as a *linear term on
        # end-of-horizon storage state*, NOT as Benders/SDDP optimal
        # cuts.  Multi-band piecewise mode exists in PLEXOS but the CEN
        # PCP bundle uses the scalar form: default 10,000 $/GWh on most
        # reservoirs, plus a sentinel ``1e+30`` on virtual storages like
        # ``L_Maule``.  Clamp the sentinel to a finite value so LP
        # coefficient ratios stay sane.
        raw_water_value_gwh = (
            db.static_property(
                "Storage", storage.object_id, "Water Value", keep_sentinel=True
            )
            or 0.0
        )
        # Sentinel >1e12 (PLEXOS uses 1e+30) indicates "never drain":
        # the reservoir must keep at least its initial volume.  We do
        # NOT clamp to a finite price (a finite efin_cost would let the
        # LP buy out of the sentinel at that price — which is exactly
        # what the sentinel forbids).  Instead we drop the water value
        # entirely and set `never_drain=True`; the writer then emits
        # `efin = eini` as a HARD `vol_end >= eini` constraint with no
        # `efin_cost` slack.
        water_value_gwh = raw_water_value_gwh
        never_drain = False
        if water_value_gwh > 1.0e12:
            logger.warning(
                "Storage '%s' (uid=%s): Water Value %.3e $/GWh exceeds "
                "1e12 ceiling — treating as PLEXOS 1e+30 never-drain "
                "sentinel: dropping water_value and emitting hard "
                "`efin = eini` constraint (no efin_cost slack).",
                name,
                storage.object_id,
                water_value_gwh,
            )
            water_value_gwh = 0.0
            never_drain = True
        out.append(
            ReservoirSpec(
                object_id=storage.object_id,
                name=name,
                emin=emin,
                emax=emax,
                eini=eini,
                water_value=water_value_gwh,
                never_drain=never_drain,
                spill_penalty_per_mwh=spill_penalty,
            )
        )
    return tuple(out)


def extract_waterways(db: PlexosDb) -> tuple[WaterwaySpec, ...]:
    """One :class:`WaterwaySpec` per PLEXOS Waterway object.

    Endpoints come from the Waterway→Storage ``Storage From`` /
    ``Storage To`` collections (collections 104 / 105 in the CEN PCP
    schema). Waterways with only one endpoint are dropped — a 1-ended
    waterway can't add a constraint to the LP. The optional fmin/fmax
    are static properties on the System→Waterways collection.
    """
    from_coll = db.collection_for_named("Waterway", "Storage", "Storage From")
    to_coll = db.collection_for_named("Waterway", "Storage", "Storage To")
    objs = db.object_by_id()
    p2c_from = db.parent_to_children(from_coll.collection_id) if from_coll else {}
    p2c_to = db.parent_to_children(to_coll.collection_id) if to_coll else {}
    out: list[WaterwaySpec] = []
    for ww in db.objects_of_class("Waterway"):
        fs = p2c_from.get(ww.object_id, [])
        ts = p2c_to.get(ww.object_id, [])
        f_name = objs[fs[0]].name if fs and fs[0] in objs else None
        t_name = objs[ts[0]].name if ts and ts[0] in objs else None
        if f_name is None or t_name is None:
            logger.debug(
                "waterway %s missing endpoint(s) (from=%s to=%s); skipping",
                ww.name,
                f_name,
                t_name,
            )
            continue
        fmin = db.static_property("Waterway", ww.object_id, "Min Flow")
        fmax = db.static_property("Waterway", ww.object_id, "Max Flow")
        # PLEXOS Max Flow Penalty: -1.0 sentinel = "feature deactivated"
        # (matches PLEXOS convention for negative-default flags); only
        # positive values translate into a real fcost.  CEN PCP uses
        # 3.6 on most Vert_* spillways and 7200/360 on a handful of
        # high-cost arcs.
        fcost_raw = db.static_property("Waterway", ww.object_id, "Max Flow Penalty")
        fcost = fcost_raw if fcost_raw and fcost_raw > 0.0 else 0.0
        out.append(
            WaterwaySpec(
                object_id=ww.object_id,
                name=ww.name,
                storage_from=f_name,
                storage_to=t_name,
                fmin=fmin,
                fmax=fmax,
                fcost=fcost,
            )
        )
    return tuple(out)


def extract_junctions(
    reservoirs: tuple[ReservoirSpec, ...],
) -> tuple[JunctionSpec, ...]:
    """Synthesise one Junction per Reservoir.

    gtopt requires explicit Junction nodes; PLEXOS folds the
    storage+topology concept into a single Storage object. We emit
    one Junction per Reservoir with the same name (the Reservoir
    JSON binding accepts a ``junction`` ref by name).
    """
    return tuple(JunctionSpec(name=r.name) for r in reservoirs)


def extract_turbines(db: PlexosDb, _bundle: PlexosBundle) -> tuple[TurbineSpec, ...]:
    """One :class:`TurbineSpec` per Generator with a Head Storage link.

    PLEXOS encodes the turbine-to-reservoir relationship via the
    Generator→Storage "Head Storage" collection. We populate
    ``production_factor`` (MW per m³/s) from t_data fallback —
    ``Hydro_EfficiencyIncr.csv`` carries the same on the CSV side but
    is keyed by storage, not generator, so the t_data path on the
    Generator System collection is more direct.
    """
    head_coll = db.collection_for_named("Generator", "Storage", "Head Storage")
    if head_coll is None:
        return ()
    objs = db.object_by_id()
    out: list[TurbineSpec] = []
    for m in db.memberships_of(head_coll.collection_id):
        gen_obj = objs.get(m.parent_object_id)
        res_obj = objs.get(m.child_object_id)
        if gen_obj is None or res_obj is None:
            continue
        # Production factor: try the t_data ``Production Rate`` /
        # ``Head Loss Coefficient`` properties on the Generator System
        # collection; CEN PCP omits this for many units, leaving us at
        # zero. The writer falls back to the global default.
        pf = db.static_property("Generator", gen_obj.object_id, "Production Rate")
        out.append(
            TurbineSpec(
                generator_name=gen_obj.name,
                reservoir_name=res_obj.name,
                production_factor=pf,
            )
        )
    return tuple(out)


def extract_flows(
    db: PlexosDb,
    bundle: PlexosBundle,
    known_junctions: frozenset[str] = frozenset(),
) -> tuple[FlowSpec, ...]:
    """One :class:`FlowSpec` per per-reservoir inflow column.

    ``Hydro_WaterFlows.csv`` is wide-format (one column per Storage,
    one row per period). Each column with non-zero values becomes a
    Flow that pushes natural inflow into the matching Junction.

    PLEXOS occasionally ships inflow columns for virtual filtration
    intermediates (``Filt_Laja``, ``Filt_Inv``, …) that are not Storage
    objects in the XML. Those would dangle Flows against non-existent
    Junctions, so the writer drops them when ``known_junctions`` is
    populated. Pass an empty set to keep every column for diagnostics.
    """
    _ = db  # reserved for future use (per-Storage Flow scaling factor)
    if not bundle.has("Hydro_WaterFlows.csv"):
        return ()
    inflows = read_wide(bundle.csv("Hydro_WaterFlows.csv"))
    out: list[FlowSpec] = []
    dropped: list[str] = []
    for storage_name, profile in inflows.items():
        if known_junctions and storage_name not in known_junctions:
            dropped.append(storage_name)
            continue
        out.append(
            FlowSpec(
                name=f"inflow_{storage_name}",
                junction_name=storage_name,
                discharge_profile=tuple(profile),
            )
        )
    if dropped:
        logger.debug(
            "Hydro_WaterFlows.csv dropped %d inflow columns with no matching "
            "Storage/Junction: %s",
            len(dropped),
            ", ".join(sorted(dropped)),
        )
    return tuple(out)


def _parse_res_requirement_csv(
    path: Path, reserve_names: frozenset[str]
) -> dict[str, list[float]]:
    """Parse ``Res_Requirement.csv``'s ``NAME, PATTERN, VALUE`` layout.

    Only rows whose ``NAME`` matches a known Reserve object are kept;
    PATTERN ``"DO_d,Hh"`` is parsed for ``Hh`` (1..24). Day-of-week
    field ``DO_d`` is ignored — CEN PCP is a daily run.

    Returns ``{reserve_name -> 24-element profile}``.
    """
    import csv  # pylint: disable=import-outside-toplevel

    hour_re = re.compile(r"H(\d+)")
    out: dict[str, list[float]] = {}
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            name = (row.get("NAME") or "").strip()
            if name not in reserve_names:
                continue
            pattern = row.get("PATTERN") or ""
            m = hour_re.search(pattern)
            if m is None:
                continue
            try:
                hour = int(m.group(1))
                value = float(row.get("VALUE") or 0.0)
            except ValueError:
                continue
            if hour < 1 or hour > 24:
                continue
            series = out.setdefault(name, [0.0] * 24)
            series[hour - 1] = value
    return out


def extract_reserves(db: PlexosDb, bundle: PlexosBundle) -> tuple[ReserveSpec, ...]:
    """One :class:`ReserveSpec` per ``t_object`` in class ``Reserve``.

    Up/down direction comes from a name-suffix convention: PLEXOS CEN
    PCP names reserves ``<type>_LW`` for lower (down) reserve and
    ``<type>_RS`` for raise/spinning (up) reserve. Unknown suffixes
    default to up-reserve. The Reserve→Generator membership table
    populates :attr:`ReserveSpec.eligible_generators` for downstream
    :func:`extract_reserve_provisions` consumption.
    """
    reserves_objs = db.objects_of_class("Reserve")
    if not reserves_objs:
        return ()
    reserve_names = frozenset(r.name for r in reserves_objs)
    csv_profiles: dict[str, list[float]] = {}
    if bundle.has("Res_Requirement.csv"):
        csv_profiles = _parse_res_requirement_csv(
            bundle.csv("Res_Requirement.csv"), reserve_names
        )

    # Pull the Reserve→Generator membership for eligibility.
    elig_coll = db.collection_for_named("Reserve", "Generator", "Generators")
    objs = db.object_by_id()
    eligibility: dict[str, list[str]] = {}
    if elig_coll is not None:
        for m in db.memberships_of(elig_coll.collection_id):
            rsv_obj = objs.get(m.parent_object_id)
            gen_obj = objs.get(m.child_object_id)
            if rsv_obj is None or gen_obj is None:
                continue
            eligibility.setdefault(rsv_obj.name, []).append(gen_obj.name)

    # PLEXOS Reserve.Type → type_tag mapping.  Per the Energy Exemplar
    # Reserve.Type docs: 1=Regulation, 2=Spinning, 3=Regulation Raise,
    # 4=Regulation Lower, 5=Replacement, 6=Tertiary.  CEN PCP uses
    # 2/3/4/6 (no plain 1=Regulation; the raise/lower split via 3/4
    # carries the same intent).
    type_tag_map = {
        1: "regulation",
        2: "spinning",
        3: "regulation",
        4: "regulation",
        5: "replacement",
        6: "tertiary",
    }
    # PLEXOS reserve-shortage penalty is exposed under one of several
    # property names depending on PLEXOS version / template.  Probe the
    # canonical name first, then a documented set of fallbacks; CEN PCP
    # uses ``VoRS`` (Value of Reserve Shortage).  Values <= 0 are
    # treated as "unset" / sentinel (PLEXOS uses ``-1`` to mean "use
    # system default / disabled") and folded to ``0.0`` so the LP row
    # stays unpenalised rather than rewarding shortage.
    violation_cost_props = (
        "Violation Cost",
        "Shortage Penalty",
        "Penalty Cost",
        "VoRS",
        "Cost",
    )
    out: list[ReserveSpec] = []
    for rsv in reserves_objs:
        profile = csv_profiles.get(rsv.name, [])
        is_down = rsv.name.endswith("_LW")
        ur_req: tuple[float, ...] = ()
        dr_req: tuple[float, ...] = ()
        if profile:
            if is_down:
                dr_req = tuple(profile)
            else:
                ur_req = tuple(profile)
        plexos_type_raw = db.static_property("Reserve", rsv.object_id, "Type")
        plexos_type = int(plexos_type_raw) if plexos_type_raw else 0
        type_tag = type_tag_map.get(plexos_type, "other")
        violation_cost = 0.0
        for prop_name in violation_cost_props:
            val = db.static_property("Reserve", rsv.object_id, prop_name)
            if val and val > 0.0:
                violation_cost = float(val)
                break
        # PLEXOS Type 4 = Regulation Lower (down reserve); everything
        # else (1/2/3/5/6/unknown) is treated as a raise/up product.
        # The Reserve object carries a single shortage cost, applied in
        # the direction of the reserve.  Name suffix ``_LW`` is a
        # belt-and-braces fallback when Type is missing.
        if plexos_type == 4 or is_down:
            urcost = 0.0
            drcost = violation_cost
        else:
            urcost = violation_cost
            drcost = 0.0
        out.append(
            ReserveSpec(
                object_id=rsv.object_id,
                name=rsv.name,
                ur_requirement=ur_req,
                dr_requirement=dr_req,
                eligible_generators=tuple(sorted(set(eligibility.get(rsv.name, [])))),
                plexos_type=plexos_type,
                type_tag=type_tag,
                urcost=urcost,
                drcost=drcost,
            )
        )
    return tuple(out)


def extract_reserve_provisions(
    reserves: tuple[ReserveSpec, ...],
    generators: tuple[GeneratorSpec, ...] = (),
    db: PlexosDb | None = None,
    committed_gens: frozenset[str] = frozenset(),
) -> tuple[ReserveProvisionSpec, ...]:
    """Invert Reserve→Generator memberships into per-Generator
    provisions.

    Single ``ReserveProvisionSpec`` per (generator, direction-of-
    participation) covering ALL the gen's eligible reserve zones —
    matches the legacy user_constraint ``reserve_provision(provision_<gen>)``
    naming convention.  Per-type splitting (one provision per
    (gen, type)) was investigated as Option B but discarded for CEN
    PCP because the user_constraint coefficient references don't
    carry a type — only a gen — and the LP economic difference was
    ~$1M / $212M.  Type-aware splitting would need a coordinated
    refactor of _DIRECT_COEFFS + Reserve.Provision Coefficient
    expansion in extract_user_constraints to emit per-type refs.

    Per-PLEXOS-type Min Provision floors (5 MW Regulation vs 10 MW
    Spinning/Replacement) are aggregated via MAX, giving the strictest
    floor across all types the gen participates in.  This is
    conservative compared to the per-type SUM that PLEXOS would
    impose, but cheaper LP-wise and avoids user_constraint plumbing.

    Floor gating: ``commitment_lp.cpp`` transforms the hard col-lowb
    into the conditional row ``provision - urmin·u_commit ≥ 0`` when
    the gen has a Commitment row.  Only applies urmin/drmin to gens
    with a Commitment (otherwise the hard floor over-forces dispatch
    for small gens whose pmax < floor).
    """
    pmax_by_gen = {g.name: g.pmax for g in generators}
    pmax_varies = {
        g.name: bool(g.pmax_profile) and (max(g.pmax_profile) != min(g.pmax_profile))
        for g in generators
    }
    by_gen: dict[str, list[str]] = {}
    for rsv in reserves:
        for gen_name in rsv.eligible_generators:
            by_gen.setdefault(gen_name, []).append(rsv.name)
    urmin_by_gen: dict[str, float] = {}
    drmin_by_gen: dict[str, float] = {}
    if db is not None:
        elig_coll = db.collection_for_named("Reserve", "Generator", "Generators")
        if elig_coll is not None:
            min_prop_ids = [
                db.property_by_name(elig_coll.collection_id, name)
                for name in (
                    "Min Provision",
                    "Min Spinning Provision",
                    "Min Regulation Provision",
                    "Min Replacement Provision",
                )
            ]
            min_prop_ids = [pid for pid in min_prop_ids if pid is not None]
            if min_prop_ids:
                objs = db.object_by_id()
                for m in db.memberships_of(elig_coll.collection_id):
                    rsv_obj = objs.get(m.parent_object_id)
                    gen_obj = objs.get(m.child_object_id)
                    if rsv_obj is None or gen_obj is None:
                        continue
                    if pmax_varies.get(gen_obj.name, False):
                        continue
                    if gen_obj.name not in committed_gens:
                        continue
                    direction = "dn" if rsv_obj.name.endswith("_LW") else "up"
                    pair_min = 0.0
                    for pid in min_prop_ids:
                        if pid is None:
                            continue
                        for row in db.data_for(m.membership_id, pid):
                            # Skip PLEXOS ±1e+30 sentinel ("unbounded")
                            # — would otherwise force pair_min = 1e+30
                            # and emit a meaningless huge urmin/drmin.
                            if row.value and abs(row.value) < 1.0e20:
                                pair_min = max(pair_min, row.value)
                    if pair_min <= 0.0:
                        continue
                    target = urmin_by_gen if direction == "up" else drmin_by_gen
                    if pair_min > target.get(gen_obj.name, 0.0):
                        target[gen_obj.name] = pair_min
    return tuple(
        ReserveProvisionSpec(
            generator_name=gen_name,
            reserve_zones=tuple(sorted(zones)),
            urmax=pmax_by_gen.get(gen_name, 0.0),
            drmax=pmax_by_gen.get(gen_name, 0.0),
            urmin=urmin_by_gen.get(gen_name, 0.0),
            drmin=drmin_by_gen.get(gen_name, 0.0),
        )
        for gen_name, zones in sorted(by_gen.items())
    )


def extract_commitments(
    db: PlexosDb,
    bundle: PlexosBundle,
    generators: tuple[GeneratorSpec, ...],
    fuels: tuple[FuelSpec, ...] = (),
) -> tuple[CommitmentSpec, ...]:
    """One :class:`CommitmentSpec` per thermal unit with any UC parameter set.

    Pulls per-unit data from the six PLEXOS UC CSV files (when present)
    plus ``t_data`` Min Up / Min Down / Max Ramp Up / Max Ramp Down
    fallbacks. Emits a Commitment only when at least one parameter is
    non-zero — there's no point creating empty Commitment rows that
    would just relax to the LP-only path.

    ``noload_cost`` is computed from PLEXOS quadratic heat-rate form:
    ``Heat Rate Base [MMBtu/hr] × Fuel Price [$/MMBtu]``. For 118-Bus
    style schemas the fuel price lives on the Generator
    (``fuel_price_override``); for CEN PCP / RTS-96 it comes from the
    Fuel object's catalogue price.
    """
    fuel_price_by_name = {f.name: f.price for f in fuels}
    start_cost = (
        read_long(bundle.csv("Gen_StartCost.csv"))
        if bundle.has("Gen_StartCost.csv")
        else {}
    )
    shut_cost = (
        read_long(bundle.csv("Gen_ShutDownCost.csv"))
        if bundle.has("Gen_ShutDownCost.csv")
        else {}
    )
    # PLEXOS Min Stable Level — per-unit when-committed floor; goes
    # into ``CommitmentSpec.pmin`` (distinct from
    # ``GeneratorSpec.pmin`` which is the always-on floor = 0 in CEN
    # PCP).  Hoisted out of the gen loop so the CSV is parsed once.
    msl_csv = (
        read_long(bundle.csv("Gen_MinStableLevel.csv"))
        if bundle.has("Gen_MinStableLevel.csv")
        else {}
    )
    ini_units = (
        read_long(bundle.csv("Gen_IniUnits.csv"))
        if bundle.has("Gen_IniUnits.csv")
        else {}
    )
    ini_hours_up = (
        read_long(bundle.csv("Gen_IniHoursUp.csv"))
        if bundle.has("Gen_IniHoursUp.csv")
        else {}
    )
    ini_hours_down = (
        read_long(bundle.csv("Gen_IniHoursDown.csv"))
        if bundle.has("Gen_IniHoursDown.csv")
        else {}
    )
    out: list[CommitmentSpec] = []
    for gen in generators:
        name = gen.name
        sc = start_cost.get(name, [])
        sd = shut_cost.get(name, [])
        units = ini_units.get(name, [])
        up = ini_hours_up.get(name, [])
        down = ini_hours_down.get(name, [])
        startup_cost = sc[0] if sc else 0.0
        shutdown_cost = sd[0] if sd else 0.0
        initial_status = 1.0 if units and units[0] > 0.0 else 0.0
        # PLEXOS reports IniHoursUp / IniHoursDown as separate
        # non-negative scalars; the active one wins. gtopt's
        # ``initial_hours`` is signed (+ up, − down).
        ih_up = up[0] if up else 0.0
        ih_down = down[0] if down else 0.0
        if ih_up > 0.0:
            initial_hours = ih_up
        elif ih_down > 0.0:
            initial_hours = -ih_down
        else:
            initial_hours = 0.0
        # t_data fallbacks for static UC parameters.
        min_up = db.static_property("Generator", gen.object_id, "Min Up Time")
        min_down = db.static_property("Generator", gen.object_id, "Min Down Time")
        ramp_up = db.static_property("Generator", gen.object_id, "Max Ramp Up")
        ramp_down = db.static_property("Generator", gen.object_id, "Max Ramp Down")
        # PLEXOS ``Run Up Rate`` is expressed in MW/min.  gtopt's
        # ``Commitment.startup_ramp`` is the maximum output [MW] the
        # unit can reach in the startup block.  Block duration in CEN
        # PCP is 1 hour, so the per-block envelope is
        # ``Run Up Rate × 60``.  When the CSV value is unset the
        # parser keeps the gtopt default (unset → no startup ramp
        # constraint, equivalent to startup_ramp = pmax).
        run_up_rate = db.static_property("Generator", gen.object_id, "Run Up Rate")
        startup_ramp = run_up_rate * 60.0 if run_up_rate else 0.0
        # No-load cost: PLEXOS ``Heat Rate Base`` × fuel price. For
        # generators carrying ``Heat Rate Base = 0`` (the CEN PCP and
        # most simple PLEXOS exports), this stays zero and the LP
        # behaves identically to the pre-noload path.
        hr_base = db.static_property("Generator", gen.object_id, "Heat Rate Base")
        if gen.fuel_price_override > 0.0:
            primary_price = gen.fuel_price_override
        else:
            primary_fuel = gen.fuel_names[0] if gen.fuel_names else None
            primary_price = (
                fuel_price_by_name.get(primary_fuel, 0.0) if primary_fuel else 0.0
            )
        noload_cost = hr_base * primary_price
        # PLEXOS Min Stable Level → gtopt Commitment.pmin.
        # ``GeneratorSpec.pmin`` is now the *always-on* floor (= 0
        # for CEN PCP).  CSV first, XML t_data fallback.
        msl_series = msl_csv.get(name, [])
        cmt_pmin = msl_series[0] if msl_series else 0.0
        if cmt_pmin == 0.0:
            cmt_pmin = (
                db.static_property("Generator", gen.object_id, "Min Stable Level")
                or 0.0
            )
        any_param = (
            startup_cost
            or shutdown_cost
            or min_up
            or min_down
            or ramp_up
            or ramp_down
            or startup_ramp
            or initial_hours
            or noload_cost
            or cmt_pmin
        )
        if not any_param:
            continue
        out.append(
            CommitmentSpec(
                generator_name=name,
                startup_cost=startup_cost,
                shutdown_cost=shutdown_cost,
                min_up_time=min_up,
                min_down_time=min_down,
                initial_status=initial_status,
                initial_hours=initial_hours,
                ramp_up=ramp_up,
                ramp_down=ramp_down,
                startup_ramp=startup_ramp,
                noload_cost=noload_cost,
                pmin=cmt_pmin,
            )
        )
    return tuple(out)


def extract_flow_rights(
    bundle: PlexosBundle,
    turbines: tuple[TurbineSpec, ...],
    known_junctions: frozenset[str] = frozenset(),
) -> tuple[FlowRightSpec, ...]:
    """Parse ``Hydro_AntucoBounds.csv`` (Laja irrigation envelope) into FlowRights.

    The CSV ships per-day rows for names like ``ANTUCOmin`` / ``ANTUCOmax`` /
    ``ELTOROmax`` — the suffix selects fmin vs fmax. We collapse to the
    first matching row per name (single-day PCP horizon) and resolve
    the junction via the Generator→Head Storage mapping in
    ``turbines`` (e.g. ``ANTUCO*`` → POLCURA junction).

    Flow rights whose junction cannot be resolved are emitted with
    ``junction_name=None`` and a debug log; the writer drops them
    rather than dangle them against non-existent junctions.
    """
    import csv  # pylint: disable=import-outside-toplevel

    if not bundle.has("Hydro_AntucoBounds.csv"):
        return ()
    # Build name-prefix → junction map from turbines. PLEXOS uses two
    # conventions in ``Hydro_AntucoBounds.csv``:
    #   1. Reservoir name (``ELTOROmax`` → reservoir ELTORO directly).
    #   2. Generator-name prefix (``ANTUCOmin/max`` → POLCURA via the
    #      ANTUCO_U1/U2/U3 turbines that draw from POLCURA).
    # Both are tried in order: a direct reservoir-name match wins,
    # otherwise we fall back to the generator-prefix lookup.
    reservoir_names = {t.reservoir_name for t in turbines}
    prefix_to_junction: dict[str, str] = {}
    for t in turbines:
        # Treat the upper-cased alpha-numeric prefix of the gen name
        # (before any non-alnum separator) as the bound key.
        gen_name = t.generator_name.upper()
        prefix = ""
        for char in gen_name:
            if char.isalpha():
                prefix += char
            else:
                break
        if prefix and prefix not in prefix_to_junction:
            prefix_to_junction[prefix] = t.reservoir_name

    out: list[FlowRightSpec] = []
    seen_names: set[str] = set()
    with Path(bundle.csv("Hydro_AntucoBounds.csv")).open(
        "r", encoding="utf-8", newline=""
    ) as fh:
        reader = csv.reader(fh)
        rows = list(reader)
    if not rows:
        return ()
    # Header row defines a "Value" column at index 5 (NAME,YEAR,MONTH,
    # DAY,PERIOD,Value,…).
    for row in rows[1:]:
        if not row or not row[0]:
            continue
        name = row[0]
        if name in seen_names:
            continue
        seen_names.add(name)
        try:
            value = float(row[5])
        except (ValueError, IndexError):
            continue
        # Strip "min"/"max" suffix to find the matching turbine prefix.
        lname = name.lower()
        if lname.endswith("min"):
            base = name[:-3]
            fmin, fmax = value, 0.0
        elif lname.endswith("max"):
            base = name[:-3]
            fmin, fmax = 0.0, value
        else:
            base = name
            fmin, fmax = 0.0, 0.0
        # Resolve junction:
        #   1. Try the base directly as a reservoir name (ELTORO).
        #   2. Try the all-caps base as a generator-name prefix that
        #      points at the upstream reservoir (ANTUCO → POLCURA).
        base_upper = base.upper()
        if base in reservoir_names:
            junction: str | None = base
        elif base_upper in reservoir_names:
            junction = base_upper
        else:
            junction = prefix_to_junction.get(base_upper)
        if junction is None or (known_junctions and junction not in known_junctions):
            logger.debug(
                "FlowRight %s: no junction resolves for base %r (skipping bind)",
                name,
                base,
            )
            junction = None
        out.append(
            FlowRightSpec(
                name=f"irrigation_{name}",
                junction_name=junction,
                fmin=fmin,
                fmax=fmax,
                # Soft-cap penalty matching the global irrigation
                # default (see ``feedback_irrigation_design`` memory:
                # ``hydro_fail_cost=10 $/m³``). PLEXOS doesn't ship a
                # per-bound slack cost; this default lets the LP
                # relax irrigation envelopes when truly infeasible
                # rather than crashing the solve.
                fcost=10.0,
            )
        )
    return tuple(out)


def _format_coefficient(value: float, first: bool) -> str:
    """Format a coefficient term prefix (sign + numeric)."""
    if first:
        if value < 0:
            return f"-{abs(value):g} * "
        return f"{value:g} * "
    # Non-leading term: emit ``+`` / ``-`` explicitly.
    if value < 0:
        return f" - {abs(value):g} * "
    return f" + {value:g} * "


def _is_contingency_constraint(
    name: str,  # noqa: ARG001  # reserved for future name-based recognisers
    coefficients: list[float],
    op: str,
    rhs_val: float,
) -> bool:
    """Defensive fallback detector for PLEXOS contingency rows.

    The PRIMARY source for "skip this constraint in the ST run" is
    the PLEXOS ``Include in ST Schedule`` property (read in
    ``extract_user_constraints`` and routed into
    ``include_st_excluded``).  This helper exists ONLY for the
    pathological case where a constraint is implicitly active in
    PLEXOS (no explicit ``Include in ST Schedule = 0/-1``) yet its
    coefficient structure is infeasible-as-hard.

    Recogniser: every coefficient ≤ 0, ``op = ">="``, and
    ``rhs > 0``.  The LHS is bounded above by 0 yet the row demands
    ≥ ``rhs > 0`` — no feasible solution.  A single positive
    coefficient means the LHS can grow without bound and the row is
    satisfiable, so don't mark it inactive.
    """
    return bool(
        op == ">="
        and rhs_val > 0.0
        and coefficients
        and all(c <= 0.0 for c in coefficients)
    )


def _first_value(rows: list) -> float | None:
    """Return the lowest-data_id value from a ``data_for`` result, or None.

    Drops the PLEXOS ``±1E+30`` infinity sentinel — caller would treat
    it as a real numeric bound and stamp it into the LP otherwise.
    """
    if not rows:
        return None
    rows.sort(key=lambda r: r.data_id)
    raw = rows[0].value
    if raw is None:
        return None
    if abs(raw) >= 1.0e20:
        return None
    return raw


def _build_membership_pair_index(
    db: PlexosDb, collection_id: int
) -> dict[int, tuple[int, int]]:
    """Return ``{membership_id: (parent_object_id, child_object_id)}`` for ``collection_id``."""
    return {
        m.membership_id: (m.parent_object_id, m.child_object_id)
        for m in db.memberships
        if m.collection_id == collection_id
    }


def _index_coefficient_rows(
    db: PlexosDb,
    coll_id: int,
    prop_id: int,
    objs: dict[int, PlexosObject],
) -> dict[int, list[tuple[str, float]]]:
    """``{constraint_object_id -> [(parent_name, coefficient), …]}`` for one prop."""
    mid_to_pair = _build_membership_pair_index(db, coll_id)
    per_constr: dict[int, list[tuple[str, float]]] = {}
    for d in db.data_rows:
        pair = mid_to_pair.get(d.membership_id)
        if pair is None or d.property_id != prop_id:
            continue
        parent_oid, constr_oid = pair
        parent_obj = objs.get(parent_oid)
        if parent_obj is None or d.value == 0.0:
            continue
        per_constr.setdefault(constr_oid, []).append((parent_obj.name, d.value))
    return per_constr


def extract_decision_variables(
    db: PlexosDb,
) -> tuple[DecisionVariableSpec, ...]:
    """One :class:`DecisionVariableSpec` per PLEXOS ``Decision Variable``.

    Bounds come from the ``Lower Bound`` / ``Upper Bound`` properties
    on the System→Decision Variables collection; ``Objective Function
    Coefficient`` populates the LP cost. Missing values fall back to
    free / zero respectively.
    """
    objs = db.objects_of_class("Decision Variable")
    if not objs:
        return ()
    out: list[DecisionVariableSpec] = []
    for dv in objs:
        lower = db.static_property(
            "Decision Variable", dv.object_id, "Lower Bound", default=float("nan")
        )
        upper = db.static_property(
            "Decision Variable", dv.object_id, "Upper Bound", default=float("nan")
        )
        cost = db.static_property(
            "Decision Variable", dv.object_id, "Objective Function Coefficient"
        )
        out.append(
            DecisionVariableSpec(
                name=dv.name,
                lower_bound=lower if not math.isnan(lower) else None,
                upper_bound=upper if not math.isnan(upper) else None,
                cost=cost,
            )
        )
    return tuple(out)


def extract_user_constraints(
    db: PlexosDb,
    _bundle: PlexosBundle,
    *,
    emitted_names: dict[str, frozenset[str]] | None = None,
    heat_rate_by_gen: dict[str, float] | None = None,
    pmax_by_gen: dict[str, float] | None = None,
) -> tuple[UserConstraintSpec, ...]:
    """Translate PLEXOS ``Constraint`` objects into gtopt UserConstraints.

    Each PLEXOS Constraint carries (Sense, RHS, optional Penalty Price)
    on its System→Constraints membership, plus per-(child class)
    coefficient memberships in ``Generator → Constraints``,
    ``Line → Constraints``, ``Battery → Constraints``, ``Fuel →
    Constraints`` (offtake — expanded to a sum of generator terms
    weighted by their heat rate). Constraint kinds that map to gtopt
    LP variables — direct (``generator.generation``, ``line.flow``,
    ``battery.charge``/``discharge``), commitment binaries
    (``commitment("uc_<gen>").status``/``startup``/``shutdown``) and
    reserve provision (``reserve_provision("provision_<gen>").uprovision``/
    ``dprovision``) — are emitted with the original coefficient.

    Steps:
      1. Read Sense, RHS, Penalty Price from t_data on the
         System→Constraints membership.
      2. Walk every direct-coefficient kind in ``_DIRECT_COEFFS`` plus
         the special ``Fuel.Offtake Coefficient`` expansion, building
         per-Constraint term lists.
      3. ``emitted_names`` filters out terms that reference PLEXOS
         objects we didn't actually emit (gens without bus, lines
         below capacity, gens dropped from commitment / reserve
         provision, …). Pass ``None`` to skip filtering (unit-test
         convenience).
      4. Drop Constraints whose LHS ends up empty after filtering.
      5. Map Sense → ``<=`` / ``>=`` / ``=`` and emit
         ``LHS <op> RHS`` as the final expression string.
    """
    constraints = db.objects_of_class("Constraint")
    if not constraints:
        return ()
    sys_coll = db.collection_for_named("System", "Constraint", "Constraints")
    if sys_coll is None:
        logger.debug("System→Constraints collection missing; skipping")
        return ()
    prop_sense = db.property_by_name(sys_coll.collection_id, "Sense")
    prop_rhs = db.property_by_name(sys_coll.collection_id, "RHS")
    prop_penalty = db.property_by_name(sys_coll.collection_id, "Penalty Price")
    # PLEXOS authoritative include-flag for the Short-Term Schedule
    # (PRGdia = daily PCP).  When the bundle ships an explicit value
    # of 0 or -1 on this property, the constraint is excluded from
    # the ST run regardless of name or coefficient structure.  Audit
    # of DATOS20260422: 1107/1218 constraints carry an explicit
    # exclude (~91%).  Falling back to name/structure heuristics
    # mis-classifies most of these (e.g. `ANTUCOmin`, `ANGOSTURAmaxramp`,
    # `ANGmax` are operational rows, not contingency rows).
    prop_include_st = db.property_by_name(
        sys_coll.collection_id, "Include in ST Schedule"
    )
    if prop_sense is None or prop_rhs is None:
        logger.debug("Sense / RHS properties absent; skipping user constraints")
        return ()

    sys_mid_by_constr: dict[int, int] = {}
    for m in db.memberships:
        if m.collection_id == sys_coll.collection_id:
            sys_mid_by_constr[m.child_object_id] = m.membership_id

    # Pre-resolve the Include-in-ST-Schedule flag per constraint:
    # {constr_object_id -> bool}.  Missing entries are implicitly
    # active (PLEXOS default for the run stage).
    include_st_excluded: set[int] = set()
    if prop_include_st is not None:
        for constr_oid, constr_mid in sys_mid_by_constr.items():
            rows = db.data_for(constr_mid, prop_include_st)
            if not rows:
                continue
            rows.sort(key=lambda r: r.data_id)
            val = rows[0].value
            # PLEXOS uses {-1, 0} for "exclude", positive (typically 1)
            # for "include".  Sentinel-magnitude values land in
            # exclude territory too — defensive.
            if val is not None and val <= 0.0:
                include_st_excluded.add(constr_oid)
    if include_st_excluded:
        logger.info(
            "PLEXOS Constraint: %d/%d explicitly excluded via "
            '"Include in ST Schedule"; emitting with active=False.',
            len(include_st_excluded),
            len(sys_mid_by_constr),
        )

    objs = db.object_by_id()

    # Direct-coefficient index: one row per (parent_class, gtopt_class,
    # accessor, name_template) tuple with a pre-built lookup
    # ``{constraint_oid -> [(parent_name, coeff)]}``.
    direct_index: list[
        tuple[str, str, str, str, dict[int, list[tuple[str, float]]]]
    ] = []
    for (
        parent_class,
        coll_name,
        prop_name,
        gtopt_class,
        accessor,
        name_tmpl,
    ) in _DIRECT_COEFFS:
        coll = db.collection_for_named(parent_class, "Constraint", coll_name)
        if coll is None:
            continue
        prop_id = db.property_by_name(coll.collection_id, prop_name)
        if prop_id is None:
            continue
        direct_index.append(
            (
                parent_class,
                gtopt_class,
                accessor,
                name_tmpl,
                _index_coefficient_rows(db, coll.collection_id, prop_id, objs),
            )
        )

    # Fuel.Offtake expansion: needs the Fuel→Generator membership and
    # each generator's heat rate. Heat-rate lookup prefers the values
    # the GeneratorSpec extractor already resolved (CSV → t_data
    # fallback chain); falls back to direct t_data when not provided.
    fuel_offtake_index: dict[int, list[tuple[str, float]]] = {}
    fuel_to_gens: dict[str, list[str]] = {}
    gen_heat_rate: dict[str, float] = dict(heat_rate_by_gen or {})
    fuel_coll = db.collection_for_named(
        _FUEL_OFFTAKE[0], "Constraint", _FUEL_OFFTAKE[1]
    )
    if fuel_coll is not None:
        prop_id = db.property_by_name(fuel_coll.collection_id, _FUEL_OFFTAKE[2])
        if prop_id is not None:
            fuel_offtake_index = _index_coefficient_rows(
                db, fuel_coll.collection_id, prop_id, objs
            )
            gen_fuel_coll = db.collection_for_named("Generator", "Fuel", "Fuels")
            if gen_fuel_coll is not None:
                for parent, children in db.parent_to_children(
                    gen_fuel_coll.collection_id
                ).items():
                    gen_obj = objs.get(parent)
                    if gen_obj is None:
                        continue
                    for cid in children:
                        fuel_obj = objs.get(cid)
                        if fuel_obj is None:
                            continue
                        fuel_to_gens.setdefault(fuel_obj.name, []).append(gen_obj.name)
                        if gen_obj.name not in gen_heat_rate:
                            gen_heat_rate[gen_obj.name] = (
                                db.static_property(
                                    "Generator", gen_obj.object_id, "Heat Rate"
                                )
                                or db.static_property(
                                    "Generator", gen_obj.object_id, "Heat Rate Incr"
                                )
                                or db.static_property(
                                    "Generator", gen_obj.object_id, "Heat Rate Base"
                                )
                            )

    # Derived-coefficient index: PLEXOS Capacity Factor / Generation
    # Sent Out / Generation Curtailed / Available Capacity. Each kind
    # has its own coefficient-row index PLUS a ``mode`` tag the LHS
    # builder uses to pick the rewrite.
    derived_index: list[tuple[str, str, str, dict[int, list[tuple[str, float]]]]] = []
    for parent_class, coll_name, prop_name, mode in _DERIVED_COEFFS:
        coll = db.collection_for_named(parent_class, "Constraint", coll_name)
        if coll is None:
            continue
        prop_id = db.property_by_name(coll.collection_id, prop_name)
        if prop_id is None:
            continue
        per_constr = _index_coefficient_rows(db, coll.collection_id, prop_id, objs)
        if per_constr:
            derived_index.append((parent_class, mode, prop_name, per_constr))

    # Reserve.Provision expansion: walk the Reserve→Constraint
    # coefficients and the Reserve→Generator eligibility memberships.
    reserve_provision_index: dict[int, list[tuple[str, float]]] = {}
    reserve_to_providers: dict[str, list[str]] = {}
    reserve_direction: dict[str, str] = {}  # name → "up" or "dn"
    rp_coll = db.collection_for_named(
        _RESERVE_PROVISION_EXPANSION[0],
        "Constraint",
        _RESERVE_PROVISION_EXPANSION[1],
    )
    if rp_coll is not None:
        rp_prop_id = db.property_by_name(
            rp_coll.collection_id, _RESERVE_PROVISION_EXPANSION[2]
        )
        if rp_prop_id is not None:
            reserve_provision_index = _index_coefficient_rows(
                db, rp_coll.collection_id, rp_prop_id, objs
            )
            elig_coll = db.collection_for_named("Reserve", "Generator", "Generators")
            if elig_coll is not None:
                for parent, children in db.parent_to_children(
                    elig_coll.collection_id
                ).items():
                    rsv_obj = objs.get(parent)
                    if rsv_obj is None:
                        continue
                    # Name-suffix convention: ``_LW`` = down-reserve.
                    reserve_direction[rsv_obj.name] = (
                        "dn" if rsv_obj.name.endswith("_LW") else "up"
                    )
                    for cid in children:
                        gen_obj = objs.get(cid)
                        if gen_obj is not None:
                            reserve_to_providers.setdefault(rsv_obj.name, []).append(
                                gen_obj.name
                            )

    out: list[UserConstraintSpec] = []
    unsupported_rhs_shift_warns: set[str] = set()
    for constr in constraints:
        mid = sys_mid_by_constr.get(constr.object_id)
        if mid is None:
            continue
        sense_val = _first_value(db.data_for(mid, prop_sense))
        rhs_val = _first_value(db.data_for(mid, prop_rhs))
        if sense_val is None or rhs_val is None:
            continue
        op = _SENSE_OP.get(sense_val)
        if op is None:
            logger.debug("constraint %s has unknown Sense %s", constr.name, sense_val)
            continue
        penalty_val = (
            _first_value(db.data_for(mid, prop_penalty)) if prop_penalty else None
        )

        # 1. Direct coefficient terms.  Filter at two levels:
        #    a) ``emitted_names[parent_class]`` ensures the PLEXOS
        #       parent (e.g. Generator name) exists in our emitted
        #       case (so the constraint isn't routed at a dropped gen).
        #    b) ``emitted_names[gtopt_class.capitalised]`` ensures the
        #       synthesised gtopt element name (e.g. ``uc_<gen>`` or
        #       ``provision_<gen>``) exists too — generators without a
        #       Commitment / ReserveProvision row would otherwise
        #       dangle the constraint reference.
        terms: list[str] = []
        # Track every coefficient appended so we can classify the
        # constraint as contingency after the build loop (signs +
        # magnitudes are what determines feasibility).
        coefficients: list[float] = []
        for parent_class, gtopt_class, accessor, name_tmpl, per_constr in direct_index:
            allowed_parent = (
                emitted_names.get(parent_class) if emitted_names is not None else None
            )
            gtopt_key = {
                "generator": "Generator",
                "line": "Line",
                "battery": "Battery",
                "waterway": "Waterway",
                "reservoir": "Reservoir",
                "commitment": "Commitment",
                "reserve_provision": "ReserveProvision",
                "decision_variable": "DecisionVariable",
            }.get(gtopt_class)
            allowed_ref = (
                emitted_names.get(gtopt_key)
                if emitted_names is not None and gtopt_key is not None
                else None
            )
            for parent_name, coeff in per_constr.get(constr.object_id, ()):
                if allowed_parent is not None and parent_name not in allowed_parent:
                    continue
                ref_name = name_tmpl.format(name=parent_name)
                if allowed_ref is not None and ref_name not in allowed_ref:
                    continue
                var_ref = f'{gtopt_class}("{ref_name}").{accessor}'
                terms.append(_format_coefficient(coeff, first=not terms) + var_ref)
                coefficients.append(coeff)

        # 2. Fuel.Offtake expansion: a Fuel→Constraint coefficient ``α``
        #    becomes ``α × heat_rate(g) × generator(g).generation`` summed
        #    over every Generator g that consumes that Fuel.
        allowed_gens = (
            emitted_names.get("Generator") if emitted_names is not None else None
        )
        for fuel_name, alpha in fuel_offtake_index.get(constr.object_id, ()):
            for gen_name in fuel_to_gens.get(fuel_name, ()):
                if allowed_gens is not None and gen_name not in allowed_gens:
                    continue
                hr = gen_heat_rate.get(gen_name, 0.0)
                if hr == 0.0:
                    continue
                coeff = alpha * hr
                var_ref = f'generator("{gen_name}").generation'
                terms.append(_format_coefficient(coeff, first=not terms) + var_ref)
                coefficients.append(coeff)

        # 2b. Reserve.Provision expansion: α × Σ_g reserve_provision(
        #     "provision_<g>").<up|dn> over generators eligible for
        #     that Reserve. Direction picked from name-suffix
        #     convention (``_LW`` → down, otherwise up).
        allowed_rp = (
            emitted_names.get("ReserveProvision") if emitted_names is not None else None
        )
        for rsv_name, alpha in reserve_provision_index.get(constr.object_id, ()):
            direction = reserve_direction.get(rsv_name, "up")
            for gen_name in reserve_to_providers.get(rsv_name, ()):
                if allowed_gens is not None and gen_name not in allowed_gens:
                    continue
                ref_name = f"provision_{gen_name}"
                if allowed_rp is not None and ref_name not in allowed_rp:
                    continue
                var_ref = f'reserve_provision("{ref_name}").{direction}'
                terms.append(_format_coefficient(alpha, first=not terms) + var_ref)
                coefficients.append(alpha)

        # 3. Derived-coefficient kinds (Capacity Factor / Generation
        #    Sent Out / Generation Curtailed / Available Capacity).
        #    These are PLEXOS-side expressions, not standalone LP vars
        #    — translate to a Generation Coefficient with appropriate
        #    rescaling; flag the RHS-shift kinds since UserConstraint
        #    can't express a per-block RHS yet.
        gen_pmax_by_name = pmax_by_gen or {}
        constraint_has_unsupported = False
        for parent_class, mode, prop_name, per_constr in derived_index:
            allowed_parent = (
                emitted_names.get(parent_class) if emitted_names is not None else None
            )
            for parent_name, alpha in per_constr.get(constr.object_id, ()):
                if allowed_parent is not None and parent_name not in allowed_parent:
                    continue
                if mode == "sent_out":
                    # α × (gen − aux) ≈ α × gen   (aux_use ≈ 0 in PCP)
                    coeff = alpha
                elif mode == "per_capacity":
                    # α × gen / (pmax × Δt)  — Δt absorbed per-block;
                    # drop the term when pmax is zero.
                    pmax = gen_pmax_by_name.get(parent_name, 0.0)
                    if pmax <= 0.0:
                        continue
                    coeff = alpha / pmax
                else:
                    # unsupported_rhs_shift — log once per (constraint, kind)
                    key = f"{constr.name}::{prop_name}"
                    if key not in unsupported_rhs_shift_warns:
                        unsupported_rhs_shift_warns.add(key)
                        logger.warning(
                            "constraint %s drops %s on %s — RHS shift not "
                            "expressible in scalar-RHS UserConstraint",
                            constr.name,
                            prop_name,
                            parent_name,
                        )
                    constraint_has_unsupported = True
                    continue
                var_ref = f'generator("{parent_name}").generation'
                terms.append(_format_coefficient(coeff, first=not terms) + var_ref)
                coefficients.append(coeff)

        if not terms:
            continue
        # If ANY term was unsupported, skip the entire constraint
        # rather than emit a partial form: a constraint missing key
        # terms is no longer the constraint PLEXOS specified, and
        # CEN PCP empirically produces infeasible LPs when the
        # partial form survives (e.g. `ralco_u1_ctf_lw_constraint`
        # demanded a reserve floor that only the dropped Units term
        # could satisfy).  Emit a single info-level summary at the
        # top-level extractor instead.
        if constraint_has_unsupported:
            logger.warning(
                "constraint %s skipped — partial form left after "
                "dropping unsupported PLEXOS coefficient(s); see prior "
                "WARNs for the specific term(s)",
                constr.name,
            )
            continue
        expression = "".join(terms) + f" {op} {rhs_val:g}"
        # PLEXOS-authoritative activation flag.  Two recognisers:
        #   (a) ``Include in ST Schedule`` ∈ {-1, 0} ⇒ PLEXOS itself
        #       excludes this constraint from the daily PCP run.
        #       This is the authoritative source — 91% of CEN PCP
        #       constraints carry this flag.
        #   (b) Structural fallback: all coefficients ≤ 0, op = ">=",
        #       rhs > 0.  Infeasible-as-hard — catches contingency
        #       rows the bundle didn't tag with the include flag.
        #       Should be rare on a healthy PLEXOS export.
        is_excluded_by_plexos = constr.object_id in include_st_excluded
        is_structurally_infeasible = _is_contingency_constraint(
            constr.name, coefficients, op, rhs_val
        )
        is_inactive = is_excluded_by_plexos or is_structurally_infeasible
        if is_excluded_by_plexos:
            logger.debug(
                "constraint %s excluded from ST run by PLEXOS "
                "(Include in ST Schedule ≤ 0); emitting active=False.",
                constr.name,
            )
        elif is_structurally_infeasible:
            logger.info(
                "constraint %s structurally infeasible-as-hard "
                "(all coefficients ≤ 0, GE sense, positive RHS); "
                "emitting active=False as a defensive fallback "
                "(PLEXOS didn't tag it with Include in ST Schedule).",
                constr.name,
            )
        out.append(
            UserConstraintSpec(
                name=constr.name,
                expression=expression,
                penalty=penalty_val if penalty_val and penalty_val > 0 else 0.0,
                active=False if is_inactive else None,
            )
        )
    return tuple(out)


def extract_case(bundle: PlexosBundle) -> PlexosCase:
    """Run every extractor and return the assembled :class:`PlexosCase`.

    This is the single entry-point the writer should consume; the
    individual ``extract_*`` functions are exported for unit-test
    targeting only.
    """
    db = load_xml(bundle.xml_path)
    reservoirs = extract_reservoirs(db, bundle)
    junctions = extract_junctions(reservoirs)
    known_junction_names = frozenset(j.name for j in junctions)
    reserves = extract_reserves(db, bundle)
    generators = extract_generators(db, bundle)
    fuels = extract_fuels(db, bundle)
    turbines = extract_turbines(db, bundle)
    # PLEXOS Region.VoLL → demand_fail_cost.  Take the max across
    # regions (conservative; CEN PCP has identical 467.19 on both).
    bundle_spec = extract_bundle_spec(bundle)
    voll_values = []
    for region in db.objects_of_class("Region"):
        v = db.static_property("Region", region.object_id, "VoLL")
        if v and v > 0.0:
            voll_values.append(v)
    if voll_values:
        # Replace the default 1000.0 with the PLEXOS value (take max).
        # gtopt has one global demand_fail_cost — when regions disagree,
        # the max wins.  Surface that with a WARN so a multi-region case
        # with diverging VoLLs doesn't silently round its cheapest
        # regions up.  Future: per-Bus / per-Region demand_fail_cost
        # would let us honour the full PLEXOS shape.
        chosen = max(voll_values)
        if len(set(voll_values)) > 1:
            logger.warning(
                "PLEXOS Regions ship %d distinct VoLL values "
                "(min=%.2f, max=%.2f) — gtopt has one global "
                "`demand_fail_cost`; using the max (%.2f).  "
                "Regions with a lower VoLL will not see their "
                "lower curtailment price.",
                len(set(voll_values)),
                min(voll_values),
                max(voll_values),
                chosen,
            )
        bundle_spec = BundleSpec(
            bundle_date=bundle_spec.bundle_date,
            step_count=bundle_spec.step_count,
            step_type=bundle_spec.step_type,
            day_beginning=bundle_spec.day_beginning,
            currency=bundle_spec.currency,
            bundle_name=bundle_spec.bundle_name,
            demand_fail_cost=chosen,
        )
    case = PlexosCase(
        bundle=bundle_spec,
        nodes=extract_nodes(db),
        fuels=fuels,
        generators=generators,
        lines=extract_lines(db, bundle),
        demands=extract_demands(db, bundle),
        batteries=extract_batteries(db, bundle),
        reservoirs=reservoirs,
        waterways=extract_waterways(db),
        junctions=junctions,
        turbines=turbines,
        flows=extract_flows(db, bundle, known_junction_names),
        reserves=reserves,
        reserve_provisions=extract_reserve_provisions(
            reserves,
            generators,
            db=db,
            committed_gens=frozenset(
                c.generator_name
                for c in extract_commitments(db, bundle, generators, fuels)
            ),
        ),
        commitments=extract_commitments(db, bundle, generators, fuels),
        flow_rights=extract_flow_rights(bundle, turbines, known_junction_names),
        decision_variables=extract_decision_variables(db),
    )

    # Only allow constraint references to generators whose pmax is
    # active in every block of the horizon. PLEXOS Constraint
    # expressions are LHS-scoped to "all blocks", and gtopt only
    # registers a ``GenerationName`` variable at blocks where pmax > 0
    # — solar / wind gens with zero-block hours would dangle the
    # reference at those blocks and crash LP assembly. Constraints
    # that lose every reference get dropped (their LHS becomes empty).
    def _gen_always_active(g: GeneratorSpec) -> bool:
        if g.pmax <= 0.0:
            return False
        if not g.pmax_profile:
            return True
        return all(p > 0.0 for p in g.pmax_profile)

    emitted_names: dict[str, frozenset[str]] = {
        "Generator": frozenset(
            g.name for g in case.generators if _gen_always_active(g)
        ),
        "Line": frozenset(line.name for line in case.lines),
        "Battery": frozenset(b.name for b in case.batteries),
        # gtopt element names for synthesised commitment / reserve
        # provision rows. The constraint writer routes coefficients via
        # the templated names (``uc_<gen>`` / ``provision_<gen>``) and
        # needs to drop references for generators that didn't emit a
        # matching Commitment / ReserveProvision row.
        "Commitment": frozenset(f"uc_{c.generator_name}" for c in case.commitments),
        "ReserveProvision": frozenset(
            f"provision_{p.generator_name}" for p in case.reserve_provisions
        ),
        # DecisionVariable: PLEXOS DV.Value coefficient references the
        # DV by name directly (no template indirection).
        "Decision Variable": frozenset(d.name for d in case.decision_variables),
        "DecisionVariable": frozenset(d.name for d in case.decision_variables),
        # Waterway / Reservoir / Storage allow-lists for the
        # waterway.flow, reservoir.efin (PLEXOS Storage.End Volume),
        # and battery.energy coefficient kinds.
        "Waterway": frozenset(w.name for w in case.waterways),
        "Reservoir": frozenset(r.name for r in case.reservoirs),
        "Storage": frozenset(r.name for r in case.reservoirs),
    }
    heat_rate_by_gen = {g.name: g.heat_rate for g in case.generators if g.heat_rate}
    pmax_by_gen_for_uc = {g.name: g.pmax for g in case.generators if g.pmax > 0}
    case = dataclasses.replace(
        case,
        user_constraints=extract_user_constraints(
            db,
            bundle,
            emitted_names=emitted_names,
            heat_rate_by_gen=heat_rate_by_gen,
            pmax_by_gen=pmax_by_gen_for_uc,
        ),
    )
    logger.info(
        "parsed bundle %s: nodes=%d fuels=%d gens=%d lines=%d demands=%d "
        "batteries=%d reservoirs=%d waterways=%d turbines=%d flows=%d "
        "reserves=%d provisions=%d commitments=%d flow_rights=%d "
        "user_constraints=%d",
        bundle.source.name,
        len(case.nodes),
        len(case.fuels),
        len(case.generators),
        len(case.lines),
        len(case.demands),
        len(case.batteries),
        len(case.reservoirs),
        len(case.waterways),
        len(case.turbines),
        len(case.flows),
        len(case.reserves),
        len(case.reserve_provisions),
        len(case.commitments),
        len(case.flow_rights),
        len(case.user_constraints),
    )
    return case


__all__ = [
    "extract_batteries",
    "extract_bundle_spec",
    "extract_case",
    "extract_commitments",
    "extract_decision_variables",
    "extract_demands",
    "extract_flow_rights",
    "extract_flows",
    "extract_fuels",
    "extract_generators",
    "extract_junctions",
    "extract_lines",
    "extract_nodes",
    "extract_reserve_provisions",
    "extract_reserves",
    "extract_reservoirs",
    "extract_turbines",
    "extract_user_constraints",
    "extract_waterways",
]
