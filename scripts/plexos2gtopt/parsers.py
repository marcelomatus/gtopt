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
#   - ``"curtailed"``: PLEXOS ``Generation Curtailed`` =
#     ``(Capacity × cf) − generation``.  LHS gets ``-α × gen``;
#     RHS gets the per-block shift ``+α × Capacity × cf[block]``
#     where ``Capacity × cf[block]`` reads from ``Gen_Rating.csv``
#     (the per-block ``pmax_profile``).  Requires the gtopt
#     ``UserConstraint.rhs`` TB-schedule feature.
#   - ``"available_capacity"``: PLEXOS ``Available Capacity`` =
#     ``Capacity × cf``.  Pure RHS contribution (no LHS term);
#     RHS gets ``+α × Capacity × cf[block]`` per block.  Same
#     TB-schedule requirement as ``curtailed``.
#   - ``"unsupported_rhs_shift"``: cross-block (ramp) and
#     battery-reserve-units coefficients that gtopt's UserConstraint
#     cannot express today — neither the LHS (gtopt has no
#     ``generator.ramp_up`` accessor) nor any per-block
#     reformulation is available.  Dropped with a WARNING.
_DERIVED_COEFFS: tuple[tuple[str, str, str, str], ...] = (
    ("Generator", "Constraints", "Generation Sent Out Coefficient", "sent_out"),
    ("Generator", "Constraints", "Capacity Factor Coefficient", "per_capacity"),
    (
        "Generator",
        "Constraints",
        "Generation Curtailed Coefficient",
        "curtailed",
    ),
    (
        "Generator",
        "Constraints",
        "Available Capacity Coefficient",
        "available_capacity",
    ),
    # Ramp Up/Down Coefficient references the inter-block ramp delta
    # ``α × (gen(t) − gen(t−1))``.  gtopt now exposes the prior-block
    # generation via the ``generator("X").generation_prev`` AMPL
    # accessor (resolved by ``element_column_resolver.cpp`` against
    # the chronological block sequence on the active StageLP;
    # first-block boundary case treats the prior dispatch as 0 /
    # cold start).  Emit BOTH terms in mode ``ramp_delta`` below:
    # ``+α × generation`` and ``−α × generation_prev``.
    (
        "Generator",
        "Constraints",
        "Ramp Up Coefficient",
        "ramp_delta",
    ),
    (
        "Generator",
        "Constraints",
        "Ramp Down Coefficient",
        "ramp_delta",
    ),
    # Battery Reserve Units Coefficient: gtopt has no
    # ``battery_commitment("X").status`` AMPL accessor at the
    # UserConstraint expression level (the LP backend manages
    # u_charge / u_discharge internally on the Battery LP).
    # Pragmatic fix: forward the term to the **synthetic
    # ``<battery>_gen`` Generator** that gtopt's
    # ``system.cpp::expand_batteries`` auto-creates as the
    # battery's discharge path — that generator carries a
    # standard ``Commitment`` object (uid ``uc_<battery>_gen``)
    # and exposes ``.status``, ``.startup``, ``.shutdown`` via
    # the existing ``commitment("X").status`` AMPL accessor.
    # Mode ``forward_to_battery_gen_commit`` emits
    # ``α × commitment("uc_<battery_name>_gen").status``.
    (
        "Battery",
        "Constraints",
        "Reserve Units Coefficient",
        "forward_to_battery_gen_commit",
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
        n_days=bundle.n_days,
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


#: Regex peeling the trailing PLEXOS band suffix off a fuel name so
#: sibling-band detection groups e.g. ``Gas_Kelar_GN_A``,
#: ``Gas_Kelar_GN_B``, … under the same base ``Gas_Kelar_GN``.
#: Recognised suffixes (in order tried): ``_INF`` / ``_GNL_INF`` /
#: ``_GN_INF`` (PLEXOS "unlimited" sentinel), single-letter band IDs
#: (``_A``…``_F``, ``_X``).
_FUEL_BAND_SUFFIX_RE = re.compile(
    r"_(?:GN_INF|GNL_INF|INF|[A-FX])$",
)


def _patch_uncapped_zero_fuel_bands(prices: dict[str, list[float]]) -> None:
    """In-place patch: zero-priced bands that have a priced sibling get
    promoted to the MAX of the group's priced bands.

    PLEXOS-CEN convention: a fuel "base" (e.g. ``Gas_Kelar_GN``) ships
    multiple bands (``_A``, ``_B``, ``_C``, ``_D``) — sometimes only
    one band carries a price and the others are explicit 0.  The
    zero-priced bands ARE constrained on the PLEXOS side by
    ``FueMaxOffWeek_<fuel>`` Constraint objects that live only in the
    solution ``.accdb``, so plexos2gtopt's input-only parsing can't
    see them.  Without that cap, the gtopt LP exploits the zero band
    as free fuel.

    The heuristic here turns each unconstrained zero band into a
    worst-case-priced band (the max of its priced siblings).  This
    over-estimates cost on those bands but eliminates the
    free-arbitrage shortcut that would otherwise dwarf PLEXOS's
    operational dispatch cost on CEN PCP.

    Patches only groups that have a MIX of zero and non-zero bands.
    Wholly-zero groups (biomass / biogas / geothermal / ERNC) are
    left alone — their explicit zero is the correct economic signal.
    """
    by_base: dict[str, list[str]] = {}
    for name in prices:
        base = _FUEL_BAND_SUFFIX_RE.sub("", name)
        by_base.setdefault(base, []).append(name)

    patched: list[tuple[str, float]] = []
    for base, names in by_base.items():
        if len(names) < 2:
            continue
        non_zero = [n for n in names if prices[n][0] > 0.0]
        zero = [n for n in names if prices[n][0] == 0.0]
        if not non_zero or not zero:
            continue
        cap_price = max(prices[n][0] for n in non_zero)
        for name in zero:
            old_vals = prices[name]
            prices[name] = [cap_price] * len(old_vals)
            patched.append((name, cap_price))

    if patched:
        # One INFO line per affected fuel keeps the patch auditable
        # without flooding the log on bundles with many bands.
        for name, cap in sorted(patched):
            logger.info(
                "patched uncapped zero-priced fuel %s → %.4f "
                "(worst-case sibling price; PLEXOS caps live in .accdb "
                "FueMaxOffWeek_* and aren't visible to input-only "
                "parsing)",
                name,
                cap,
            )


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

    # Patch "uncapped zero-priced band" fuels.  PLEXOS-CEN ships
    # multi-band fuel contracts (e.g. ``Gas_Kelar_GN_A/B/C/D``) where
    # only band A carries a price and B/C/D are explicit 0.0 in
    # ``Fuel_Price.csv``.  PLEXOS itself doesn't dispatch unlimited
    # zero-price fuel because it carries weekly-offtake caps as
    # ``FueMaxOffWeek_<fuel>`` Constraint objects — but those
    # Constraints live only in the **solution** ``.accdb`` (not the
    # input ``DBSEN_PRGDIARIO.xml``), so plexos2gtopt can't see them
    # by parsing the input alone.  Without the cap, the gtopt LP
    # discovers a "free fuel" arbitrage and dispatches the
    # corresponding generator-variant (``KELAR-TG1+TG2+TV_GN_B``,
    # ``MEJILLONES_3-TG+TV_GN_C``, ``COCHRANE_*_GN_D``, …) for
    # ~150 GWh of zero-cost thermal generation, undercutting PLEXOS
    # by ~$15M on the CEN PCP daily-week.  As a band-aid until proper
    # ``FueMaxOff*`` constraint extraction is wired up (would need
    # the .accdb at conversion time), promote each zero-priced band
    # that has a priced sibling to the MAX of its priced siblings
    # in the same fuel "base group" (same prefix sans trailing
    # ``_<LETTER>`` band suffix).  This converts the band from a
    # free-arbitrage option to a worst-case backup that the LP only
    # uses when nothing cheaper is available — closer to PLEXOS's
    # cap-constrained dispatch.  Confirmed-zero-price fuels
    # (biomass / biogas / ERNC where the WHOLE group is zero) are
    # untouched.
    if prices:
        _patch_uncapped_zero_fuel_bands(prices)

    membership_rates = _extract_fuel_co2_membership_rates(db)
    out: list[FuelSpec] = []
    for fuel in db.objects_of_class("Fuel"):
        # CSV-present (even explicit 0) takes precedence over t_data
        # fallback.  ``fuel_price = 0`` is a legitimate value for
        # biomass / biogas / geothermal / ERNC fuels in CEN PCP
        # (~25 fuels in DATOS20260422 ship explicit Price = 0).
        # Using ``dict.get(name, [0.0])[0]`` followed by
        # ``if price == 0.0`` would silently overwrite those with
        # the System→Fuels static-property default (often non-zero).
        if fuel.name in prices:
            price = prices[fuel.name][0]
        else:
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
        read_long(bundle.csv("Gen_Rating.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_Rating.csv")
        else {}
    )
    # ────────────────────────────────────────────────────────────────
    # Optional: override pmax_profile with PLEXOS-solved commitment
    # ────────────────────────────────────────────────────────────────
    # When ``GTOPT_USE_PLEXOS_COMMIT=1`` (or the
    # ``--use-plexos-commit`` CLI flag), pin per-period pmax to
    # PLEXOS's solved ``Units Generating`` (pid 7) from the solution
    # .accdb cache.  This forces the LP to follow PLEXOS's MIP
    # commitment decisions exactly — useful when the LP-relax
    # over-dispatches by fractional commitment of units PLEXOS
    # decided to leave OFF (e.g. RUCUE, QUILLECO, LAJA_I on the
    # CEN PCP weekly bundle).  Mechanics: for each generator and
    # each period, if PLEXOS committed 0 units, set pmax=0 (forces
    # gen=0); otherwise scale pmax by the fraction of units PLEXOS
    # committed (full pmax × units_on / max_units).
    import os

    use_plexos_commit = os.environ.get("GTOPT_USE_PLEXOS_COMMIT", "0").lower() in (
        "1",
        "true",
        "yes",
    )
    plexos_commit: dict[str, dict[int, float]] = {}
    if (
        use_plexos_commit
        and bundle.accdb_cache_dir is not None
        and bundle.accdb_cache_dir.is_dir()
    ):
        from .plexos_block_layout import extract_generator_commit_per_period

        commit_data = extract_generator_commit_per_period(bundle.accdb_cache_dir)
        if commit_data:
            # Restrict the override to HYDRO TURBINE generators only.
            # Applying it to ALL 1700+ generators breaks reserve-provision
            # LP build (pmax=0 hours drop generator columns that reserve
            # rows reference → flat_map::at exception).  Hydro turbine
            # units are the ones with cascade-compounding overshoot;
            # thermal and renewables have their own commit logic that
            # should not be curve-fitted to PLEXOS solution.
            head_coll = db.collection_for_named("Generator", "Storage", "Head Storage")
            turbine_names: set[str] = set()
            if head_coll is not None:
                turbine_oids = {
                    m.parent_object_id
                    for m in db.memberships_of(head_coll.collection_id)
                }
                turbine_names = {
                    gen.name
                    for gen in db.objects_of_class("Generator")
                    if gen.object_id in turbine_oids
                }
            plexos_commit = {k: v for k, v in commit_data.items() if k in turbine_names}
            logger.info(
                "extract_generators: GTOPT_USE_PLEXOS_COMMIT=1 — overriding "
                "pmax_profile with PLEXOS-solved Units Generating for %d "
                "hydro turbine generators (skipped %d non-turbine)",
                len(plexos_commit),
                len(commit_data) - len(plexos_commit),
            )
    # ────────────────────────────────────────────────────────────────
    # Optional: hard-cap pmax to PLEXOS-solved per-period Generation
    # ────────────────────────────────────────────────────────────────
    # When ``GTOPT_USE_PLEXOS_GEN_CAP=1`` (or the ``--use-plexos-gen-cap``
    # CLI flag), pin per-period pmax to PLEXOS's published ``Generation``
    # (pid 2) from the solution .accdb cache.  This is the TIGHTEST
    # possible curve-fit: every block, the LP can dispatch at most what
    # PLEXOS dispatched.  Useful for validating that the LP would match
    # PLEXOS dispatch exactly if every per-period cap were correctly
    # propagated, and for diagnosing whether the +71 % hydro overshoot
    # is purely from missing per-block caps vs structural differences.
    #
    # Same restriction as ``--use-plexos-commit``: applied only to
    # HYDRO TURBINE generators (those with a Head Storage membership)
    # to avoid the ReserveProvisionLP::flat_map::at defect that fires
    # when thermal generator gen-cols get elided at zero hours.
    use_plexos_gen_cap = os.environ.get("GTOPT_USE_PLEXOS_GEN_CAP", "0").lower() in (
        "1",
        "true",
        "yes",
    )
    plexos_gen_cap: dict[str, dict[int, float]] = {}
    if (
        use_plexos_gen_cap
        and bundle.accdb_cache_dir is not None
        and bundle.accdb_cache_dir.is_dir()
    ):
        from .plexos_block_layout import extract_generator_generation_per_period

        gen_data = extract_generator_generation_per_period(bundle.accdb_cache_dir)
        if gen_data:
            # Broaden the cap from "turbine-mapped" (77) to ALL hydro
            # generators (191 on CEN PCP) by PLEXOS category lookup.
            # Includes the 'zombie' hydros (PEHUENCHE_U1, COLBUN_U1,
            # PANGUE_U1, ANTUCO_U2, ALFALFAL_2, ...) that exist in the
            # JSON but lack a Head Storage membership — without the
            # broader cap they dispatch at nameplate freely and account
            # for ~250 GWh of the +71 % hydro overshoot (verified
            # 2026-05-22).  PLEXOS classification uses the
            # ``Hydro Gen Group A/B/C`` categories in t_category.
            from .plexos_block_layout import _read_cached_csv

            hydro_names: set[str] = set()
            cat_data = _read_cached_csv(bundle.accdb_cache_dir, "t_category")
            obj_data = _read_cached_csv(bundle.accdb_cache_dir, "t_object")
            cls_data = _read_cached_csv(bundle.accdb_cache_dir, "t_class")
            if cat_data and obj_data and cls_data:
                cat_name_map = {c["category_id"]: c["name"] for c in cat_data}
                gen_cls = next(
                    (c["class_id"] for c in cls_data if c["name"] == "Generator"),
                    None,
                )
                if gen_cls is not None:
                    for o in obj_data:
                        if o.get("class_id") != gen_cls:
                            continue
                        cn = cat_name_map.get(o.get("category_id", ""), "")
                        if "hidr" in cn.lower() or "hydro" in cn.lower():
                            hydro_names.add(o["name"])
            plexos_gen_cap = {k: v for k, v in gen_data.items() if k in hydro_names}
            logger.info(
                "extract_generators: GTOPT_USE_PLEXOS_GEN_CAP=1 — capping "
                "pmax_profile to PLEXOS-solved Generation per period for %d "
                "hydro generators (PLEXOS category 'Hydro*')",
                len(plexos_gen_cap),
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
        # PLEXOS-commit override: scale pmax_profile by per-period
        # Units Generating.  When PLEXOS solved with 0 units committed
        # at period p, force pmax_profile[p-1] = 0 (gen MUST be off).
        # Otherwise scale by (units_committed / max_units_observed) so
        # multi-unit generators (e.g. RUCUE with 2 units) get the
        # right partial-fleet cap when PLEXOS committed one of two.
        if plexos_commit and gen.name in plexos_commit:
            commit = plexos_commit[gen.name]
            max_units = max(commit.values()) if commit else 0.0
            if max_units > 0 and profile:
                # period_id in cache is 1-indexed; profile is 0-indexed
                new_profile = list(profile)
                for i in range(len(new_profile)):
                    units_p = commit.get(i + 1, max_units)
                    new_profile[i] = new_profile[i] * (units_p / max_units)
                profile = tuple(new_profile)
        # PLEXOS-gen-cap override: hard-cap pmax_profile to PLEXOS's
        # solved per-period Generation.  Curve-fits dispatch envelope
        # to PLEXOS exactly.  Synthesise a profile from scratch when
        # Gen_Rating didn't ship one (gtopt then emits per-block
        # matrix derived purely from PLEXOS solution).
        if plexos_gen_cap and gen.name in plexos_gen_cap:
            gen_cap = plexos_gen_cap[gen.name]
            if gen_cap:
                horizon_h = max(gen_cap.keys())
                if not profile or len(profile) < horizon_h:
                    # Bootstrap a profile of horizon_h hours filled with
                    # the static max so the override below has something
                    # to cap.  Use Gen_Rating max if available, else
                    # PLEXOS Generation peak as the upper envelope.
                    # The 1e-3 epsilon previously needed to keep gen
                    # columns alive (ReserveProvisionLP::flat_map::at
                    # defect on zero-pmax blocks) is no longer required:
                    # source/reserve_provision_lp.cpp now uses the
                    # tolerant ``lookup_generation_cols`` (patched
                    # 2026-05-22), so real zeros are safe.
                    seed = max(profile) if profile else max(gen_cap.values())
                    profile = tuple([seed] * horizon_h)
                new_profile = list(profile)
                for i in range(len(new_profile)):
                    cap_p = gen_cap.get(i + 1)
                    if cap_p is not None:
                        new_profile[i] = min(new_profile[i], cap_p)
                profile = tuple(new_profile)
        # Use the max of the per-hour profile as the static pmax (gtopt
        # will multiply by pmax_factor when we emit the profile).
        # Skip the Max Capacity fallback when an EXPLICIT cap profile is
        # active (``--use-plexos-gen-cap``): we WANT pmax=0 if PLEXOS
        # dispatched the unit at 0 every period.  Without this guard
        # the fallback overrides the cap and the LP free-runs at full
        # nameplate, defeating the override.
        pmax = max(profile) if profile else 0.0
        if pmax == 0.0 and not (plexos_gen_cap and gen.name in plexos_gen_cap):
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
        # CSV-present (even explicit 0) takes precedence over t_data
        # fallback — see fuel-price comment above.  CEN PCP ships
        # explicit heat_rate=0 for ~6 renewable generators (geothermal,
        # solar thermal) where fuel consumption truly is zero.
        if gen.name in heat_rate_csv:
            heat_rate = heat_rate_csv[gen.name][0]
        else:
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
        # VOM: CSV-present (even 0) takes precedence; only fall back to
        # t_data when the generator is absent from the CSV.
        if gen.name in vom_csv:
            vom = vom_csv[gen.name][0]
        else:
            vom = db.static_property("Generator", gen.object_id, "VO&M Charge")
        # Fuel transport charge ($/MWh): PLEXOS keys each row by the
        # ``<generator_name><fuel_name>`` concatenation (one row per gen
        # × fuel attachment).  Use the primary-fuel match — same fuel
        # the writer picks for gcost (``fuel_names[0]``).  Fall back to
        # exact gen.name (XML-only schemas with no fuel suffix).
        #
        # CSV-present (even explicit 0) honoured: the bundle ships a
        # handful of explicit-0 transport rows (NUEVA_RENCA gas grades
        # with no transport surcharge); a ``== 0.0 → fallback`` guard
        # would let an unrelated gen.name-only row overwrite the
        # intentional zero.
        fuel_transport = 0.0
        fuel_transport_found = False
        if fuel_transport_csv:
            gen_fuel_names = fuel_map.get(gen.object_id, ())
            for fname in gen_fuel_names:
                key = gen.name + fname
                if key in fuel_transport_csv:
                    fuel_transport = fuel_transport_csv[key][0]
                    fuel_transport_found = True
                    break
            if not fuel_transport_found and gen.name in fuel_transport_csv:
                fuel_transport = fuel_transport_csv[gen.name][0]
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

    # Use the full n_days × 24 horizon with PLEXOS's carry-forward
    # semantics — DLR (Dynamic Line Rating) corridors like
    # LoAguirre500->Polpaico500 ship sparse rows (period 1, 7, 24)
    # where period N's value applies through period N+1's row.
    # Without ``fill_forward`` and ``n_days``, the legacy
    # ``[period-1 only]`` slice silently picks the lowest (overnight)
    # rating and bottlenecks the 500-kV interconnections to ~half
    # their daytime capacity.
    max_rating: dict[str, list[float]] = (
        read_long(
            bundle.csv("Lin_MaxRating.csv"),
            n_days=bundle.n_days,
            fill_forward=True,
        )
        if bundle.has("Lin_MaxRating.csv")
        else {}
    )
    min_rating: dict[str, list[float]] = (
        read_long(
            bundle.csv("Lin_MinRating.csv"),
            n_days=bundle.n_days,
            fill_forward=True,
        )
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
        # Scalar tmax/tmin: use MAX of the per-hour profile so the LP
        # gets the peak capacity (binding only when the profile is
        # variant; if invariant, max == period-1).  We still pass the
        # full profile through to the writer, which honours it block-
        # by-block when it actually varies.
        tmax = max(tmax_series) if tmax_series else 0.0
        tmin = min(tmin_series) if tmin_series else 0.0
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
        # PLEXOS Resistance (pid 1888): per-unit on the system MVA
        # base.  Drives gtopt's piecewise loss model
        # `P_loss = R · f² / V²` — emit verbatim, the writer turns
        # on the piecewise mode with a small fixed number of
        # segments to mirror PLEXOS's default loss linearisation.
        resistance = db.static_property("Line", line.object_id, "Resistance")
        # PLEXOS Max Rating (pid 1882) / Min Rating (pid 1883) —
        # short-term / emergency limits, typically 1.5-2× Max Flow.
        # Carry through to LineSpec so the writer can pair with
        # Max Flow as (soft, hard) thresholds.
        #
        # Sentinel filter: PLEXOS-CEN ships sentinel "no-constraint"
        # values on a handful of lines (e.g. Antofag110->Desalant110
        # at 110 kV has Max Rating = 1000 MW = 17.5× Max Flow,
        # physically impossible).  Realistic emergency-rating
        # uplifts on Chilean transmission stay below ~2-3× for
        # lines; transformers can legitimately hit 7-8× via thermal
        # short-term overload (Jadresic500->Jadresic220 at 7.5× is
        # plausible — oil-cooled HV transformers carry significant
        # overload capacity for minutes-to-hours).  Reject any
        # Max Rating > 8× Max Flow as a sentinel; keep the rest.
        max_rating_static = (
            db.static_property("Line", line.object_id, "Max Rating") or 0.0
        )
        min_rating_static = (
            db.static_property("Line", line.object_id, "Min Rating") or 0.0
        )
        if tmax > 0.0 and max_rating_static > 8.0 * tmax:
            logger.debug(
                "Line %s: Max Rating %.0f > 8x Max Flow %.0f — "
                "treating as sentinel, ignoring uplift",
                line.name,
                max_rating_static,
                tmax,
            )
            max_rating_static = 0.0
            min_rating_static = 0.0
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
                tmax_ab_profile=tuple(tmax_series),
                tmin_ab_profile=tuple(tmin_series),
                max_rating=float(max_rating_static),
                min_rating=float(min_rating_static),
                units=units,
                reactance=reactance,
                resistance=resistance,
                wheeling_charge=wheeling,
                enforce_limits=enforce_limits,
            )
        )
    return tuple(out)


def _bus_to_region_voll(db: PlexosDb) -> dict[str, float]:
    """Return ``{bus_name -> Region.VoLL}`` ($/MWh of unserved energy).

    Walks the Node→Region membership and reads each Region's ``VoLL``
    static property.  Buses with no Region or whose Region has no VoLL
    are absent from the result; the caller defaults them to the global
    ``demand_fail_cost``.

    Replaces the previous ``max(voll_values)`` collapse that mapped a
    multi-Region system to one global penalty — per-Region routing is
    PLEXOS's native shape (literature audit #3, 2026-05-20).
    """
    objs = db.object_by_id()
    region_voll: dict[str, float] = {}
    for region in db.objects_of_class("Region"):
        v = db.static_property("Region", region.object_id, "VoLL")
        if v and v > 0.0:
            region_voll[region.name] = v

    bus_voll: dict[str, float] = {}
    node_region_coll = db.collection_for_named("Node", "Region", "Region")
    if node_region_coll is None or not region_voll:
        return bus_voll
    for parent, children in db.parent_to_children(
        node_region_coll.collection_id
    ).items():
        if not children or children[0] not in objs:
            continue
        bus = objs.get(parent)
        if bus is None:
            continue
        region_name = objs[children[0]].name
        voll = region_voll.get(region_name)
        if voll is not None:
            bus_voll[bus.name] = voll
    return bus_voll


def extract_demands(db: PlexosDb, bundle: PlexosBundle) -> tuple[DemandSpec, ...]:
    """One :class:`DemandSpec` per non-zero bus load.

    Two sources, in priority order:

    1. ``Nod_Load.csv`` (wide format, per-hour) — the CEN PCP convention.
    2. ``t_data`` ``Load`` property on the System→Nodes collection —
       used by XML-only benchmarks like ``118-Bus.xml``. The value
       there is a single scalar that the writer broadcasts across the
       24-block horizon.

    ``DemandSpec.fcost`` is populated from the Demand's bus → Region
    → VoLL chain.  Each Demand picks up the curtailment penalty of
    its serving Region; buses without a Region (or whose Region has
    no VoLL) leave ``fcost = 0`` and fall back to the global
    ``model_options.demand_fail_cost``.
    """
    bus_voll = _bus_to_region_voll(db)
    n_days = bundle.n_days
    if bundle.has("Nod_Load.csv"):
        load_by_bus = read_wide(bundle.csv("Nod_Load.csv"), n_days=n_days)
        out: list[DemandSpec] = []
        for bus_name, profile in load_by_bus.items():
            out.append(
                DemandSpec(
                    name=f"load_{bus_name}",
                    bus_name=bus_name,
                    lmax_profile=tuple(profile),
                    fcost=bus_voll.get(bus_name, 0.0),
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
                lmax_profile=(load,) * (24 * n_days),
                fcost=bus_voll.get(node.name, 0.0),
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
    skipped_aux = 0
    for batt in db.objects_of_class("Battery"):
        bus_name = bus_map.get(batt.object_id)
        if bus_name is None:
            continue
        # PLEXOS "_AUX" battery modeling artifacts: virtual buffers
        # used by PLEXOS for some reserve / contingency mechanism we
        # don't represent in gtopt.  Audit of DATOS20260422 found 5
        # such batteries (BAT_DEL_DESIERTO_AUX, BAT_TOCOPILLA_AUX,
        # BAT_MANZANO_FV_AUX, BAT_DON_HUMBERTO_FV_AUX,
        # BAT_LA_CABANA_EO_AUX), all with the sentinel values
        # ``Capacity = 99,999 MWh`` and ``Max Power = 1000 MW`` (vs
        # the real CEN BESS units at 100-1300 MWh / 2 MW).  Left in
        # the converted JSON they get freely dispatched (gcost=0) and
        # produced ~30 GWh of fake generation on day 1, pushing the
        # LP objective $1-2M below PLEXOS.  Drop them entirely; if a
        # future bundle needs them, re-enable via a CLI flag.
        if batt.name.endswith("_AUX"):
            skipped_aux += 1
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
        # Drop infeasible charge/discharge minima — PLEXOS-CEN
        # occasionally ships ``Min Charge Level > Max Power`` on
        # small placeholder batteries (e.g. BAT_DEL_DESIERTO:
        # max_power=2, min_charge=2.32; BAT_TOCOPILLA: max_power=2,
        # min_charge=2.5).  The downstream gtopt expander creates a
        # synthetic ``<bat>_dem`` Demand with ``lmax = pmax_charge``,
        # ``lmin = pmin_charge`` — when ``lmin > lmax`` the LP must
        # hit fail for the whole horizon (lmax=2 MW, lmin=2.32 MW
        # → 0.32 MW of phantom unserved demand every block ×
        # 111 blocks × 168 h = ~7.6 GWh "ghost unserved" on the
        # CEN PCP daily bundle).  Set the bad pmin → 0 to mirror
        # the plp2gtopt convention (synthetic battery demand has
        # ``lmax`` only, ``lmin = 0``, no ``fcost``).
        if pmin_charge > max_power > 0.0:
            logger.warning(
                "Battery '%s': Min Charge Level %.2f MW > Max Power "
                "%.2f MW — dropping pmin_charge (matches plp2gtopt: "
                "synthetic <bat>_dem keeps lmax only, lmin = 0).",
                batt.name,
                pmin_charge,
                max_power,
            )
            pmin_charge = 0.0
        if pmin_discharge > max_power > 0.0:
            logger.warning(
                "Battery '%s': Min Discharge Level %.2f MW > Max Power "
                "%.2f MW — dropping pmin_discharge.",
                batt.name,
                pmin_discharge,
                max_power,
            )
            pmin_discharge = 0.0
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
    if skipped_aux:
        logger.info(
            "Dropped %d `_AUX` battery modeling artifact(s) (PLEXOS "
            "virtual reserve buffers with 99,999 MWh / 1000 MW "
            "sentinel ratings).",
            skipped_aux,
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
    # CEN PCP ships Hydro_MaxVolume / Hydro_MinVolume in WIDE format
    # (YEAR, MONTH, DAY, PERIOD, then one column per Storage).  The
    # static ``Max Volume`` / ``Min Volume`` t_data fallback covers
    # most reservoirs but NOT ``L_Maule`` (Lago Maule, ~17,601 hm³)
    # — without reading the WIDE CSV that reservoir is silently
    # downscaled to ``emax = 0``, collapsing its storage column and
    # leaking ~3,000 GWh of dispatchable hydro from the LP.
    # ``Hydro_InitialVolume.csv`` is LONG format (NAME column), so it
    # still uses ``read_long``.
    emax_csv = (
        read_wide(bundle.csv("Hydro_MaxVolume.csv"), n_days=bundle.n_days)
        if bundle.has("Hydro_MaxVolume.csv")
        else {}
    )
    emin_csv = (
        read_wide(bundle.csv("Hydro_MinVolume.csv"), n_days=bundle.n_days)
        if bundle.has("Hydro_MinVolume.csv")
        else {}
    )
    eini_csv = (
        read_long(bundle.csv("Hydro_InitialVolume.csv"))
        if bundle.has("Hydro_InitialVolume.csv")
        else {}
    )
    # PLEXOS-solution End Volume (prop 646 at the LAST horizon period)
    # for every Storage object — used to pin gtopt's ``Reservoir.efin``
    # so the reservoir trajectory matches PLEXOS exactly instead of
    # draining to the loose operational floor.  Returns ``None`` when
    # the bundle has no solution .accdb cache; falls back to the
    # last-day floor from ``Hydro_MinVolume.csv`` in that case.
    solution_efin: dict[str, float] = {}
    if bundle.accdb_cache_dir is not None and bundle.accdb_cache_dir.is_dir():
        from .plexos_block_layout import extract_storage_solution_efin

        sol = extract_storage_solution_efin(bundle.accdb_cache_dir)
        if sol:
            solution_efin = sol
            logger.info(
                "extract_reservoirs: pinned efin to PLEXOS-solution "
                "End Volume for %d storages",
                len(solution_efin),
            )

    out: list[ReservoirSpec] = []
    skipped_lng = 0
    for storage in db.objects_of_class("Storage"):
        name = storage.name
        # CEN PCP misuses PLEXOS Storage to model "infinite LNG fuel
        # supply" via a ``<terminal>_GNL_INF`` naming convention (GNL =
        # Gas Natural Licuado, INF = unbounded).  These are NOT water
        # reservoirs — every volume / inflow / water-value property
        # ships at 0, they have no Waterway / Turbine / Generator
        # memberships referencing them, and they only exist as a
        # PLEXOS bookkeeping artifact for gas import accounting at
        # Mejillones / Quintero LNG terminals.  Emitting them as
        # gtopt Reservoirs creates dead all-zero rows and a paired
        # orphan Junction; drop them at the source so the JSON stays
        # clean.
        if name.endswith("_GNL_INF"):
            skipped_lng += 1
            continue
        emax_series = emax_csv.get(name, [])
        emin_series = emin_csv.get(name, [])
        eini_series = eini_csv.get(name, [])
        # Per-day CSV values: take the MAX (for emin) / MIN (for
        # emax) within each 24-slot day window.  CEN PCP
        # ``Hydro_MinVolume.csv`` ships end-of-day floors via
        # ``PERIOD=24`` rows (e.g. ELTORO has its 12,079 hm³
        # end-of-week floor in slot 167, all other slots 0), so a
        # simple slot-0 sample misses every binding value.
        # ``max`` and ``min`` correctly pick the binding edge
        # regardless of which PERIOD the CSV uses for each day.
        per_day_emin = [
            max(emin_series[d * 24 : (d + 1) * 24], default=0.0)
            for d in range(bundle.n_days)
        ]
        per_day_emax_raw = [
            [v for v in emax_series[d * 24 : (d + 1) * 24] if v > 0.0]
            for d in range(bundle.n_days)
        ]
        per_day_emax = [min(chunk) if chunk else 0.0 for chunk in per_day_emax_raw]
        static_emax = (
            db.static_property("Storage", storage.object_id, "Max Volume") or 0.0
        )
        static_emin = (
            db.static_property("Storage", storage.object_id, "Min Volume") or 0.0
        )
        # Scalar fallbacks.  ``emin`` uses the PHYSICAL static floor
        # (PLEXOS ``Min Volume`` property) only — NOT ``max(emin_series)``,
        # which would lift the operational end-of-day floor (e.g.
        # ELTORO's 12,142 GWh end-of-week target) into a constant
        # block-by-block hard floor, making the LP infeasible whenever
        # natural inflows can't refill the reservoir.  The end-of-day
        # floor is honoured separately as a SOFT ``efin`` slack below.
        # ``emax`` continues to take the binding (min) CSV value so
        # operational caps are respected at every block (caps are
        # typically physically achievable; floors are aspirational).
        emax = min(emax_series) if emax_series else static_emax
        emin = static_emin
        eini = eini_series[0] if eini_series else 0.0
        if emax == 0.0:
            emax = static_emax
        if eini == 0.0:
            eini = db.static_property("Storage", storage.object_id, "Initial Volume")
        # ── Build per-block emin/emax profile ─────────────────────────
        # PLEXOS shape: ``Hydro_*Volume.csv`` ships operational
        # floors / caps at SPECIFIC end-of-day hours (slot 23, 47,
        # …, 167 for a 7-day week).  We verified for ELTORO that
        # PLEXOS binds the floor EXACTLY at those hours.
        #
        # Conservative pass: only honour the FIRST-day and LAST-day
        # end-of-day floors / caps (hour 24 and hour ``n_days * 24``).
        # Mid-week end-of-day spikes are skipped — they over-
        # constrained v17 for reservoirs lacking an nphi safety
        # valve (e.g. L_Maule blocks 82/97 hitting 7,969 with only
        # 175 hm³ headroom).  Block 0 always uses the static
        # physical floor / cap.
        emin_profile: tuple[float, ...] = ()
        emax_profile: tuple[float, ...] = ()
        block_layout = getattr(bundle, "block_layout", ())
        if block_layout and (emin_series or emax_series):
            n_blocks = len(block_layout)
            # NO per-block emin clamp from the CSV.  All end-of-day
            # operational floors are now honoured exclusively as a SOFT
            # ``efin`` + ``efin_cost`` slack on the LAST-day EOD (set
            # below).  Intermediate (first-day, mid-week) EOD floors are
            # skipped so the LP isn't over-constrained.  Uniform across
            # horizon length — hard per-block floors caused infeasibility
            # chains we'd have to debug per bundle; the soft efin gives
            # a priced escape consistent for n_days = 1 and n_days = 7.
            allowed_eod_hours: set[int] = set()
            emin_per_block: list[float] = []
            emax_per_block: list[float] = []
            for intervals in block_layout:
                # 1-indexed hour intervals → 0-indexed CSV slot;
                # only consider hours that match one of our allowed
                # end-of-day boundaries (first / last day).
                csv_emin_in_block = [
                    emin_series[h - 1]
                    for h in intervals
                    if h in allowed_eod_hours
                    and 0 < h <= len(emin_series)
                    and emin_series[h - 1] > 0.0
                ]
                csv_emax_in_block = [
                    emax_series[h - 1]
                    for h in intervals
                    if h in allowed_eod_hours
                    and 0 < h <= len(emax_series)
                    and emax_series[h - 1] > 0.0
                ]
                emin_per_block.append(
                    max([static_emin] + csv_emin_in_block)
                    if csv_emin_in_block
                    else static_emin
                )
                emax_per_block.append(
                    min([static_emax] + csv_emax_in_block)
                    if csv_emax_in_block and static_emax > 0.0
                    else static_emax
                )
            # Only emit the profile when it actually varies across
            # blocks (otherwise the scalar emin/emax suffices).
            if len(set(emin_per_block)) > 1:
                emin_profile = tuple(emin_per_block)
            if len(set(emax_per_block)) > 1:
                emax_profile = tuple(emax_per_block)
        # ── End-of-horizon target ─────────────────────────────────────
        # Preferred: pin to the PLEXOS-solution End Volume (prop 646)
        # at the last horizon period, so gtopt's reservoir trajectory
        # matches PLEXOS exactly.  Without this, the LP is allowed to
        # drain all the way to the operational floor below, which lets
        # COLBUN/RALCO/CANUTILLAR over-dispatch by thousands of CMD
        # (and inflates hydro generation by ~80 % vs PLEXOS on the
        # CEN PCP weekly bundle).
        # Fallback: the LAST-day end-of-day floor from
        # ``Hydro_MinVolume.csv`` (PLEXOS slot ``bundle.n_days * 24 - 1``)
        # — used when no solution .accdb is available.
        efin: float = 0.0
        if name in solution_efin:
            efin = float(solution_efin[name])
        elif emin_series:
            last_day_slot = bundle.n_days * 24 - 1  # 0-indexed
            if 0 <= last_day_slot < len(emin_series):
                last_day_floor = emin_series[last_day_slot]
                # Only emit when the CSV ships a binding end-of-horizon
                # floor above the static physical emin.
                if last_day_floor > max(static_emin, 0.0):
                    efin = float(last_day_floor)
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
        # ── Pass-through PLEXOS Storage objects ─────────────────
        # CEN PCP "Storage" entries that are topology-only nodes
        # (bocatomas ``B_*``, ``Post_*``, run-of-river intakes,
        # ``LAJA_I``) ship every volume / water-value property at
        # 0.  We KEEP them as zero-storage Reservoirs (rather than
        # filtering them out) so the writer's automatic
        # ``spillway_cost=1000`` drain absorbs any net inflow at
        # the junction — matches the v10 behaviour and avoids the
        # under-constrained-drain infeasibility v17/v18/v19 hit
        # when these nodes were promoted to bare Junctions.  The
        # ``Post_Pangue accumulated 43k hm³`` artifact is acceptable
        # for now: the drain cost ($1000/m³/s·h) is high enough
        # that the LP only drains as a last resort, and the
        # accumulated volume in a zero-emax reservoir is a
        # reporting artifact, not a real cost.
        out.append(
            ReservoirSpec(
                object_id=storage.object_id,
                name=name,
                emin=emin,
                emax=emax,
                eini=eini,
                efin=efin,
                water_value=water_value_gwh,
                never_drain=never_drain,
                spill_penalty_per_mwh=spill_penalty,
                emin_profile=emin_profile,
                emax_profile=emax_profile,
            )
        )
    if skipped_lng:
        logger.info(
            "extract_reservoirs: dropped %d PLEXOS Storage object(s) with "
            "`_GNL_INF` suffix (LNG gas-import accounting artifacts, not "
            "water reservoirs).",
            skipped_lng,
        )
    return tuple(out)


def extract_waterways(
    db: PlexosDb,
    bundle: PlexosBundle | None = None,
    forced_targets_out: list[tuple[str, str, float]] | None = None,
    ocean_sources_out: set[str] | None = None,
    junction_drain_configs_out: dict[str, dict[str, float | None]] | None = None,
) -> tuple[WaterwaySpec, ...]:
    """One :class:`WaterwaySpec` per PLEXOS Waterway object.

    Endpoints come from the Waterway→Storage ``Storage From`` /
    ``Storage To`` collections (collections 104 / 105 in the CEN PCP
    schema). Waterways with only one endpoint are dropped — a 1-ended
    waterway can't add a constraint to the LP. The optional fmin/fmax
    are static properties on the System→Waterways collection.

    ``Hydro_WaterFlows.csv`` (WIDE, one column per Storage or Waterway)
    additionally carries FORCED flows for filtration / seepage
    waterways (``Filt_Laja``, ``Filt_Inv``, ``Filt_Colb`` …): these
    are gravity-driven gravity flows that always run regardless of
    operator choice.  When a Waterway name matches a column in that
    CSV with a non-zero constant value, we pin ``fmin = fmax = value``
    so the LP variable is fixed at the physical forced flow.

    Without that pinning, downstream junctions (e.g. ``POLCURA`` fed
    by ``Filt_Laja`` from ``ELTORO``) lose ~21 m³/s of baseline
    supply and the LP becomes infeasible whenever the downstream
    discharge UC (``discharge_ANTUCOmin``) requires more flow than
    the natural inflow + capped turbine spillway can supply.
    """
    forced_flows: dict[str, list[float]] = {}
    if bundle is not None and bundle.has("Hydro_WaterFlows.csv"):
        forced_flows = read_wide(
            bundle.csv("Hydro_WaterFlows.csv"), n_days=bundle.n_days
        )
    from_coll = db.collection_for_named("Waterway", "Storage", "Storage From")
    to_coll = db.collection_for_named("Waterway", "Storage", "Storage To")
    objs = db.object_by_id()
    p2c_from = db.parent_to_children(from_coll.collection_id) if from_coll else {}
    p2c_to = db.parent_to_children(to_coll.collection_id) if to_coll else {}
    out: list[WaterwaySpec] = []
    pinned_count = 0
    synthetic_sinks: list[str] = []
    # ``Vert_*`` spillways are redirected to a synthetic
    # ``<source>_ocean`` drain junction (mirrors PLP's terminal /
    # vrebemb-as-sink topology).  Collected here so the caller can
    # synthesise the matching drain junctions with ``drain = True``;
    # without the redirect, PLEXOS spillways would route into the
    # downstream reservoir's storage row and the LP could discharge
    # one reservoir to relieve pressure on the next, which is not
    # physical.  Routing to an ocean drain removes that arbitrage
    # path and matches PLP's "spill leaves the basin" convention.
    synthetic_ocean_sources: set[str] = set()
    # Waterways to drop entirely.  ``Vert_ELTORO`` is the operator-
    # controlled spillway from ELTORO → POLCURA in the Laja cascade,
    # but it is **physically inactive**: ELTORO's reservoir is far too
    # large for spillover to occur over any realistic horizon, and
    # PLEXOS keeps it at zero via Water Value.  Leaving the arc in
    # the LP lets the LP drain ELTORO from eini=12,155 hm³ down to
    # ~31 hm³ over the week as a free arbitrage (pushing water
    # through POLCURA → Post_Antuco → RUCUE → LAJA_I), verified in
    # the v21 phantom-storage audit.  Drop it entirely to mirror
    # PLEXOS's behaviour without re-introducing Water Value pricing.
    SKIP_WATERWAYS: frozenset[str] = frozenset({"Vert_ELTORO"})
    for ww in db.objects_of_class("Waterway"):
        if ww.name in SKIP_WATERWAYS:
            logger.info(
                "extract_waterways: dropping %s (operator-controlled "
                "spillway; PLEXOS keeps it at zero via Water Value, "
                "we drop it to prevent free phantom-storage drain).",
                ww.name,
            )
            continue
        fs = p2c_from.get(ww.object_id, [])
        ts = p2c_to.get(ww.object_id, [])
        f_name = objs[fs[0]].name if fs and fs[0] in objs else None
        t_name = objs[ts[0]].name if ts and ts[0] in objs else None
        # ``Hydro_WaterFlows.csv`` may carry a FORCED-flow column for
        # this waterway (filtration / seepage / irrigation extraction
        # that always runs regardless of operator decision).
        forced = forced_flows.get(ww.name, [])
        has_forced = bool(forced) and max(forced) > 0.0
        # PLEXOS sometimes ships forced-outflow waterways with only a
        # ``Storage From`` (no ``Storage To``) — modelled as "drains
        # away" in PLEXOS's reservoir-balance accounting.  gtopt
        # requires both endpoints, so when a 1-ended waterway has a
        # CSV-pinned forced flow we synthesise a sink junction
        # ``<name>_sink`` and rely on the writer's automatic
        # spillway_cost drain on its co-located zero-storage
        # reservoir.  Spillways (``Vert_*``) without a CSV pin stay
        # dropped — operator-controlled spill can fall back to the
        # source reservoir's own drain.
        if t_name is None and f_name is not None and has_forced:
            t_name = f"{ww.name}_sink"
            synthetic_sinks.append(t_name)
        # Operator-controlled spillways (``Vert_*``).
        #
        # PLEXOS routes ``Vert_PANGUE`` to ANGOSTURA (the next reservoir
        # in the cascade), but letting the LP route spillway flow into
        # the downstream storage row creates an arbitrage path —
        # spilling one reservoir relieves pressure on the next, which
        # is not physical.
        #
        # When ``junction_drain_configs_out`` is provided, we COLLAPSE
        # the legacy ``Vert_<src>`` Waterway + synthetic ``<src>_ocean``
        # Junction pair into a single ``Junction{drain: true,
        # drain_capacity, drain_cost}`` row on the source storage's
        # junction.  ``JunctionLP::add_to_lp`` builds the per-block
        # drain column with the same ``uppb`` and ``cost`` the
        # Waterway used to carry on its ``fmax`` / ``fcost`` — same
        # LP, one less Waterway and one less Junction per terminal
        # spillway.  Mirrors the plp2gtopt collapse that landed in
        # commit 3d977d57a.
        #
        # When the caller doesn't pass the collector (legacy callers /
        # unit tests), keep the old ocean-redirect path so behaviour is
        # backward compatible.
        #
        # ``Vert_*_GNL_INF`` spillways are dropped entirely — their
        # source Storage is an LNG gas-import accounting artifact
        # (see extract_reservoirs), filtered out of ``reservoir_array``
        # and ``junction_array``; emitting the spillway would leave a
        # waterway dangling at a non-existent source.
        if ww.name.startswith("Vert_") and f_name is not None:
            if f_name.endswith("_GNL_INF"):
                continue
            # New upstream collapse path (efcf98ac1): when the caller
            # supplies ``junction_drain_configs_out``, harvest the
            # Vert_*'s ``Max Flow`` / ``Max Flow Penalty`` into the
            # source junction's ``drain_capacity`` / ``drain_cost``
            # and SKIP emitting the Waterway entirely.  This replaces
            # the synthetic ``<src>_ocean`` Waterway+Junction pair
            # with a single drain row on the source junction.
            if junction_drain_configs_out is not None:
                fmax_raw = db.static_property("Waterway", ww.object_id, "Max Flow")
                fcost_raw = db.static_property(
                    "Waterway", ww.object_id, "Max Flow Penalty"
                )
                drain_capacity = (
                    float(fmax_raw) if fmax_raw and fmax_raw > 0.0 else None
                )
                drain_cost = float(fcost_raw) if fcost_raw and fcost_raw > 0.0 else None
                # CLI overrides for the spillway cost — apply to the
                # collapsed Junction.drain_cost (same effect as the
                # legacy Vert_* fcost override below, but at the
                # post-collapse representation).  Used to push water
                # back through downstream turbines when the LP prefers
                # to drain via Vert→ocean instead of routing through
                # the cascade (CEN PCP weekly bundle: gtopt under-flows
                # Maule turbines because the Bíobío drain at $3.6/m³
                # is cheaper than routing through PEHUENCHE/COLBUN/etc.).
                import os as _os

                _ov = _os.environ.get("GTOPT_SPILL_FCOST")
                _sc = _os.environ.get("GTOPT_SPILL_FCOST_SCALE")
                if _ov is not None:
                    try:
                        drain_cost = float(_ov)
                    except ValueError:
                        pass
                if _sc is not None and drain_cost is not None:
                    try:
                        drain_cost = drain_cost * float(_sc)
                    except ValueError:
                        pass
                cfg = junction_drain_configs_out.setdefault(f_name, {})
                cfg["drain_capacity"] = drain_capacity
                cfg["drain_cost"] = drain_cost
                continue  # skip Waterway emission entirely

            # ╔═════════════════════════════════════════════════════════╗
            # ║ Vert routing mode — WHY this is an option              ║
            # ║                                                        ║
            # ║ THIS OPTION EXISTS SOLELY TO REPRODUCE A STRANGE       ║
            # ║ PLEXOS BEHAVIOUR THAT IS NOT NATURAL TO OUR LP.        ║
            # ╚═════════════════════════════════════════════════════════╝
            #
            # **The strange PLEXOS behaviour we are trying to reproduce:**
            #
            # On the CEN PCP weekly bundle PLEXOS spills 7,479 m³/s·h
            # of water at MID-CASCADE Vert arcs (Vert_LAJA_I = 2,316;
            # Vert_RUCUE = 1,059; Vert_B_C_Isla = 2,858; Vert_B_M_Isla
            # = 910; Vert_ANTUCO = 175; Vert_SANIGNACIO = 160) — water
            # that, if it kept flowing through the cascade, could be
            # turbined by 1–8 downstream stations and produce ~78 GWh
            # of "free" hydro generation.  PLEXOS chooses to spill
            # this water mid-cascade despite zero direct cost.
            #
            # From a pure-LP perspective this is ANOMALOUS: hydro has
            # no marginal cost in either model, so the optimisation
            # should always prefer turbining over spilling.  The
            # PLEXOS choice is driven by features we do NOT yet
            # replicate (per-block ``Min Generation`` / ``Max
            # Generation`` published per period, unit-commitment
            # decisions, multi-band Water Value on storage, must-run
            # status).  Until those are wired in, the two converters
            # see fundamentally different optimisation problems and
            # the LP can't NATURALLY reproduce PLEXOS's mid-cascade
            # spillage.
            #
            # This routing toggle is therefore a DIAGNOSTIC SWITCH —
            # not a physically motivated modelling choice.  We pick
            # whichever wiring produces dispatch CLOSER to PLEXOS on
            # the reference bundle, accepting that "closer to PLEXOS"
            # may mean "less physically accurate" until the missing
            # pricing signals land.
            #
            # **What this option does:**
            #
            # ``GTOPT_VERT_ROUTING`` (or the ``--vert-routing`` CLI
            # flag on plexos2gtopt) selects the spillway destination
            # for every ``Vert_*`` waterway:
            #
            #   ocean    (default) — every Vert_* → <source>_ocean
            #            drain.  Spillage LEAVES the topology
            #            entirely; the LP loses the water
            #            permanently.  Matches the legacy
            #            plexos2gtopt behaviour and is closer to
            #            PLEXOS dispatch on the CEN PCP weekly
            #            bundle (gtopt 503 GWh vs PLEXOS 286 — both
            #            overdispatch hydro, but ocean is the
            #            tighter of the two: with cascade routing
            #            the LP would extract even more MWh from
            #            recycled spillage).
            #
            #   cascade  — keep the PLEXOS-published downstream
            #            target (Tail Storage) so spillage feeds the
            #            next cascade junction, where it CAN be
            #            re-turbined by downstream stations.  This
            #            mirrors plp2gtopt's ``_ver`` routing for
            #            centrals with non-zero PLP ``ser_ver`` and
            #            is the topologically correct PLEXOS shape.
            #            BUT in our LP it produces MORE hydro
            #            generation than ocean mode (every cumec
            #            spilled gets turbined again downstream),
            #            making the gap to PLEXOS worse, not better.
            #            Useful for diagnostics and for the day we
            #            wire in per-block pmax / water values that
            #            would naturally suppress the unwanted
            #            recycling.  Falls back to <source>_ocean
            #            when PLEXOS publishes no downstream target
            #            (terminal reservoirs like LMAULE, COLBUN,
            #            RAPEL — these always go to ocean).
            import os

            _routing = os.environ.get("GTOPT_VERT_ROUTING", "ocean").lower()
            if _routing == "cascade" and t_name is not None:
                # Keep the PLEXOS-published junction_b — no override.
                pass
            else:
                t_name = f"{f_name}_ocean"
                synthetic_ocean_sources.add(f_name)
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
        # Vert_* spill cost overrides via env vars (quick CLI for
        # turbine-vs-spill tradeoff tuning):
        #   GTOPT_SPILL_FCOST=<value>       absolute override
        #   GTOPT_SPILL_FCOST_SCALE=<value> multiplicative scale on PLEXOS value
        # Applied only to Vert_* spillway waterways.
        if ww.name.startswith("Vert_"):
            import os

            _override = os.environ.get("GTOPT_SPILL_FCOST")
            _scale = os.environ.get("GTOPT_SPILL_FCOST_SCALE")
            if _override is not None:
                try:
                    fcost = float(_override)
                except ValueError:
                    pass
            if _scale is not None:
                try:
                    fcost *= float(_scale)
                except ValueError:
                    pass
        # Pin every forced-flow waterway as ``fmin = fmax = forced``
        # so the LP physically routes the PLEXOS-mandated flow from
        # upstream to downstream.  Applies uniformly to:
        #   * Filt_* (filtration / seepage)
        #   * Caudal_Eco_* (ecological flow obligations)
        #   * Riego_* (irrigation diversions — water lost to
        #     agriculture, NOT turbined)
        #   * Ext_* (external diversions)
        # When the CSV column varies across the week (e.g. B_Maule:
        # 11.63 → 8.45 → 9.30 m³/s, or Riego_SANIGNACIO: 29.6 for
        # 157 hours + 0 for 11 hours where irrigation pauses), keep
        # the FULL per-hour series so the writer emits a per-block
        # matrix; the zeros are real "no obligation this hour"
        # semantics.  Pinning to ``max(forced)`` uniformly was the
        # source of phantom-water generation at B_Maule and would
        # over-deliver on Riego_SANIGNACIO by ~7 %.
        #
        # Earlier this code-path converted Caudal_Eco_/Riego_/Ext_/
        # Filt_Laja to soft FlowRights to avoid LP infeasibility when
        # ELTORO/COLBUN couldn't physically afford the obligation over
        # the operational floor.  With ``Reservoir.efin`` now pinned to
        # the PLEXOS-solution End Volume (``solution_efin`` lookup
        # above), reservoirs match PLEXOS's trajectory exactly, so
        # the obligation IS feasible and the hard pin is the right
        # choice.  Without it ~28 000 m³/s·h that PLEXOS routes to
        # irrigation / ecology gets free-routed through penstocks in
        # gtopt, inflating hydro generation by ~60 % on the CEN PCP
        # weekly bundle (2026-05-21 measurement: 459 GWh gtopt vs
        # 286 GWh PLEXOS).
        forced_target = max(forced) if has_forced else 0.0
        forced_profile: tuple[float, ...] = ()
        # Bypass waterways (``B_<reservoir>``): PLEXOS routes water
        # ABOVE the published Min Flow when surplus is available
        # (verified 2026-05-22 on the CEN PCP weekly bundle:
        # ``B_Maule`` carries 12,501 m³/s·h in PLEXOS vs 1,553 if
        # pinned to ``Hydro_WaterFlows.csv``).  Emit the CSV value as
        # ``fmin`` only — leave ``fmax`` unbounded so the LP can route
        # the extra flow.  Other forced-flow waterways
        # (Riego_/Caudal_Eco_/Filt_/Ext_) ARE pinned to fmin = fmax =
        # csv because their water IS removed at the published rate.
        is_bypass = ww.name.startswith("B_")
        if has_forced and forced_target > 0.0:
            fmin = forced_target
            fmax = 0.0 if is_bypass else forced_target
            pinned_count += 1
            if min(forced) != max(forced):
                forced_profile = tuple(forced)
        out.append(
            WaterwaySpec(
                object_id=ww.object_id,
                name=ww.name,
                storage_from=f_name,
                storage_to=t_name,
                fmin=fmin,
                fmax=fmax,
                forced_flow_profile=forced_profile,
                fcost=fcost,
                pin_fmax_from_profile=not is_bypass,
            )
        )
    if pinned_count:
        logger.info(
            "extract_waterways: converted %d forced-flow waterway(s) from "
            "Hydro_WaterFlows.csv to FlowRight (Caudal_Eco_* / Filt_* / "
            "Riego_* / Ext_Maule); the Waterway is dropped and the soft "
            "delivery target is carried on a FlowRight at the source "
            "junction at hydro_spill_cost ($10/m³).",
            pinned_count,
        )
    if synthetic_sinks:
        logger.info(
            "extract_waterways: synthesised %d sink junction(s) for 1-ended "
            "forced-outflow waterways (PLEXOS 'drains to nowhere' pattern); "
            "sinks: %s",
            len(synthetic_sinks),
            ", ".join(synthetic_sinks),
        )
    if synthetic_ocean_sources:
        logger.info(
            "extract_waterways: redirected %d Vert_* spillway(s) to per-source "
            "ocean drain(s) (<source>_ocean, drain=True) so spillage cannot "
            "relieve pressure on downstream storage rows; sources: %s",
            len(synthetic_ocean_sources),
            ", ".join(sorted(synthetic_ocean_sources)),
        )
    if junction_drain_configs_out:
        logger.info(
            "extract_waterways: collapsed %d Vert_* spillway(s) onto "
            "Junction.drain_capacity / drain_cost on the source storage's "
            "junction (saves 1 Waterway + 1 ocean Junction per spillway); "
            "sources: %s",
            len(junction_drain_configs_out),
            ", ".join(sorted(junction_drain_configs_out.keys())),
        )
    if ocean_sources_out is not None:
        ocean_sources_out.update(synthetic_ocean_sources)
    return tuple(out)


def _is_sink_junction(name: str) -> bool:
    """Return True for junctions that are meant to absorb water leaving
    the basin:
      * ``Riego_*_sink`` — irrigation diversions,
      * ``Filt_*_sink`` — filtration / seepage outflows,
      * any synthetic 1-ended forced-outflow target generated by
        :func:`extract_waterways`,
      * ``<reservoir>_ocean`` — synthetic ocean drain receiving
        every ``Vert_*`` operator-controlled spillway.

    These nodes need ``Junction.drain = True`` so the LP balance row
    accepts a free pass-through column; without it the equality
    constraint would force inflow = 0 and the forced flow would
    become infeasible.
    """
    return name.endswith("_sink") or name.endswith("_ocean")


def extract_junctions(
    reservoirs: tuple[ReservoirSpec, ...],
    extra_junction_names: tuple[str, ...] = (),
    drain_configs: dict[str, dict[str, float | None]] | None = None,
) -> tuple[JunctionSpec, ...]:
    """Synthesise one Junction per Reservoir + per extra junction name.

    gtopt requires explicit Junction nodes; PLEXOS folds the
    storage+topology concept into a single Storage object. We emit
    one Junction per Reservoir with the same name (the Reservoir
    JSON binding accepts a ``junction`` ref by name).

    ``extra_junction_names`` covers PLEXOS Storage objects that have
    no real storage (pass-through nodes like ``Post_Pangue``,
    ``B_C_Isla``, ``LAJA_I``, etc.) — these are dropped from the
    Reservoir list (so they don't become free phantom buffers in
    the LP) but their Junction node is kept so waterway endpoints
    that reference them stay valid.  Duplicates are de-duplicated.

    Junctions whose name ends in ``_sink`` (synthetic 1-ended
    forced-outflow targets + ``Riego_*_sink`` / ``Filt_*_sink``
    diversion / seepage outlets) are emitted with
    ``drain = True`` — the LP needs a free pass-through column on
    those nodes' balance row so the forced flow has somewhere to
    go.  Reservoir-side ``_sink`` nodes get the same treatment
    (the zero-storage Reservoir co-located at the sink junction
    can never accumulate, but the junction's balance constraint
    still has to absorb the inflow).

    ``drain_configs`` maps ``junction_name → {'drain_capacity',
    'drain_cost'}`` for junctions that should carry an explicit
    bounded drain (collapsed from a ``Vert_*`` spillway arc).
    Sets ``drain = True`` plus the new ``Junction.drain_capacity`` /
    ``drain_cost`` fields on the matching junction.
    """
    drain_configs = drain_configs or {}

    def _build(name: str, *, sink: bool) -> JunctionSpec:
        cfg = drain_configs.get(name)
        if cfg is None:
            return JunctionSpec(name=name, drain=sink)
        # A ``Vert_*`` collapse always implies ``drain = True``; combine
        # with any pre-existing ``sink`` flag for robustness.
        return JunctionSpec(
            name=name,
            drain=True,
            drain_capacity=cfg.get("drain_capacity"),
            drain_cost=cfg.get("drain_cost"),
        )

    seen = {r.name for r in reservoirs}
    out: list[JunctionSpec] = [
        _build(r.name, sink=_is_sink_junction(r.name)) for r in reservoirs
    ]
    for name in extra_junction_names:
        if name and name not in seen:
            seen.add(name)
            out.append(_build(name, sink=_is_sink_junction(name)))
    return tuple(out)


def extract_turbines(db: PlexosDb, bundle: PlexosBundle) -> tuple[TurbineSpec, ...]:
    """One :class:`TurbineSpec` per Generator with a Head Storage link.

    PLEXOS encodes the turbine-to-reservoir relationship via the
    Generator→Storage "Head Storage" collection.  When PLEXOS also
    ships a "Tail Storage" link, capture it so the writer can
    synthesise a per-turbine penstock with the right downstream
    junction (Head→Tail) instead of cloning the spillway path.

    ``production_factor`` (MW per m³/s) comes from PLEXOS's
    ``"Efficiency Incr"`` t_data on the System→Generator collection.
    CEN PCP ships this for 40 hydro units (e.g. ``ANTUCO_U1 = 1.6``,
    ``ANGOSTURA_U1 = 0.43``).  When the t_data path is empty we fall
    back to ``Hydro_EfficiencyIncr.csv`` (same per-Generator schema).
    The historical ``"Production Rate"`` name is a PLEXOS export
    artefact that does NOT exist in CEN PCP — looking for it produced
    a silent 0 on every turbine, defaulting to gtopt's PF = 1 MW/m³/s
    (which is right for ANTUCO but wrong by 2-3× for ANGOSTURA and
    many other plants).
    """
    head_coll = db.collection_for_named("Generator", "Storage", "Head Storage")
    if head_coll is None:
        return ()
    tail_coll = db.collection_for_named("Generator", "Storage", "Tail Storage")
    tail_by_gen: dict[int, str] = {}
    objs = db.object_by_id()
    if tail_coll is not None:
        for m in db.memberships_of(tail_coll.collection_id):
            res_obj = objs.get(m.child_object_id)
            if res_obj is not None:
                tail_by_gen[m.parent_object_id] = res_obj.name

    # CSV fallback: Hydro_EfficiencyIncr.csv keyed by generator name.
    csv_pf: dict[str, float] = {}
    if bundle.has("Hydro_EfficiencyIncr.csv"):
        try:
            csv_pf_raw = read_long(
                bundle.csv("Hydro_EfficiencyIncr.csv"),
                n_days=bundle.n_days,
            )
            for k, vals in csv_pf_raw.items():
                if vals:
                    nonzero = [v for v in vals if v > 0.0]
                    if nonzero:
                        csv_pf[k] = nonzero[0]
        except (OSError, ValueError) as exc:
            logger.debug("Hydro_EfficiencyIncr.csv fallback failed: %s", exc)

    out: list[TurbineSpec] = []
    for m in db.memberships_of(head_coll.collection_id):
        gen_obj = objs.get(m.parent_object_id)
        res_obj = objs.get(m.child_object_id)
        if gen_obj is None or res_obj is None:
            continue
        # Prefer ``Hydro_EfficiencyIncr.csv`` (the real engineering
        # values, e.g. ANTUCO_U1=1.6, ANGOSTURA_U1=0.43).  PLEXOS's
        # ``"Efficiency Incr"`` t_data ships a placeholder ``1.0`` for
        # every hydro generator in CEN PCP — using it directly would
        # give every turbine a 1 MW per m³/s conversion, wrong by 2-3×
        # for many plants.
        pf = csv_pf.get(gen_obj.name)
        if pf is None or pf <= 0.0:
            pf = db.static_property("Generator", gen_obj.object_id, "Efficiency Incr")
        out.append(
            TurbineSpec(
                generator_name=gen_obj.name,
                reservoir_name=res_obj.name,
                production_factor=pf,
                tail_reservoir_name=tail_by_gen.get(gen_obj.object_id),
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
    out: list[FlowSpec] = []
    if bundle.has("Hydro_WaterFlows.csv"):
        inflows = read_wide(bundle.csv("Hydro_WaterFlows.csv"), n_days=bundle.n_days)
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

    # PLEXOS-style "Non-physical Inflow Penalty" slack Flows are
    # DISABLED.  PLEXOS uses these to let the LP inject virtual water
    # at a penalty cost so the storage balance closes under tight
    # forced flows / reserve obligations — useful in PLEXOS to keep
    # demonstration scenarios feasible, but in gtopt they would mask
    # genuine infeasibilities and let the LP buy its way out of
    # cascade constraints at a fixed cost.  Operators rely on the
    # native infeasibility signal to catch model bugs (mis-aligned
    # forced flows, broken hydro chains, irrigation over-commitments),
    # so we surface the infeasibility instead of papering over it.
    #
    # The legacy emission read ``Non-physical Inflow Penalty`` off
    # each Storage and produced one ``nphi_<storage>`` FlowSpec with a
    # 5000 m³/s cap at the published penalty (25,200 $/(m³/s)/h on
    # the 2026-04-22 CEN PCP bundle).  Restore by reverting this
    # block if a future use case genuinely needs the slack — but
    # prefer fixing the root infeasibility first.
    return tuple(out)


def _parse_res_requirement_csv(
    path: Path,
    reserve_names: frozenset[str],
    n_days: int = 1,
) -> dict[str, list[float]]:
    """Parse ``Res_Requirement.csv``'s ``NAME, PATTERN, VALUE`` layout.

    Only rows whose ``NAME`` matches a known Reserve object are kept;
    PATTERN ``"DO_d,Hh"`` is parsed for ``Hh`` (1..24). Day-of-week
    field ``DO_d`` is ignored — CEN PCP is a daily run.

    Returns ``{reserve_name -> (24*n_days)-element profile}``.  The
    24-hour daily pattern is replicated ``n_days`` times so the per-
    block requirement matches the LP's horizon; without this gtopt's
    ``FieldSched::optval`` would read past the end of the array for
    blocks beyond day 0 — observed as ``2.83e+256`` lower bounds on
    ``reservezone_drequirement_12_<scene>_<stage>_<block>`` columns on
    the 7-day CEN PCP case.
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
    if n_days > 1:
        for k, daily in out.items():
            out[k] = list(daily) * n_days
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
            bundle.csv("Res_Requirement.csv"),
            reserve_names,
            n_days=bundle.n_days,
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
    # uses ``VoRS`` (Value of Reserve Shortage).
    #
    # Sentinel handling: PLEXOS uses ``VoRS = -1`` to mean "make the
    # reserve constraint HARD" (no shortage admissible — the constraint
    # is enforced as an equality on the requirement column with no
    # slack variable, and the dual on the binding equality IS the
    # implicit reserve cost).  We map that to gtopt's "hard" form by
    # leaving ``urcost``/``drcost`` at ``0.0``; the writer omits the
    # field, and ``reserve_zone_lp.cpp`` then fixes the requirement
    # column to ``lowb = uppb = block_rreq`` — no slack added.
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
        # PLEXOS ``Min Provision`` is a STATIC constant floor (MW) on the
        # provided reserve, distinct from the time-varying CSV profile.
        # CPF zones and most BESS zones carry only this static floor —
        # without folding it into the requirement RHS those zones get
        # zero reserve and PLEXOS's $0.65M reserve cost is unreproducible.
        min_provision = (
            db.static_property("Reserve", rsv.object_id, "Min Provision") or 0.0
        )
        # Build the requirement profile: per-block max(csv_value, min_provision).
        # When the CSV is absent the requirement is a flat
        # ``min_provision``; when the CSV is present each hour gets the
        # max of the two so the static floor still binds even on the
        # low-requirement hours that ``Res_Requirement.csv`` reports.
        target_len = 24 * bundle.n_days
        if profile:
            requirement = tuple(max(float(v), min_provision) for v in profile)
        elif min_provision > 0.0:
            requirement = tuple([min_provision] * target_len)
        else:
            requirement = ()
        ur_req: tuple[float, ...] = ()
        dr_req: tuple[float, ...] = ()
        if requirement:
            if is_down:
                dr_req = requirement
            else:
                ur_req = requirement
        plexos_type_raw = db.static_property("Reserve", rsv.object_id, "Type")
        plexos_type = int(plexos_type_raw) if plexos_type_raw else 0
        type_tag = type_tag_map.get(plexos_type, "other")
        violation_cost = 0.0
        for prop_name in violation_cost_props:
            val = db.static_property("Reserve", rsv.object_id, prop_name)
            if val is None:
                continue
            if val == -1.0:
                # PLEXOS "use default / hard" sentinel — leave the
                # shortage cost at 0 so the writer omits the field and
                # the LP fixes the requirement column with no slack.
                violation_cost = 0.0
                break
            if val > 0.0:
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


def extract_hydro_discharge_user_constraints(
    db: PlexosDb,
    bundle: PlexosBundle,
    turbines: tuple[TurbineSpec, ...],
    generators: tuple[GeneratorSpec, ...] = (),
) -> tuple[UserConstraintSpec, ...]:
    """Build PLEXOS ``<plant>min`` / ``<plant>max`` discharge constraints.

    ``Hydro_AntucoBounds.csv`` ships per-day rows for objects named
    ``ANTUCOmin`` / ``ANTUCOmax`` / ``ELTOROmax`` whose PLEXOS class is
    ``Constraint`` (NOT a junction-level water right).  Each constraint
    has ``Generator → Constraint`` memberships listing the units whose
    DISCHARGE (m³/s, not MW) the constraint bounds.  The proper gtopt
    encoding is a per-block ``UserConstraint`` summing
    ``(1/pf_i) × generator(i).generation`` over the member units, with
    operator chosen by the ``min`` / ``max`` suffix.

    Without these constraints in the LP, multi-unit hydro plants are
    free to dispatch beyond their combined penstock capacity — that's
    why our previous LP cost was ~$5 M lower than the PLEXOS MIP.
    """
    import csv  # pylint: disable=import-outside-toplevel

    if not bundle.has("Hydro_AntucoBounds.csv"):
        return ()

    # Per-name first-seen RHS value (the CSV ships one row per day;
    # collapse to the first value because gtopt's scalar RHS is shared
    # across blocks unless the new TB-schedule rhs is used).
    rhs_by_name: dict[str, float] = {}
    with Path(bundle.csv("Hydro_AntucoBounds.csv")).open(
        "r", encoding="utf-8", newline=""
    ) as fh:
        for row in csv.reader(fh):
            if not row or not row[0] or row[0] == "NAME":
                continue
            name = row[0]
            if name in rhs_by_name:
                continue
            try:
                rhs_by_name[name] = float(row[5])
            except (ValueError, IndexError):
                continue

    if not rhs_by_name:
        return ()

    # Map from generator name → production_factor (MW per m³/s).  When
    # unset the LP defaults to 1, so the constraint converts gen → flow
    # at a 1:1 ratio which is wrong for most plants but better than
    # dropping the constraint outright.
    pf_by_gen: dict[str, float] = {}
    for t in turbines:
        if t.production_factor and t.production_factor > 0.0:
            pf_by_gen[t.generator_name] = float(t.production_factor)

    # Generators are dispatchable iff their pmax is positive at EVERY
    # block — gtopt drops gen columns at blocks where the per-block
    # pmax_profile entry is 0 (out-of-service hour), and any
    # UserConstraint referencing a column that disappears for some
    # block fails its row build with "element is missing or inactive".
    # Partial-availability units (e.g. ELTORO_U1 with 13 non-zero out
    # of 111 blocks) are therefore EXCLUDED — losing precision but
    # keeping the constraint emittable.  Promoting partial gens into
    # the constraint would require a gtopt-side "always-emit zero-
    # bounded columns" change.
    dispatchable: set[str] = set()
    for g in generators:
        if g.pmax_profile:
            if all(v > 0.0 for v in g.pmax_profile):
                dispatchable.add(g.name)
        elif g.pmax and g.pmax > 0.0:
            dispatchable.add(g.name)

    # PLEXOS Generator → Constraint memberships (coll_id=32 in CEN PCP).
    coll = db.collection_for_named("Generator", "Constraint", "Constraints")
    if coll is None:
        return ()
    objs = db.object_by_id()
    members_by_constraint: dict[str, list[str]] = {}
    for m in db.memberships_of(coll.collection_id):
        gen_obj = objs.get(m.parent_object_id)
        cstr_obj = objs.get(m.child_object_id)
        if gen_obj is None or cstr_obj is None:
            continue
        if cstr_obj.name not in rhs_by_name:
            continue
        if dispatchable and gen_obj.name not in dispatchable:
            continue
        members_by_constraint.setdefault(cstr_obj.name, []).append(gen_obj.name)

    out: list[UserConstraintSpec] = []
    for cstr_name, rhs in rhs_by_name.items():
        members = members_by_constraint.get(cstr_name, [])
        if not members:
            logger.debug(
                "discharge constraint %s has RHS=%g but no Generator members; "
                "skipping (likely not parsed in this bundle)",
                cstr_name,
                rhs,
            )
            continue
        # Sense from the name suffix — case-sensitive on the trailing
        # "min"/"max" segment to match PLEXOS naming.
        lname = cstr_name.lower()
        if lname.endswith("max"):
            op = "<="
            # ``max`` is a physical penstock / turbine capacity cap —
            # exceeding it is non-physical, so keep the constraint
            # HARD (penalty=0 ⇒ no LP slack).
            uc_penalty = 0.0
        elif lname.endswith("min"):
            op = ">="
            # ``min`` is an OPERATIONAL forced-flow floor (e.g.
            # PLEXOS ``ANTUCOmin = 59 m³/s`` on the ANTUCO turbines).
            # Operators routinely violate these under stress; gtopt
            # models them as SOFT with the same hydro_spill_cost
            # ($10/(m³/s)·h) used on the soft Filt_Laja / Riego_*
            # FlowRights so the LP has a consistent "drop a forced
            # flow when the cascade can't physically meet it" cost.
            # Without this softening, the discharge floor cascaded
            # into POLCURA emin infeasibility on the 2026-04-22
            # bundle (ANTUCOmin = 59 m³/s + ELTOROmax = 36.95 m³/s
            # + POLCURA natural inflow = 9.6 m³/s ⇒ net depletion
            # 12 m³/s/h with only ~3 hm³ headroom).
            uc_penalty = 10.0
        else:
            # Mid-name "min"/"max" or unknown suffix: skip.
            continue
        terms: list[str] = []
        for gen_name in sorted(set(members)):
            pf = pf_by_gen.get(gen_name, 1.0)
            coeff = 1.0 / pf
            sign = " - " if coeff < 0 else (" + " if terms else "")
            terms.append(f'{sign}{coeff:.6g} * generator("{gen_name}").generation')
        expression = "".join(terms) + f" {op} {rhs:g}"
        out.append(
            UserConstraintSpec(
                name=f"discharge_{cstr_name}",
                expression=expression,
                penalty=uc_penalty,
                description=f"PLEXOS Constraint {cstr_name} — Σ (gen/pf) {op} {rhs}",
            )
        )
    logger.info(
        "extract_hydro_discharge_user_constraints: emitted %d discharge UCs "
        "from Hydro_AntucoBounds.csv + Generator→Constraint memberships",
        len(out),
    )
    return tuple(out)


def _synthesise_pinned_flow_rights(
    forced_waterway_targets: list[tuple[str, str, float]],
    known_junction_names: frozenset[str],
) -> tuple[FlowRightSpec, ...]:
    """Convert pinned forced-flow waterways into soft FlowRights.

    For each ``(name, source_junction, target_m3s)`` triplet captured
    by ``extract_waterways`` (Filt_Laja / Filt_Inv / Filt_Colb /
    Caudal_Eco_Ralco / Riego_RUCUE etc.), emit a FlowRight at the
    SOURCE junction with:

      * ``target = target_m3s``  (the operational delivery target)
      * ``fcost  = 36,000 $/(m³/s)/h``  (= $10/m³ × 3600 — mirrors
        PLP's ``hydro_fail_cost = 10 $/m³`` convention; converts
        per-volume to per-flow-per-hour units the LP expects)
      * ``fmax  = 10 × target``  (large enough to also serve as a
        spill outlet when upstream has surplus — FlowRight's column
        drains the junction, so any flow above target is overflow)

    The paired Waterway still exists with its operational ``fmax``
    cap but no longer hard-pins ``fmin``, so the LP gets a soft
    delivery requirement priced at the unserved cost instead of an
    infeasibility when upstream is short.
    """
    # FlowRight unserved cost — high enough that the LP only
    # shortchanges the obligation when no feasible alternative
    # exists.  Previously 10 $/(m³/s)·h; bumped to 1000 so
    # irrigation / ecological-flow shortfalls cost ~100× more
    # than thermal generation and the LP avoids them by default.
    fcost_per_cumec_hour = 1000.0
    out: list[FlowRightSpec] = []
    for name, source_junction, target in forced_waterway_targets:
        if source_junction not in known_junction_names:
            continue
        out.append(
            FlowRightSpec(
                # ``soft_`` prefix: each FlowRight here is a SOFT
                # delivery obligation priced at ``fcost`` (= $10/(m³/s)·h
                # shortfall, ≈ $10/m³).  ``target`` is the soft kink the
                # LP is steered toward; ``fmax = 10 × target`` lets the
                # LP also deliver more if it's profitable.  The legacy
                # ``pinned_`` prefix was misleading — these are not
                # hard pins.
                name=f"soft_{name}",
                junction_name=source_junction,
                purpose="forced_flow",
                fmin=0.0,
                fmax=target * 10.0,
                target=target,
                fcost=fcost_per_cumec_hour,
            )
        )
    if out:
        logger.info(
            "_synthesise_pinned_flow_rights: emitted %d soft FlowRight(s) "
            "from pinned forced-flow waterways at $%.0f/(m³/s·h) "
            "shortfall penalty ($10/m³).",
            len(out),
            fcost_per_cumec_hour,
        )
    return tuple(out)


def extract_flow_rights(
    bundle: PlexosBundle,
    turbines: tuple[TurbineSpec, ...],
    known_junctions: frozenset[str] = frozenset(),
) -> tuple[FlowRightSpec, ...]:
    """Disabled — ``Hydro_AntucoBounds.csv`` entries are NOT junction-level
    irrigation rights.

    The PLEXOS DB shows ``ANTUCOmax`` / ``ANTUCOmin`` / ``ELTOROmax``
    objects with **Generator → FlowConstraint** memberships (coll_id=32)
    — they are **per-generator turbine discharge** constraints binding the
    SUM of multiple turbine flows on a single hydro plant (e.g.
    ``ANTUCOmax = 63 m³/s`` caps the COMBINED discharge of ANTUCO_U1 +
    ANTUCO_U2).  Modelling them as FlowRights on the upstream reservoir
    introduces a phantom water-rights consumer at the junction that
    has no PLEXOS basis.

    The correct gtopt encoding is a **UserConstraint** with expression
    ``(1/pf_i) * generator(i).generation`` summed over the bound's
    member generators, bounded by the CSV value.  That mapping is a
    TODO; for now we emit no FlowRights from this CSV so the LP isn't
    biased by the wrong abstraction.  Real junction-level irrigation
    rights (if any) would come from a different source.
    """
    _ = bundle, turbines, known_junctions
    return ()


def _extract_flow_rights_legacy_misclassified(
    bundle: PlexosBundle,
    turbines: tuple[TurbineSpec, ...],
    known_junctions: frozenset[str] = frozenset(),
) -> tuple[FlowRightSpec, ...]:
    """Legacy mis-classified FlowRight extractor — kept for reference.

    ``Hydro_AntucoBounds.csv`` ships per-day rows for names like
    ``ANTUCOmin`` / ``ANTUCOmax`` / ``ELTOROmax`` — but these are
    PER-GENERATOR DISCHARGE LIMITS in PLEXOS, not junction-level water
    rights.  Emitting them as FlowRights on the upstream reservoir
    caused phantom infeasibilities; see ``extract_flow_rights``
    docstring for the proper UserConstraint reformulation.
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
                # No hardcoded shortfall penalty: PLEXOS doesn't ship
                # one, so we leave ``fcost = 0`` and the writer omits
                # the field — the LP enforces the bounds hard.  A
                # caller wanting a soft cap can set
                # ``model_options.hydro_fail_cost`` instead.
                fcost=0.0,
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
    pmax_profiles_by_gen: dict[str, tuple[float, ...]] | None = None,
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
        # Per-block RHS shift bookkeeping — accumulated from both the
        # Fuel.Offtake daily-to-block budget split AND the derived
        # Curtailed / Available Capacity coefficients below.  Hoisted
        # to the top of the per-constraint scope so all downstream
        # sections can append to it.
        gen_pmax_by_name = pmax_by_gen or {}
        gen_pmax_profiles = pmax_profiles_by_gen or {}
        rhs_shift_per_block: list[float] = []

        def _shift_at(idx: int) -> float:
            while idx >= len(rhs_shift_per_block):
                rhs_shift_per_block.append(0.0)
            return rhs_shift_per_block[idx]

        def _set_shift(idx: int, value: float) -> None:
            while idx >= len(rhs_shift_per_block):
                rhs_shift_per_block.append(0.0)
            rhs_shift_per_block[idx] = value

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
        #    over every Generator g that consumes that Fuel.  The PLEXOS
        #    constraint RHS is a CUMULATIVE budget (Day / Week / Month
        #    depending on name); CEN PCP daily case is exclusively
        #    "Day" — see the 257-vs-0 name-bucket distribution.  gtopt's
        #    UserConstraint is per-block, so we approximate the daily
        #    cumulative cap by emitting a TB-schedule ``rhs`` of
        #    ``rhs_val / blocks_per_day`` for each block.  Under uniform
        #    dispatch this sums to exactly ``rhs_val`` per day; under
        #    non-uniform dispatch the per-block cap is conservatively
        #    tight (LP will not exceed ``rhs_val/blocks_per_day`` in
        #    any single hour).  Stage-level cumulative constraints
        #    would be the principled fix when gtopt grows that surface.
        allowed_gens = (
            emitted_names.get("Generator") if emitted_names is not None else None
        )
        is_fuel_offtake = False
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
                is_fuel_offtake = True

        # When this constraint had any fuel-offtake LHS contribution,
        # convert the scalar daily cap into a per-block budget so
        # ``rhs[block] = rhs_val / blocks_per_day``.  The block count
        # comes from the longest profile we've seen so far in
        # ``rhs_shift_per_block`` (populated by curtailed /
        # available_capacity above) or — when no profile is available
        # yet — from a representative generator's pmax_profile length.
        if is_fuel_offtake:
            horizon = max(
                len(rhs_shift_per_block),
                next(
                    (len(p) for p in gen_pmax_profiles.values() if p),
                    24,
                ),
            )
            blocks_per_day = 24
            per_block_rhs = rhs_val / blocks_per_day
            # Subtract from the current per-block RHS so the final
            # emitted profile is ``rhs_val_per_block`` rather than the
            # daily total.  ``shift = rhs_val - per_block_rhs`` for
            # every block.
            shift = rhs_val - per_block_rhs
            for idx in range(horizon):
                _set_shift(idx, _shift_at(idx) + shift)

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
                elif mode in ("curtailed", "available_capacity"):
                    # Per-block RHS shift via UserConstraint.rhs TB
                    # schedule.  ``Capacity × cf[block]`` is the
                    # generator's per-block ``pmax_profile`` value
                    # (Gen_Rating.csv).  When the profile is missing,
                    # fall back to the scalar ``pmax`` broadcast across
                    # the horizon.
                    profile = gen_pmax_profiles.get(parent_name)
                    fallback_pmax = gen_pmax_by_name.get(parent_name, 0.0)
                    if not profile and fallback_pmax <= 0.0:
                        continue
                    horizon = (
                        len(profile) if profile else max(1, len(rhs_shift_per_block))
                    )
                    for idx in range(horizon):
                        cap_cf = profile[idx] if profile else fallback_pmax
                        _set_shift(idx, _shift_at(idx) + alpha * cap_cf)
                    if mode == "curtailed":
                        # α × (Capacity × cf − gen) ⇒ LHS gets −α × gen,
                        # RHS already received the +α × cap_cf shift
                        # above.
                        coeff = -alpha
                        var_ref = f'generator("{parent_name}").generation'
                        terms.append(
                            _format_coefficient(coeff, first=not terms) + var_ref
                        )
                        coefficients.append(coeff)
                    # available_capacity: pure RHS — no LHS term to emit.
                    continue
                elif mode == "forward_to_battery_gen_commit":
                    # Battery.Reserve Units → forward to the
                    # auto-synthesised ``<battery>_gen`` Generator's
                    # Commitment.  ``system.cpp::expand_batteries``
                    # creates this Commitment object with uid
                    # ``uc_<battery_name>_gen``; the
                    # ``commitment("X").status`` AMPL accessor
                    # exists and is exact.
                    var_ref = f'commitment("uc_{parent_name}_gen").status'
                    terms.append(_format_coefficient(alpha, first=not terms) + var_ref)
                    coefficients.append(alpha)
                    continue
                elif mode == "ramp_delta":
                    # Inter-block ramp constraint:
                    #   α × (gen(t) − gen(t−1)) ≤ rhs
                    # gtopt's UC parser recognises a trailing ``_prev``
                    # on the attribute name; ``element_column_resolver``
                    # strips it and looks up the SAME class+uid+base-
                    # attribute at the immediately preceding block in
                    # the chronological stage.  First-block boundary
                    # falls through to ``resolve_single_param`` which
                    # treats prior gen as 0 (cold start), so the
                    # ``−α × generation_prev`` term vanishes cleanly
                    # at t=0.
                    cur_ref = f'generator("{parent_name}").generation'
                    prev_ref = f'generator("{parent_name}").generation_prev'
                    terms.append(_format_coefficient(alpha, first=not terms) + cur_ref)
                    coefficients.append(alpha)
                    terms.append(_format_coefficient(-alpha, first=False) + prev_ref)
                    coefficients.append(-alpha)
                    continue
                else:
                    # Reserved for future cross-block coefficient kinds
                    # that can't be reformulated.  No active mode lands
                    # here today.
                    key = f"{constr.name}::{prop_name}"
                    if key not in unsupported_rhs_shift_warns:
                        unsupported_rhs_shift_warns.add(key)
                        logger.warning(
                            "constraint %s drops %s on %s — coefficient "
                            "kind has no gtopt LHS counterpart and no "
                            "per-block RHS reformulation",
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
        # Per-block effective RHS = scalar_rhs − Σ(α × Capacity × cf[t]).
        # Build the schedule only when at least one curtailed /
        # available_capacity coefficient contributed; otherwise leave
        # ``rhs_profile`` empty so the writer keeps the inline scalar.
        rhs_profile_tuple: tuple[float, ...] = ()
        if any(abs(s) > 0.0 for s in rhs_shift_per_block):
            rhs_profile_tuple = tuple(rhs_val - shift for shift in rhs_shift_per_block)
        out.append(
            UserConstraintSpec(
                name=constr.name,
                expression=expression,
                penalty=penalty_val if penalty_val and penalty_val > 0 else 0.0,
                active=False if is_inactive else None,
                rhs_profile=rhs_profile_tuple,
            )
        )
    return tuple(out)


def _build_fuel_offtake_caps_ucs(
    bundle: PlexosBundle,
    case: PlexosCase,
) -> tuple[UserConstraintSpec, ...]:
    """Synthesise UserConstraintSpec entries for PLEXOS ``FueMaxOff*``
    weekly/daily fuel-offtake caps.

    PLEXOS-CEN-PCP creates these constraints at solve time (they live
    ONLY in the solution ``.accdb``) and they cap the total offtake
    ``Σ_g heat_rate(g) × generation(g)`` over a week or a day per fuel
    name.  Without them, gtopt's LP exploits multi-band fuel contracts
    (e.g. ``Gas_Kelar_GN_A/B/C/D``) as if every band were unlimited,
    routing ~150 GWh of cheap thermal that PLEXOS-MIP would block —
    closing ~$15 M of the operational-cost gap on the CEN PCP daily
    week.

    Approach:
      1. Read the per-fuel weekly / daily cap totals from the
         ``.accdb`` cache (``extract_fuel_offtake_caps``).  Returns
         ``{fuel_name: (cap, scope_hours)}`` where the cap is in
         PLEXOS fuel units (typically GWh thermal).
      2. Build ``fuel_to_gens`` and per-gen heat-rate maps from the
         already-parsed ``case.generators`` (one Generator can list
         multiple fuels via ``GeneratorSpec.fuel_names``).
      3. For each capped fuel, emit ONE per-block UserConstraint
         with LHS ``Σ heat_rate(g) × generator(g).generation`` and
         RHS ``cap × block_duration / scope_hours`` (uniform per-
         block decomposition — conservative w.r.t. PLEXOS, which
         allows peak-vs-off-peak shaping within the scope).

    Returns an empty tuple when no cache is available, no caps were
    extracted, or no generator references the capped fuel.
    """
    if bundle.accdb_cache_dir is None or not bundle.accdb_cache_dir.is_dir():
        return ()
    from .plexos_block_layout import extract_fuel_offtake_caps

    caps = extract_fuel_offtake_caps(bundle.accdb_cache_dir)
    if not caps:
        return ()

    # Build the (fuel_name → list[(gen_name, heat_rate)]) inverse
    # index from the already-parsed generators.  When a generator
    # uses N fuels, its heat_rate is shared across all of them.
    # Skip generators with pmax = 0 — PLEXOS-CEN ships alternate-
    # fuel-mode variants (e.g. ``ATA-TG2A_GNL_E`` carrying only the
    # fuel reference for ATA-TG2A under GNL_E gas tier) that gtopt's
    # GeneratorLP omits from the AMPL element registry because they
    # have no dispatch column.  Referencing them here would yield a
    # strict-mode "cannot resolve element reference" error at LP
    # construction.
    fuel_to_gens: dict[str, list[tuple[str, float]]] = {}
    for g in case.generators:
        if not g.fuel_names or g.heat_rate <= 0.0 or g.pmax <= 0.0:
            continue
        # Skip generators whose per-block pmax profile drops to 0
        # in any block — gtopt's GeneratorLP omits the LP column on
        # zero-pmax blocks and strict-mode UC resolution then fails
        # with "missing or inactive" on the very first such block.
        # These are PLEXOS startup-staged / alternate-fuel-mode
        # variants that only become dispatchable mid-horizon; the
        # cap LHS slightly under-counts them but the constraint
        # still binds on the always-dispatchable variants.
        if g.pmax_profile and any(p <= 0.0 for p in g.pmax_profile):
            continue
        for fuel_name in g.fuel_names:
            fuel_to_gens.setdefault(fuel_name, []).append((g.name, g.heat_rate))

    # Per-block durations: when a block_layout is present, the
    # per-block hours sum to ``scope_hours``; otherwise assume
    # uniform 1h blocks across the full horizon.
    block_layout = bundle.block_layout if hasattr(bundle, "block_layout") else ()
    if block_layout:
        block_durations = tuple(float(len(intervals)) for intervals in block_layout)
        horizon_hours = float(sum(block_durations))
    else:
        block_durations = ()
        horizon_hours = float(24 * bundle.n_days)

    out: list[UserConstraintSpec] = []
    for fuel_name, (cap, scope_hours) in caps.items():
        gens = fuel_to_gens.get(fuel_name)
        if not gens:
            continue
        # Skip caps that would never bind: cap >= sum of every
        # generator's pmax × scope (i.e., the LP couldn't reach
        # the cap even running flat-out).  Saves LP rows.
        if cap >= 1e15:
            continue

        # Build LHS terms.  Skip generators without a positive
        # heat_rate (renewables / hydro with explicit heat=0).
        terms: list[str] = []
        for gname, heat in gens:
            coef = heat
            terms.append(
                _format_coefficient(coef, first=not terms)
                + f'generator("{gname}").generation'
            )
        if not terms:
            continue

        # Per-block RHS decomposition: each block carries
        # ``cap × duration / scope`` of the weekly/daily budget.
        if block_durations:
            rhs_profile = tuple(cap * (d / scope_hours) for d in block_durations)
            # The scalar baked into the expression is the average
            # (matches the rhs_profile when the layout is uniform).
            scalar_rhs = (
                cap
                * (horizon_hours / max(scope_hours, 1.0))
                / max(len(block_durations), 1)
            )
        else:
            rhs_profile = ()
            scalar_rhs = (
                cap
                * (horizon_hours / max(scope_hours, 1.0))
                / max(int(horizon_hours), 1)
            )

        # ``_format_coefficient`` already emits the inter-term `` + ``
        # / `` - `` prefix for non-first terms, so concat without
        # an extra separator.  Using ``" + ".join(terms)`` would
        # produce ``X +  + Y`` (double-plus) and break the parser.
        expression = "".join(terms) + f" <= {scalar_rhs:.6f}"
        out.append(
            UserConstraintSpec(
                name=f"FueMaxOff_{fuel_name}",
                expression=expression,
                rhs_profile=rhs_profile,
            )
        )
    if out:
        logger.info(
            "synthesised %d FueMaxOff* UserConstraint(s) from solution .accdb "
            "(weekly/daily fuel-offtake caps; per-block uniform decomposition)",
            len(out),
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
    # Extract waterways FIRST so we can discover any synthetic sink
    # junctions (1-ended PLEXOS forced-outflow waterways like
    # ``Filt_Colb`` / ``Riego_NoGen_Colbun``) and ``<name>_ocean``
    # spillway drains.  Both kinds of sink are emitted as Junction-only
    # nodes with ``drain = True`` — the gtopt LP only needs the drain
    # column on the Junction balance row, and a co-located zero-storage
    # Reservoir would just add a redundant balance equation and an
    # unconstrained ``vol_t`` variable.
    forced_waterway_targets: list[tuple[str, str, float]] = []
    # Drain configs harvested from ``Vert_*`` spillway arcs that the
    # extractor collapses onto the source storage's junction instead of
    # emitting a ``<src>_ocean`` Junction + connecting Waterway.  Maps
    # ``source_storage_name → {'drain_capacity', 'drain_cost'}``;
    # consumed by ``extract_junctions`` (extra_drain_configs kwarg)
    # below to set the new ``Junction.drain_capacity`` / ``drain_cost``
    # fields on the corresponding junction.
    junction_drain_configs: dict[str, dict[str, float | None]] = {}
    waterways = extract_waterways(
        db,
        bundle,
        forced_targets_out=forced_waterway_targets,
        junction_drain_configs_out=junction_drain_configs,
    )
    # GTOPT_RESERVOIR_SPILL=1 (--reservoir-spillway) shifts spillage
    # from the Junction.drain collapse onto Reservoir.spillway_cost=0.
    # The actual filtering of ``junction_drain_configs`` happens
    # AFTER reservoir demotion below (see the matching block inside
    # the ``if pondage_names:`` clause) — at that point we know which
    # reservoirs survived demotion and can selectively keep drain on
    # the demoted junctions (which need it for cascade pass-through)
    # while removing it from real reservoirs that now use spillway_cost.
    import os as _os_rs  # noqa: F401  used in the post-demotion block

    # Two distinct "extra junction" sources:
    #   1. PLEXOS Storage objects that ``extract_reservoirs``
    #      filtered out as pass-through nodes (B_*, Post_*, ISLA,
    #      LAJA_I, CURILLINQUE, etc.).  These need Junctions for
    #      waterway endpoints but NOT Reservoirs — emitting them
    #      as zero-storage Reservoirs would let the LP accumulate
    #      unbounded phantom volume (writer omits ``emax`` when 0).
    #   2. Synthetic sinks created by ``extract_waterways`` for 1-ended
    #      forced-outflow waterways (``<name>_sink``) and Vert_*
    #      spillway destinations (``<name>_ocean``).  These need
    #      Junctions with ``drain = True`` (handled by ``_is_sink_junction``
    #      inside ``extract_junctions``) but NOT Reservoirs.
    plexos_storage_names = {s.name for s in db.objects_of_class("Storage")}
    reservoir_names = {r.name for r in reservoirs}
    # ``_GNL_INF`` storages were already filtered from ``reservoirs`` by
    # ``extract_reservoirs`` (LNG gas-import accounting artifacts, not
    # water — see e763f39d1).  They have no Waterway / Turbine /
    # Generator memberships referencing them, so they don't need a
    # Junction either — emitting one leaves orphan nodes in
    # ``junction_array`` (the LP balance constraint then forces them
    # to receive zero flow forever, which is correct but wasteful).
    # Strip them here so they vanish from the topology entirely.
    dropped_passthrough = {
        n
        for n in (plexos_storage_names - reservoir_names)
        if not n.endswith("_GNL_INF")
    }
    # Synthetic sink endpoints (``<name>_sink`` for 1-ended forced-outflow
    # waterways, ``<name>_ocean`` for Vert_* spillways) are emitted as
    # Junction-only nodes with ``drain = True``.  A zero-storage Reservoir
    # at the same name would be pure LP overhead — its balance row is
    # redundant with the Junction's drain-enabled balance row, and the
    # extra ``vol_t`` variable just inflates the model (52 → 12 reservoirs
    # on CEN PCP).  Endpoints that are *not* sinks (would be a parser bug,
    # since extract_waterways only synthesises ``_sink`` / ``_ocean``
    # names) are logged and skipped — they would have created an orphan
    # node with no inflow source either way.
    sink_names_seen: set[str] = set()
    extra_sink_junctions: list[str] = []
    unexpected_endpoints: list[str] = []
    for ww in waterways:
        for endpoint in (ww.storage_from, ww.storage_to):
            if (
                endpoint
                and endpoint not in reservoir_names
                and endpoint not in plexos_storage_names
                and endpoint not in sink_names_seen
            ):
                sink_names_seen.add(endpoint)
                if _is_sink_junction(endpoint):
                    extra_sink_junctions.append(endpoint)
                else:
                    unexpected_endpoints.append(endpoint)
    if unexpected_endpoints:
        logger.warning(
            "extract_case: %d waterway endpoint(s) not in reservoir/storage "
            "tables and not recognised as sink/ocean names — skipped: %s",
            len(unexpected_endpoints),
            ", ".join(sorted(unexpected_endpoints)),
        )
    extra_junction_names = tuple(
        sorted(dropped_passthrough.union(extra_sink_junctions))
    )
    junctions = extract_junctions(
        reservoirs,
        extra_junction_names=extra_junction_names,
        drain_configs=junction_drain_configs or None,
    )
    known_junction_names = frozenset(j.name for j in junctions)
    reserves = extract_reserves(db, bundle)
    generators = extract_generators(db, bundle)
    fuels = extract_fuels(db, bundle)
    turbines = extract_turbines(db, bundle)

    # Drop turbines whose generator has ``pmax = 0`` (unit out of
    # service for the whole horizon — e.g. ``EL_TORO_U1`` in the
    # 2026-04-22 CEN PCP bundle).  The downstream writer would
    # otherwise synthesise an unbounded zero-cost penstock
    # ``penstock_<unit>`` that the LP exploits as a FREE drain pipe
    # from the upstream reservoir (gen col is skipped at every
    # block since pmax=0, so the ``gen = pf × flow`` equality is
    # never installed and the waterway's fmax=+inf carries any
    # volume the LP wants at zero cost).  Concretely: EL_TORO_U1
    # in v22 let the LP push ~12,000 hm³ from ELTORO into POLCURA
    # and downstream pass-through buffers, defeating the
    # discharge_ELTOROmax UC and producing the nphi_ELTORO
    # phantom-drain artefact.  Filtering at parse time keeps both
    # the turbine_array and the synthetic penstock_<unit>
    # waterway out of the JSON.
    def _gen_max_pmax(g: GeneratorSpec) -> float:
        if g.pmax_profile:
            return max(g.pmax_profile, default=0.0)
        return g.pmax or 0.0

    inactive_gens = frozenset(g.name for g in generators if _gen_max_pmax(g) <= 0.0)
    if inactive_gens:
        dropped_turbs = tuple(t for t in turbines if t.generator_name in inactive_gens)
        if dropped_turbs:
            logger.info(
                "extract_turbines: dropped %d turbine(s) whose generator "
                "has pmax = 0 across the horizon (would otherwise create "
                "an unbounded zero-cost penstock drain): %s",
                len(dropped_turbs),
                ", ".join(t.generator_name for t in dropped_turbs),
            )
        turbines = tuple(t for t in turbines if t.generator_name not in inactive_gens)

    # Demote pure-pondage / pure-tailrace Reservoirs to Junction-only.
    #
    # PLEXOS models every cascade balance / tailrace point as a Storage
    # object (B_C_Isla, B_M_Isla, B_Maule, Post_Antuco, Post_Isla, …),
    # even when the point has no storage capacity in real life — water
    # arrives and leaves within the same hour.  ``extract_reservoirs``
    # faithfully emits them as ReservoirSpec(eini=0, emin=0, emax=0, …),
    # but the resulting LP carries one redundant balance row and one
    # unbounded ``vol_t`` variable per block for each such "reservoir".
    # On the CEN PCP 2026-04-22 bundle that's 8 spurious storage
    # blocks (B_C_Isla, B_M_Isla, B_Maule, Post_Antuco, Post_Isla,
    # Post_Machicura, Post_Pangue, Post_Quilleco — every other unbounded
    # Storage is the head of a turbine and stays a Reservoir).
    #
    # A Reservoir qualifies for demotion when ALL of:
    #   - every volume / cost / penalty / profile field is zero / empty,
    #   - it is not a turbine ``main_reservoir`` reference (would break
    #     the turbine.head_storage lookup at JSON load),
    #   - it is not the PLEXOS 1e+30 ``never_drain`` sentinel.
    # ``tail_reservoir_name`` references stay valid because the writer
    # uses them only as a junction-name string for the synthetic
    # ``penstock_*`` waterway's ``junction_b``.  Waterway endpoint
    # references resolve through the Junction list — also unchanged.
    def _is_pure_pondage(r: ReservoirSpec) -> bool:
        # A reservoir qualifies for Junction-only demotion when its
        # storage envelope is entirely zero AND nobody attaches a
        # water-value / penalty / efin target to it.  We DO allow
        # demotion when the reservoir is still a turbine
        # ``main_reservoir`` reference, because the writer will
        # synthesise a Junction with the same name (extract_junctions
        # adds ``extra_junction_names`` below) and the turbine's
        # head_storage lookup resolves through that Junction.  This
        # cleans up degenerate run-of-river plants (ISLA, LA_MINA,
        # LAJA_I, LOMAALTA, RUCUE, QUILLECO, CURILLINQUE,
        # SANIGNACIO on CEN PCP) that currently emit a dummy
        # ``vol[t] = 0`` reservoir balance row per block — pure LP
        # overhead with no information.  ``eini = 0`` is the
        # discriminator: real reservoirs with any initial volume
        # (PANGUE 773, MACHICURA 152, POLCURA 7) stay Reservoirs even
        # if their other fields are zero.
        return (
            r.eini == 0.0
            and r.emin == 0.0
            and r.emax == 0.0
            and r.efin == 0.0
            and r.water_value == 0.0
            and r.spill_penalty_per_mwh == 0.0
            and not r.never_drain
            and not r.emin_profile
            and not r.emax_profile
            and not r.inflow_profile
        )

    pondage_names = tuple(sorted(r.name for r in reservoirs if _is_pure_pondage(r)))
    if pondage_names:
        logger.info(
            "extract_case: demoted %d pondage/tailrace Reservoir(s) to "
            "Junction-only (no bounds, no water-value, not referenced "
            "as a turbine main_reservoir): %s",
            len(pondage_names),
            ", ".join(pondage_names),
        )
        reservoirs = tuple(r for r in reservoirs if r.name not in pondage_names)
        extra_junction_names = tuple(
            sorted(set(extra_junction_names).union(pondage_names))
        )
        # Re-apply GTOPT_RESERVOIR_SPILL: discard Junction.drain for
        # the FINAL real reservoir set (post-demotion).  The earlier
        # discard inside this block was overly broad — it removed
        # drain from all reservoirs including the to-be-demoted ones,
        # making the LP infeasible at demoted junctions that need the
        # drain to dispose of cascade water arriving when turbines are
        # capped.  Here we know the survivors.
        if _os_rs.environ.get("GTOPT_RESERVOIR_SPILL", "0").lower() in (
            "1",
            "true",
            "yes",
        ):
            real_reservoir_names = {r.name for r in reservoirs}
            # ``junction_drain_configs`` was already touched by the
            # earlier block — restore it from PLEXOS and re-filter
            # against the post-demotion set.  Simplest: rebuild from
            # nothing here using the same logic.
            # Actually we just need to ensure none of the survivors
            # have drain.  The demoted junctions keep drain.
            for n in list(junction_drain_configs.keys()):
                if n in real_reservoir_names:
                    junction_drain_configs.pop(n, None)
        junctions = extract_junctions(
            reservoirs,
            extra_junction_names=extra_junction_names,
            drain_configs=junction_drain_configs or None,
        )
        known_junction_names = frozenset(j.name for j in junctions)

    # PLEXOS Region.VoLL → demand_fail_cost.
    #
    # Per-Region VoLLs are routed onto each ``Demand.fcost`` (via
    # ``_bus_to_region_voll`` inside ``extract_demands``), preserving
    # the per-Region granularity PLEXOS expresses.  This global value
    # only serves as the fallback for Demands whose bus has no Region
    # or whose Region has no VoLL — pick the MIN across all
    # known VoLLs so the fallback errs on the conservative side
    # (cheaper curtailment ⇒ LP can curtail; the per-Region override
    # raises the price back to the right level for matched Demands).
    # Prior behaviour was ``max(voll_values)``, which the literature
    # audit flagged (2026-05-20) as overpricing curtailment in
    # cheaper regions when VoLLs differed.
    bundle_spec = extract_bundle_spec(bundle)
    voll_values = []
    for region in db.objects_of_class("Region"):
        v = db.static_property("Region", region.object_id, "VoLL")
        if v and v > 0.0:
            voll_values.append(v)
    if voll_values:
        chosen = min(voll_values)
        if len(set(voll_values)) > 1:
            logger.info(
                "PLEXOS Regions ship %d distinct VoLL values "
                "(min=%.2f, max=%.2f).  Per-Region values land on "
                "each Demand's `fcost`; the global default falls "
                "back to the min (%.2f) for Demands without a "
                "matched Region.",
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
            n_days=bundle_spec.n_days,
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
        waterways=waterways,
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
        flow_rights=_synthesise_pinned_flow_rights(
            forced_waterway_targets, known_junction_names
        )
        + extract_flow_rights(bundle, turbines, known_junction_names),
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
    pmax_profiles_by_gen = {
        g.name: g.pmax_profile for g in case.generators if g.pmax_profile
    }
    base_ucs = extract_user_constraints(
        db,
        bundle,
        emitted_names=emitted_names,
        heat_rate_by_gen=heat_rate_by_gen,
        pmax_by_gen=pmax_by_gen_for_uc,
        pmax_profiles_by_gen=pmax_profiles_by_gen,
    )
    hydro_ucs = extract_hydro_discharge_user_constraints(
        db, bundle, case.turbines, case.generators
    )
    # Solution-side FueMaxOff* weekly/daily caps.  Synthesised only
    # when the .accdb cache is available (``--horizon-mode plexos``
    # branch caches it before ``extract_case`` runs); otherwise this
    # extractor returns an empty tuple and nothing is appended.
    fuel_offtake_ucs = _build_fuel_offtake_caps_ucs(bundle, case)
    case = dataclasses.replace(
        case,
        user_constraints=tuple(base_ucs) + tuple(hydro_ucs) + tuple(fuel_offtake_ucs),
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
