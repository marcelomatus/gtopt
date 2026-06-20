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

import csv
import dataclasses
import difflib
import functools
import logging
import math
import re
from collections.abc import Callable
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Any

from .entities import (
    BatterySpec,
    BoundaryCutSpec,
    BundleSpec,
    CommitmentSpec,
    ConstraintDirective,
    DecisionVariableSpec,
    DemandSpec,
    FlowRightSpec,
    FlowSpec,
    FuelSpec,
    GeneratorSpec,
    JunctionSpec,
    LineSpec,
    NodeSpec,
    PlantSpec,
    PlexosCase,
    ReservoirSpec,
    ReserveProvisionSpec,
    ReserveSpec,
    TurbineSpec,
    UserConstraintSpec,
    UserConstraintStats,
    WaterwaySpec,
)
from . import _uc_policy
from .plexos_csv import DEFAULT_PERIODS, read_long, read_wide
from .plexos_loader import PlexosBundle
from .plexos_xml import PlexosDataRow, PlexosDb, PlexosObject, load_xml


logger = logging.getLogger(__name__)


class UnresolvedConstraintReferenceError(RuntimeError):
    """A UserConstraint term references an element gtopt never emits.

    Raised by :func:`extract_user_constraints` after processing ALL
    constraints when one or more direct-coefficient terms point at an
    element name that is not in the emitted-name set and could not be
    reconciled (e.g. the BESS zone-suffix Fix).  The converter MUST
    fail loudly — analogous to gtopt's strict JSON parser erroring on
    an unknown field — rather than silently dropping the term or the
    whole constraint.  The message lists every offending reference with
    the closest emitted name as a hint.
    """


# PLEXOS Sense → gtopt operator.
#
# Conventions used in CEN PCP DBSEN_PRGDIARIO.xml (verified 2026-05-30
# by name+behaviour audit of all explicit-Sense constraints in
# RES20260422.accdb):
#   * Sense = -1 → ``<=`` (cap)    — 902 constraints, mostly line
#     security (``SD_*`` and dated numeric forms)
#   * Sense =  0 → ``>=`` (MIN)    — 20 constraints, all of which are
#     "MinProvision" reserve-aggregation rows (``CPF_DownMinProvision``,
#     ``CSF_UpMinProvision``, ``CTF_DownMinProvision``, ``CPFN_*``,
#     ``CPF_BESS_*``) — physically a lower bound on Σ provisions
#   * Sense =  1 → ``>=`` (MIN)    — 157 constraints named ``*min`` /
#     ``*eco`` / similar (``ANTUCOmin``, ``ANGOSTURAmin``, ``ANGOSTURAeco``)
#   * Sense unset → ``=`` (definitional eq) — see fall-through at
#     ``sense_val is None`` further down; covers ``MACHICURA_GENT4def``,
#     ``CSF_LW_Def``, etc. where the constraint DEFINES a variable
#
# Previously Sense=0 mapped to ``=`` (equality), which made the
# ``*MinProvision`` family infeasible when hardened — the LP couldn't
# satisfy ``Σ provisions = RHS`` exactly when other per-generator
# reserve-cap rows pinned individual provisions.  PLEXOS solves them
# at the lower bound naturally (Σ provisions ≥ RHS, LP minimises cost
# → equality at the optimum) without the data-fit risk an equality
# would impose at LP-decision time.
_SENSE_OP = {-1.0: "<=", 1.0: ">=", 0.0: ">="}

# Reservoirs whose drain (spill) variable is disabled (``spillway_capacity =
# 0``) so water leaves only through turbines.  Reserved for ELTORO — the head
# of the Laja cascade, which PLEXOS forces to refill and never spills.
_NEVER_DRAIN_RESERVOIRS = frozenset({"ELTORO"})


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
    # GENERIC Battery reserve provision (no Raise/Lower in the kind).  The
    # accessor is the empty-string sentinel ``""`` rather than a guessed
    # ``up``: a battery reference always routes through the zone-suffix
    # reconciliation (``_bess_provision_direction`` /
    # ``_bess_matching_provisions``), where ``""`` signals "direction is
    # NOT carried by the coefficient kind — take it from the CONSTRAINT
    # NAME" (``*_LW*`` / ``*Down*`` → dn, ``*_RS*`` / ``*Up*`` → up).
    # The sentinel never leaks into an emitted ``var_ref`` because the
    # bare ``provision_<bat>`` it would produce is never in
    # ``emitted_names`` (the SSCC emitter only ships zone-suffixed names).
    (
        "Battery",
        "Constraints",
        "Reserve Provision Coefficient",
        "reserve_provision",
        "",
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
    # Battery commitment-unit coefficients.  gtopt's ``expand_batteries``
    # creates a ``uc_<bat>_gen`` Commitment UNCONDITIONALLY for every
    # battery (relaxed continuous ``u`` when the battery carries no
    # commitment economics — see ``source/system.cpp``), exposing
    # ``.status`` / ``.startup`` / ``.shutdown`` on the synthetic
    # ``<bat>_gen`` discharge generator.  PLEXOS Battery
    # ``Units Generating / Started / Shutdown Coefficient`` rows
    # therefore route to ``commitment("uc_<bat>_gen").<attr>`` via the
    # ``uc_{name}_gen`` template (the ``Reserve Units Coefficient`` on a
    # Battery is handled separately by the
    # ``forward_to_battery_gen_commit`` derived mode below — kept there
    # so the gtopt aliasing rationale stays adjacent to it).  These
    # property kinds are absent from the CEN PCP DB today; wired for
    # robustness so a future export resolves them instead of failing.
    (
        "Battery",
        "Constraints",
        "Units Generating Coefficient",
        "commitment",
        "status",
        "uc_{name}_gen",
    ),
    (
        "Battery",
        "Constraints",
        "Units Started Coefficient",
        "commitment",
        "startup",
        "uc_{name}_gen",
    ),
    (
        "Battery",
        "Constraints",
        "Units Shutdown Coefficient",
        "commitment",
        "shutdown",
        "uc_{name}_gen",
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


# ── BESS reserve-provision name reconciliation ──────────────────────────
# The SSCC BESS emitter (``extract_sscc_bess_provisions``) names each
# battery's reserve provisions with a ZONE suffix —
# ``provision_<bat>_gen__<ZONE>`` where ``<ZONE>`` is the ``*_BESS``
# reserve (``CSF_LW_BESS`` / ``CSF_RS_BESS`` / ``CPF_LW_BESS`` /
# ``CPF_RS_BESS``).  A PLEXOS Battery reserve-provision coefficient,
# however, references the BARE battery name, which the direct-
# coefficient builder maps to the plain (never-emitted)
# ``provision_<bat>``.  Rather than silently dropping the term we route
# it to EVERY zone-suffixed provision the SSCC emitter actually
# produced that matches the coefficient's DIRECTION (``.up`` /
# ``.dn``) and the constraint's reserve TYPE (CPF / CSF / both).  This
# generalises the original per-battery ``CSF_LW_MIN_BAT_<bat>`` prefix
# fix to the aggregate ``CPF_*MinProvision`` / ``UP/DOWNStorageBound_*``
# constraints, which carry the direction in the coefficient KIND
# (``Regulation Raise`` / ``Lower``) rather than a name prefix.

#: Zone suffix carried by each up/down BESS reserve, by reserve type.
#: ``provision_<bat>_gen__<TYPE>_<DIR>_BESS``.
_BESS_ZONE_UP_SUFFIX = "_RS_BESS"
_BESS_ZONE_DN_SUFFIX = "_LW_BESS"


def _bess_provision_direction(constraint_name: str, accessor: str) -> str | None:
    """Resolve the BESS provision direction (``"up"`` / ``"dn"``).

    The coefficient KIND is authoritative: a ``Regulation Raise`` /
    ``Raise`` / ``*Raise*`` coefficient already arrives with
    ``accessor == "up"`` and a ``Lower`` one with ``accessor == "dn"``
    (see ``_DIRECT_COEFFS``).  When the kind is the generic
    ``Reserve Provision Coefficient`` (which the direct builder maps to
    the placeholder ``"up"``), the constraint NAME breaks the tie:
    ``*_LW*`` / ``*Down*`` → ``"dn"``; ``*_RS*`` / ``*Up*`` → ``"up"``.
    Returns ``None`` when neither the kind nor the name carries a
    recognisable direction.
    """
    if accessor in ("up", "dn"):
        return accessor
    upper = constraint_name.upper()
    if "_LW" in upper or "DOWN" in upper:
        return "dn"
    if "_RS" in upper or "UP" in upper:
        return "up"
    return None


def _bess_provision_zone_types(constraint_name: str) -> tuple[str, ...]:
    """Reserve TYPE filter (``CPF`` / ``CSF``) from the constraint name.

    A name containing ``CPF`` restricts to ``CPF_*`` zones; ``CSF`` to
    ``CSF_*`` zones.  When neither token appears (e.g.
    ``UP/DOWNStorageBound_BAT_*``) BOTH types are eligible — the term
    is a legitimate SUM over the matching-direction zones of both types.
    """
    upper = constraint_name.upper()
    types: list[str] = []
    if "CPF" in upper:
        types.append("CPF")
    if "CSF" in upper:
        types.append("CSF")
    return tuple(types) if types else ("CPF", "CSF")


def _bess_matching_provisions(
    bat_name: str,
    direction: str,
    constraint_name: str,
    allowed_ref: frozenset[str] | None,
) -> list[str]:
    """Zone-suffixed SSCC provisions a battery term should route to.

    Builds the candidate ``provision_<bat>_gen__<TYPE>_<DIR>_BESS`` names
    for every reserve TYPE permitted by the constraint name and the given
    DIRECTION, then keeps only the names the SSCC emitter actually
    produced (``allowed_ref``).  An empty result means the battery is not
    SSCC-eligible for that direction → genuinely unmappable.
    """
    dir_suffix = _BESS_ZONE_UP_SUFFIX if direction == "up" else _BESS_ZONE_DN_SUFFIX
    out: list[str] = []
    for ztype in _bess_provision_zone_types(constraint_name):
        ref = f"provision_{bat_name}_gen__{ztype}{dir_suffix}"
        if allowed_ref is None or ref in allowed_ref:
            out.append(ref)
    return out


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


#  Retired 2026-05-23: `_patch_uncapped_zero_fuel_bands` + the
#  `_FUEL_BAND_SUFFIX_RE` regex used to live here.  They were a
#  band-aid for the missing ``FueMaxOffWeek_<fuel>`` constraints —
#  promoting zero-priced fuel bands to a worst-case sibling price so
#  the LP wouldn't discover them as free fuel arbitrage.  With
#  ``Fuel.max_offtake`` (PR #487) we now read the real weekly caps
#  from ``Fuel_MaxOfftakeWeek.csv`` and emit them as a per-stage cap
#  row on the gtopt Fuel element.  See
#  ``_extract_fuel_max_offtake_week`` below.


#: PLEXOS-CEN FueMaxOffWeek_* — units + semantics.
#:
#: 1. **Units**: ``Fuel_MaxOfftakeWeek.csv`` ships the cap in
#:    **TJ/week**, while ``Gen_HeatRate.csv`` ships heat rates in
#:    **GJ/MWh**.  Verified against the PLEXOS solution ``.accdb``:
#:    the constraint's t_data RHS = CSV × 1000 = total weekly cap
#:    in GJ (for ``FueMaxOffWeek_Gas_NuevaRenca_GN_A``: CSV 9.8 TJ
#:    matches the per-period RHS 58.33 GJ × 168 hours = 9800 GJ).
#:    → we multiply the CSV value by ``_TJ_TO_GJ`` (1000) when
#:    emitting to gtopt so the cap units match the LHS basis.
#:
#: 2. **Semantics**: PLEXOS enforces the cap **per period** (the
#:    weekly cap distributed as a uniform per-hour rate, then
#:    multiplied by each period's duration).  Evidence: the
#:    constraint's RHS in t_data ships uniformly as 58.33 across
#:    all 111 periods, and Activity_p values per period vary but
#:    never exceed RHS_p — consistent with `Activity_p ≤ RHS_p`
#:    per-period enforcement.
#:    → we set ``Fuel.max_offtake_per_block = True`` so gtopt's
#:    ``FuelLP`` builds one cap row per (scenario, stage, block),
#:    pro-rating the weekly cap by block duration share.  Matches
#:    PLEXOS's per-period semantics.
_TJ_TO_GJ = 1000.0


def _extract_fuel_max_offtake_week(
    bundle: PlexosBundle,
    horizon_start: datetime | None = None,
) -> dict[str, float]:
    """Read ``Fuel_MaxOfftakeWeek.csv`` → ``{fuel_name: cap_for_horizon_GJ}``.

    Each row carries ``NAME,YEAR,MONTH,DAY,PERIOD,VALUE`` where the
    ``(YEAR, MONTH, DAY)`` triple is the **week-start date** (PLEXOS
    convention) and ``VALUE`` is the cap for that week in **TJ/week**.
    CEN PCP daily bundles typically ship 1-2 weekly rows per fuel band;
    PLEXOS itself enforces the weekly cap **per period**, switching the
    per-hour rate at the calendar week boundary inside the 7-day
    horizon (verified on RES20260422.accdb: ``Gas_Colbun_GN_B`` per-
    period RHS = 12.5 GJ/h for the 24 day-1 hourly periods (week
    04-16's 2.1 TJ / 168 h) and 27.38 GJ/h for the 87 day-2-to-7
    aggregated periods (week 04-23's 4.6 TJ / 168 h)).

    gtopt's ``Fuel.max_offtake`` is a **single horizon-wide budget**
    (gtopt pro-rates per block by duration share — see
    ``max_offtake_per_block = True`` on the writer side).  To match
    PLEXOS's effective horizon-wide budget exactly we compute the
    time-weighted sum: each week contributes
    ``cap_week_GJ × overlap_days / 7`` where ``overlap_days`` is the
    number of horizon days falling inside ``[week_start, week_start + 6]``.

    Example (DATOS20260422, horizon Apr 22-28 = 7 days,
    ``Gas_Colbun_GN_B`` rows ``[2026-04-16: 2.1 TJ, 2026-04-23: 4.6 TJ]``):
    week 04-16 covers Apr 16-22 (1 horizon day = Apr 22) → 2100 × 1/7
    = 300 GJ; week 04-23 covers Apr 23-29 (6 horizon days) → 4600 × 6/7
    = 3943 GJ; total = **4243 GJ** — matches PLEXOS's ``Σ_period
    duration × RHS_period`` to the cent (vs the legacy first-row bug
    which returned 2100 GJ, starving NEHUENCO_1-TG+TV in the gtopt MIP).

    When ``horizon_start`` is ``None`` (or the bundle's horizon length is
    unknown), fall back to the legacy first-row behaviour.

    The TJ→GJ conversion uses ``_TJ_TO_GJ`` so the cap units match the
    per-block LHS basis (heat_rate × MWh, where PLEXOS heat_rate is in
    GJ/MWh).

    Returns a ``{name: cap_GJ_for_horizon}`` dict.  Fuels absent from
    the CSV are absent from the dict (= no cap).  Fuels with an
    explicit 0 cap in EVERY overlapping week land in the dict with
    value 0.0 — PLEXOS uses this to "shut" a band on a given week.
    """
    csv_name = "Fuel_MaxOfftakeWeek.csv"
    if not bundle.has(csv_name):
        return {}

    ref_date: date | None = horizon_start.date() if horizon_start else None
    horizon_days = max(int(bundle.n_days) or 1, 1)

    rows_by_fuel: dict[str, list[tuple[date, float]]] = {}
    with bundle.csv(csv_name).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames:
            reader.fieldnames = [h.lstrip("﻿") for h in reader.fieldnames]
        for row in reader:
            name = (row.get("NAME") or "").strip()
            if not name:
                continue
            try:
                wk = date(
                    int(row.get("YEAR", "") or 0),
                    int(row.get("MONTH", "") or 0),
                    int(row.get("DAY", "") or 0),
                )
                val = float(row.get("VALUE", "0") or "0")
            except (ValueError, KeyError):
                continue
            rows_by_fuel.setdefault(name, []).append((wk, val))

    if ref_date is None:
        # Legacy fallback: first CSV row's value (raw TJ → GJ).
        return {
            name: candidates[0][1] * _TJ_TO_GJ
            for name, candidates in rows_by_fuel.items()
            if candidates
        }

    # Horizon coverage [ref_date, ref_date + horizon_days - 1].  Each
    # week_start row covers [week_start, week_start + 6].  Overlap days
    # weight each week's contribution; rows with zero overlap drop out.
    horizon_last = ref_date + timedelta(days=horizon_days - 1)
    out: dict[str, float] = {}
    for name, candidates in rows_by_fuel.items():
        total_gj = 0.0
        for wk_start, cap_tj in candidates:
            wk_last = wk_start + timedelta(days=6)
            overlap_lo = max(wk_start, ref_date)
            overlap_hi = min(wk_last, horizon_last)
            overlap_days = (overlap_hi - overlap_lo).days + 1
            if overlap_days <= 0:
                continue
            total_gj += cap_tj * _TJ_TO_GJ * overlap_days / 7.0
        out[name] = total_gj
    return out


#: PLEXOS ``Fuel.Min Offtake`` family — pids 595-600 plus the
#: ``Min Offtake Penalty`` pid 602 (default ``$1000/fuel-unit``
#: when the bundle ships an offtake floor without an explicit
#: penalty — PLEXOS soft-by-default; the converter mirrors that
#: idiom on the gtopt-native "unset ⇒ hard" model).
#:
#: Each property is a CUMULATIVE total over the named window in the
#: fuel's native unit.  We fold all six period flavours into a single
#: horizon-wide ``min_offtake`` scalar (matching the way
#: ``max_offtake`` folds ``Fuel_MaxOfftakeWeek.csv`` rows): each
#: variant's contribution is its value × the number of windows of
#: that length contained in the bundle horizon, then summed.
#:
#: Across the 14 cached CEN PCP bundles (2025-10 → 2026-05) **zero**
#: fuels populate ANY of these properties — the entire family is
#: dormant in current CEN cases.  The parser is defensive plumbing:
#: it logs a WARNING when a non-zero Min Offtake property is found
#: so the first real CEN bundle to ship one surfaces loudly rather
#: than being silently dropped.
_MIN_OFFTAKE_PROPS_AND_HOURS: tuple[tuple[str, float | None], ...] = (
    # Property name in PLEXOS,        nominal window length (hours)
    ("Min Offtake", None),  # bare: per-simulation-interval; unknown
    ("Min Offtake Hour", 1.0),  # one window per hour
    ("Min Offtake Day", 24.0),
    ("Min Offtake Week", 168.0),
    ("Min Offtake Month", 730.0),  # 30.42 d × 24 h, calendar-month avg
    ("Min Offtake Year", 8760.0),
)
_MIN_OFFTAKE_PENALTY_PROP = "Min Offtake Penalty"
#: PLEXOS-faithful soft-by-default penalty when bundle ships a Min
#: Offtake floor without an explicit ``Min Offtake Penalty``.  Matches
#: PLEXOS pid 602 default; gtopt itself has no such default, so the
#: converter must inject it explicitly to preserve PLEXOS economics.
_PLEXOS_DEFAULT_MIN_OFFTAKE_PENALTY = 1000.0


def _extract_fuel_min_offtake_horizon(
    db: PlexosDb,
    fuel_object_id: int,
    fuel_name: str,
    horizon_hours: float,
) -> tuple[float | None, float | None]:
    """Return ``(min_offtake_for_horizon, min_offtake_cost)`` for one fuel.

    Reads the PLEXOS Min Offtake family (pids 595-600) and folds each
    populated variant into the same horizon-wide budget that
    ``Fuel.min_offtake`` represents on the gtopt side.  Returns
    ``(None, None)`` when the entire family is unset for this fuel
    (the common case across the cached CEN PCP archive).

    The cost return value applies the PLEXOS soft-by-default idiom:
    if any Min Offtake property is set but ``Min Offtake Penalty`` is
    NOT explicitly set, returns ``_PLEXOS_DEFAULT_MIN_OFFTAKE_PENALTY``
    so the gtopt LP model preserves PLEXOS economics.  If the bundle
    explicitly ships a penalty (including 0 or a negative value), that
    value is returned verbatim and the caller's normalisation rules
    apply.
    """
    total: float | None = None
    any_set = False
    for prop_name, window_hours in _MIN_OFFTAKE_PROPS_AND_HOURS:
        raw = db.static_property("Fuel", fuel_object_id, prop_name)
        if raw == 0.0:
            continue
        any_set = True
        if total is None:
            total = 0.0
        # Convert the per-window cumulative value to a horizon-wide
        # contribution.  ``window_hours = None`` (the bare property)
        # is treated as "per simulation interval"; without knowing
        # the period count we conservatively use the bundle horizon
        # directly so the floor binds at the same magnitude PLEXOS
        # would report.  Pro-rating for shorter windows divides the
        # per-window total across windows that overlap the horizon.
        if window_hours is None or window_hours <= 0.0:
            contrib = raw
        else:
            windows_in_horizon = max(1.0, horizon_hours / window_hours)
            contrib = raw * windows_in_horizon
        total += contrib
        logger.warning(
            "Fuel %r: Min Offtake property %r populated (raw=%g, "
            "horizon contribution=%g %s units) — this is the first "
            "CEN bundle to ship a non-zero Min Offtake; gtopt floor "
            "wiring is new, please cross-check the LP-side enforcement.",
            fuel_name,
            prop_name,
            raw,
            contrib,
            "fuel",
        )

    if not any_set:
        return (None, None)

    # Penalty side — PLEXOS soft-by-default at $1000/fuel-unit.
    explicit_penalty = db.static_property(
        "Fuel", fuel_object_id, _MIN_OFFTAKE_PENALTY_PROP
    )
    # ``static_property`` returns 0.0 when the property is undefined
    # for this object (the System collection ships the class-wide
    # default which we treat as "no override").  Distinguishing
    # "explicit 0" from "unset" requires probing ``data_for``
    # directly; for the PLEXOS-default-1000 idiom the conservative
    # rule is "treat 0 as unset" — a literal 0 penalty on a Min
    # Offtake floor means the floor is unenforced anyway.
    cost = (
        explicit_penalty
        if explicit_penalty != 0.0
        else _PLEXOS_DEFAULT_MIN_OFFTAKE_PENALTY
    )
    return (total, cost)


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

    # Weekly offtake caps from ``Fuel_MaxOfftakeWeek.csv`` — the
    # missing piece that PREVIOUSLY required the
    # ``_patch_uncapped_zero_fuel_bands`` band-aid (now retired).
    # PLEXOS's ``FueMaxOffWeek_<fuel>`` Constraint binds total weekly
    # fuel consumption per band; gtopt's ``Fuel.max_offtake`` field
    # (landed in PR #487) is the corresponding LP row.
    max_offtake_week = _extract_fuel_max_offtake_week(bundle, db.horizon_start)

    # Bundle horizon length in hours — used to fold the
    # ``Fuel.Min Offtake {Hour, Day, Week, Month, Year}`` family into
    # a single horizon-wide budget.  ``bundle.n_days`` defaults to 7
    # on CEN PCP weekly bundles; treat 0 / unknown as 168 h so the
    # parser never divides by zero.
    horizon_hours = float(max(int(bundle.n_days) or 7, 1) * 24)

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
        price_profile: tuple[float, ...] = ()
        if fuel.name in prices:
            series = prices[fuel.name]
            price = series[0]
            # gtopt ``Fuel.price`` is now per-(stage, block)
            # (``OptTBRealFieldSched``), so a genuinely time-varying
            # ``Fuel_Price.csv`` series is no longer collapsed to the
            # period-1 scalar — the full profile is carried through to
            # the writer, which aggregates it to the block layout and
            # emits a ``[[stage_blocks]]`` matrix when it varies.  The
            # scalar ``price`` is kept as the constant / fallback value.
            price_profile = tuple(series)
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

        # PLEXOS ``Heat Content`` (Fuel property, GJ per fuel-unit).
        # When present, this gives the converter the GJ/fuel-unit
        # multiplier needed to derive Fuel.emission_factors from per-
        # GJ combustion rates AND to convert startup-cost $ → tCO2eq
        # under ``--only-emissions``.  Absent on most CEN PCP fuels
        # (the IPCC fallback in ``apply_emission_defaults_from_file``
        # fills it from the canonical family lookup); read here so
        # source-supplied values pass through unchanged and downstream
        # tools work even under ``--no-emissions``.
        heat_content = db.static_property("Fuel", fuel.object_id, "Heat Content")
        if heat_content is None or heat_content == 0.0:
            # Some schemas spell it "Heat Rate" on Fuel (different from
            # Generator.Heat Rate which is fuel-unit/MWh).  Try the
            # synonym before falling back to the IPCC default downstream.
            alt = db.static_property("Fuel", fuel.object_id, "Heat Rate")
            if alt and float(alt) > 0.0:
                heat_content = alt

        # Emission→Fuel membership path (preferred when present).
        co2_mem, co2_up_mem = membership_rates.get(fuel.object_id, (0.0, 0.0))
        co2_rate = co2_mem if co2_mem != 0.0 else co2_direct
        co2_upstream_rate = co2_up_mem if co2_up_mem != 0.0 else co2_up_direct

        # Weekly offtake cap for the binding week.  Absent from the
        # CSV → ``None`` (no cap); explicit 0 in the CSV → 0.0
        # (band shut on this week).
        cap_week = max_offtake_week.get(fuel.name)

        # Min Offtake floor + PLEXOS soft-by-default penalty
        # translation.  Returns ``(None, None)`` when the entire
        # Min Offtake family is unset for this fuel — the case for
        # every CEN PCP bundle in the 2025-10..2026-05 cache.
        min_off, min_off_cost = _extract_fuel_min_offtake_horizon(
            db, fuel.object_id, fuel.name, horizon_hours
        )

        # Classify the fuel into the canonical family taxonomy shared
        # with ``fuel_family_of_generator`` — keeps the orphan-recovery
        # sibling matcher and the published ``Fuel.type`` aligned by
        # construction.  ``"other"`` falls back when the bundle ships a
        # fuel whose name doesn't match any CEN prefix family (none
        # observed in the 2025-10 → 2026-05 PCP cache, but the default
        # keeps the field non-null for hand-rolled / 118-Bus fuels).
        type_tag = fuel_family_of_fuel(fuel.name) or FUEL_FAMILY_OTHER
        subtype = _fuel_subtype_from_name(fuel.name)
        out.append(
            FuelSpec(
                object_id=fuel.object_id,
                name=fuel.name,
                price=price,
                price_profile=price_profile,
                heat_content=float(heat_content or 0.0),
                co2_rate=co2_rate,
                co2_upstream_rate=co2_upstream_rate,
                max_offtake=cap_week,
                min_offtake=min_off,
                min_offtake_cost=min_off_cost,
                type_tag=type_tag,
                subtype=subtype,
            )
        )
    return tuple(out)


def _fuel_subtype_from_name(name: str) -> str:
    """Derive an IPCC sub-grade hint from a PLEXOS Fuel object name.

    PLEXOS Fuel objects don't carry a structured sub-type property in
    the CEN-Chile schema (verified on 2026-04-12 PCP bundle: 221 Fuels
    × 76 distinct properties — none semantically a sub-type).  The
    sub-grade signal lives only in the name itself for the cases
    where IPCC families split into sub-grades that matter for CO2:

    * ``Gas_*_GN_*`` — pipeline natural gas (low upstream factor).
    * ``Gas_*`` (no ``_GN_``) — LNG (real upstream chain
      6-15 kg CO2/GJ from liquefaction + tanker + regas).

    The empty string means "no project-specific subtype detected" —
    the emission lookup falls back to the family alias in the IPCC
    defaults.  Coal is intentionally NOT subtyped here even though
    Chile burns sub-bituminous: that choice belongs in a
    project-specific overrides file (``share/gtopt/emissions/cen_chile.json``),
    not in the generic plexos2gtopt parser — other PLEXOS markets
    use the same ``Carbon_*`` naming convention with different coal
    grades.

    Returns the IPCC canonical sub-grade name (e.g. ``"natural_gas"``,
    ``"lng"``).  Empty string for unrecognized / ambiguous cases.
    """
    if not name:
        return ""
    # Gas_*_GN_* — pipeline natural gas (distinct from LNG bands).
    # The "_GN_" infix marker is the CEN convention for pipeline tier.
    if name.startswith("Gas_") and "_GN_" in name:
        return "natural_gas"
    # Gas_* without _GN_ — defaults to LNG (CEN combined-cycle bands
    # like Gas_GNLQuintero_A, Gas_EnelMejillones_B, etc.).
    if name.startswith("Gas_"):
        return "lng"
    # All other families (Carbon_*, Diesel_*, FuelOil_*, Biomasa_*,
    # Biogas_*, GLP_*, Petcoke_*, Otros_*) stay untyped — the
    # emission lookup's family-prefix fallback gets them right
    # (Carbon_* → coal_bituminous via "carbon" alias, etc.).
    # Project-specific overrides (Andean coal = sub-bituminous, etc.)
    # belong in share/gtopt/emissions/cen_chile.json.
    return ""


# ----------------------------------------------------------------------
# Canonical fuel-family tags
# ----------------------------------------------------------------------
#
# A single source of truth for "what kind of fuel is this?", usable
# uniformly across:
#
# * The **generator** side — PLEXOS encodes the fuel family in the
#   trailing segment of the Generator name (``UJINA_U5_DIE`` → diesel,
#   ``ATA-TG1A_GNL_X`` → gas).  ``fuel_family_of_generator`` returns
#   the canonical tag.
# * The **fuel** side — PLEXOS encodes the fuel family in the prefix
#   of the Fuel object name (``Diesel_Collahuasi`` → diesel,
#   ``Gas_Kelar_A`` → gas).  ``fuel_family_of_fuel`` returns the same
#   canonical tag for the matching family.
# * The **JSON output** — ``FuelSpec.type_tag`` is populated in
#   :func:`extract_fuels` and surfaces as ``Fuel.type`` in
#   ``build_fuel_array`` (gtopt-side field documented at
#   ``include/gtopt/fuel.hpp:126`` as "Optional element type/category
#   tag").  Mirrors the ``Generator.type = "thermal" | "renewable"``
#   convention already established for Generators.
#
# Tag values are stable lowercase strings matching the standard
# commodity vocabulary: ``"diesel"``, ``"fuel_oil"``, ``"gas"``,
# ``"glp"``, ``"biomasa"``, ``"biogas"``, ``"carbon"``, ``"otros"``.
# ``"other"`` is the conventional fallback for unrecognised names —
# kept distinct from ``"otros"`` (CEN's "Otros_*" residual category)
# so downstream consumers can tell "we tried and failed to classify"
# apart from "PLEXOS itself labelled this as residual".

FUEL_FAMILY_DIESEL = "diesel"
FUEL_FAMILY_FUEL_OIL = "fuel_oil"
FUEL_FAMILY_GAS = "gas"
FUEL_FAMILY_GLP = "glp"
FUEL_FAMILY_BIOMASA = "biomasa"
FUEL_FAMILY_BIOGAS = "biogas"
FUEL_FAMILY_CARBON = "carbon"
FUEL_FAMILY_OTROS = "otros"
FUEL_FAMILY_OTHER = "other"

#: Renewable / non-fuel primary-energy tags.  Shared with the
#: thermal-side ``FUEL_FAMILY_*`` namespace so the Generator-level
#: classification ``GeneratorSpec.type_tag`` carries a single canonical
#: token regardless of whether the unit burns fuel or harvests a
#: renewable resource.  ``Fuel.type`` only ever uses the thermal-side
#: subset (no Fuel object is renewable).
FUEL_FAMILY_SOLAR = "solar"
FUEL_FAMILY_WIND = "wind"
FUEL_FAMILY_HYDRO = "hydro"
FUEL_FAMILY_GEOTHERMAL = "geothermal"
#: Generic fallbacks — preserved from the legacy binary
#: ``"thermal"`` / ``"renewable"`` Generator-side classification so
#: downstream tools that rely on ``type.startswith("thermal")`` /
#: ``startswith("renewable")`` keep working when the hierarchical
#: ``<top>:<sub>`` string falls back to top-only.
FUEL_FAMILY_THERMAL = "thermal"
FUEL_FAMILY_RENEWABLE = "renewable"

#: Canonical tags that classify as thermal (fuel-burning) — used by
#: the writer to compose the hierarchical ``Generator.type`` string
#: (``thermal:<family>`` vs. ``renewable:<family>``).
THERMAL_FAMILIES: frozenset[str] = frozenset(
    {
        FUEL_FAMILY_DIESEL,
        FUEL_FAMILY_FUEL_OIL,
        FUEL_FAMILY_GAS,
        FUEL_FAMILY_GLP,
        FUEL_FAMILY_BIOMASA,
        FUEL_FAMILY_BIOGAS,
        FUEL_FAMILY_CARBON,
        FUEL_FAMILY_OTROS,
    }
)

#: Canonical tags that classify as renewable / non-burning.
RENEWABLE_FAMILIES: frozenset[str] = frozenset(
    {
        FUEL_FAMILY_SOLAR,
        FUEL_FAMILY_WIND,
        FUEL_FAMILY_HYDRO,
        FUEL_FAMILY_GEOTHERMAL,
    }
)

#: Generator-name suffix → canonical fuel family.  Empirically derived
#: from the 716-Fuel-FK tally on CEN-PCP 2026-03-15: every suffix below
#: binds 1:1 to exactly one fuel family.  ``_GN``/``_INF`` are CEN-PCP
#: gas variants (pipeline, "inflexible LNG"); ``_IFO``/``_FO6`` are
#: Intermediate Fuel Oil and Bunker C respectively, both bound to the
#: ``FuelOil_*`` family.
_GEN_SUFFIX_TO_FUEL_FAMILY: dict[str, str] = {
    "DIE": FUEL_FAMILY_DIESEL,
    "HFO": FUEL_FAMILY_FUEL_OIL,
    "IFO": FUEL_FAMILY_FUEL_OIL,
    "FO6": FUEL_FAMILY_FUEL_OIL,
    "GN": FUEL_FAMILY_GAS,
    "GNL": FUEL_FAMILY_GAS,
    "INF": FUEL_FAMILY_GAS,
    "GLP": FUEL_FAMILY_GLP,
}

#: Fuel-object name prefix → canonical fuel family.  Verified on the
#: 240-fuel ``Fuel_Price.csv`` 2026-03-15 (8 distinct family prefixes,
#: each 1:1 with one of the canonical tags above).
_FUEL_NAME_PREFIX_TO_FAMILY: dict[str, str] = {
    "Diesel_": FUEL_FAMILY_DIESEL,
    "FuelOil_": FUEL_FAMILY_FUEL_OIL,
    "Gas_": FUEL_FAMILY_GAS,
    "GLP_": FUEL_FAMILY_GLP,
    "Biomasa_": FUEL_FAMILY_BIOMASA,
    "Biogas_": FUEL_FAMILY_BIOGAS,
    "Carbon_": FUEL_FAMILY_CARBON,
    "Otros_": FUEL_FAMILY_OTROS,
}

#: CEN renewable-generator name suffix → canonical tag.  Verified on
#: the CEN-PCP 2026-03-15 bundle: every ``*_FV`` generator (740 / 740)
#: lives in PLEXOS category ``"Solar Farms"`` and every ``*_EO`` (66 /
#: 66) lives in ``"Wind Farms"`` — perfect agreement, suffix and
#: category are interchangeable signals.
_GEN_SUFFIX_TO_RENEWABLE_TECH: dict[str, str] = {
    "FV": FUEL_FAMILY_SOLAR,
    "EO": FUEL_FAMILY_WIND,
}

#: PLEXOS category name **substring** → canonical primary-energy tag.
#: Matched case-insensitively on the FIRST occurrence below (order
#: matters — ``"thermal"`` is intentionally last so a hypothetical
#: ``"Thermal Solar"`` category still resolves to ``solar``).  The
#: substring approach handles CEN-PCP's group variants
#: (``"Hydro Gen Group A/B/C"``, ``"Thermal Gen N./S. Zone"``,
#: ``"Termicas Ficticias"``) without an exhaustive enumeration.
_PLEXOS_CATEGORY_SUBSTR_TO_FAMILY: tuple[tuple[str, str], ...] = (
    ("solar", FUEL_FAMILY_SOLAR),
    ("wind", FUEL_FAMILY_WIND),
    # PLEXOS / Spanish synonyms — ``eolica`` covers CEN's
    # accent-stripped category name should one appear.
    ("eolic", FUEL_FAMILY_WIND),
    # PLEXOS uses ``Hydro Gen Group A/B/C``; Spanish: ``hidr*``.
    ("hydro", FUEL_FAMILY_HYDRO),
    ("hidr", FUEL_FAMILY_HYDRO),
    ("geother", FUEL_FAMILY_GEOTHERMAL),
    # Thermals fall through to ``thermal`` only when no fuel signal
    # is available — kept here so renewable-vs-thermal detection
    # works even when the generator carries no Fuel FK (e.g. CEN's
    # ``"Termicas Ficticias"`` fictitious thermal reserve units).
    ("thermal", FUEL_FAMILY_THERMAL),
    ("termic", FUEL_FAMILY_THERMAL),
)


def fuel_family_of_generator(name: str) -> str | None:
    """Return the canonical fuel-family tag implied by a PLEXOS Generator name.

    The CEN-PCP naming convention is ``<plant>[_<mode>]_<FUEL>[_<sub>]``:

    * ``UJINA_U5_DIE`` → ``"diesel"`` (last segment is the tag)
    * ``ATA-TG1A_GNL_X`` → ``"gas"`` (second-to-last segment; trailing
      ``_X`` is the dispatch-mode marker, not a fuel tag)
    * ``COLMITO_GNL_B`` → ``"gas"``
    * ``KELAR-TG1+0.5TV_GNL_X`` → ``"gas"``

    Returns ``None`` when no recognised suffix is present
    (``EL_TOTORAL_U2`` — ``_U2`` is a unit index, not a fuel tag).
    """
    parts = name.rsplit("_", 1)
    if len(parts) == 2 and parts[1] in _GEN_SUFFIX_TO_FUEL_FAMILY:
        return _GEN_SUFFIX_TO_FUEL_FAMILY[parts[1]]
    parts2 = name.rsplit("_", 2)
    if len(parts2) == 3 and parts2[1] in _GEN_SUFFIX_TO_FUEL_FAMILY:
        return _GEN_SUFFIX_TO_FUEL_FAMILY[parts2[1]]
    return None


def fuel_family_of_fuel(name: str) -> str | None:
    """Return the canonical fuel-family tag implied by a PLEXOS Fuel name.

    * ``Diesel_Collahuasi`` → ``"diesel"``
    * ``Gas_Kelar_A`` → ``"gas"``
    * ``FuelOil_Norgener`` → ``"fuel_oil"``
    * ``Biomasa_CMPCLaja_B1`` → ``"biomasa"``
    * ``Otros_Noracid`` → ``"otros"``

    Returns ``None`` when the name doesn't begin with one of the eight
    canonical prefixes (any non-CEN bundle or a hand-rolled fuel name).
    Callers that want a guaranteed-non-None default should use
    ``fuel_family_of_fuel(name) or FUEL_FAMILY_OTHER``.
    """
    for prefix, family in _FUEL_NAME_PREFIX_TO_FAMILY.items():
        if name.startswith(prefix):
            return family
    return None


def _sibling_fuel_family_matches(sib: GeneratorSpec, family: str) -> bool:
    """``True`` iff any of ``sib``'s Fuels classifies to ``family``."""
    return any(fuel_family_of_fuel(fn) == family for fn in sib.fuel_names)


def renewable_tech_of_generator(name: str) -> str | None:
    """Return the canonical renewable-tech tag implied by a Generator name.

    CEN-PCP encodes the renewable technology in the trailing name
    segment: ``*_FV`` → solar PV, ``*_EO`` → wind.  Returns ``None``
    when the suffix doesn't match; callers may then fall back to
    :func:`primary_energy_of_generator`'s category branch.
    """
    last = name.rsplit("_", 1)[-1] if "_" in name else ""
    return _GEN_SUFFIX_TO_RENEWABLE_TECH.get(last)


def family_from_plexos_category(category_name: str) -> str | None:
    """Map a PLEXOS Generator category name to a canonical family tag.

    Case-insensitive substring match — handles CEN-PCP's group
    variants (``"Hydro Gen Group A/B/C"`` → ``"hydro"``,
    ``"Thermal Gen N./S. Zone"`` → ``"thermal"``,
    ``"Termicas Ficticias"`` → ``"thermal"``) without enumerating every
    suffix.  Returns ``None`` when no substring matches.
    """
    if not category_name:
        return None
    lowered = category_name.lower()
    for substr, family in _PLEXOS_CATEGORY_SUBSTR_TO_FAMILY:
        if substr in lowered:
            return family
    return None


def primary_energy_of_generator(
    name: str,
    category_name: str | None,
    fuel_names: tuple[str, ...] = (),
    *,
    has_heat_rate: bool = False,
) -> str:
    """Classify a Generator into the canonical primary-energy taxonomy.

    Returns one of the ``FUEL_FAMILY_*`` tags — a thermal family
    (``diesel``/``fuel_oil``/``gas``/``glp``/``biomasa``/``biogas``/
    ``carbon``/``otros``) for fuel-burning units, a renewable tag
    (``solar``/``wind``/``hydro``/``geothermal``) for renewables, or
    the generic ``thermal`` / ``renewable`` fallbacks when no specific
    family can be inferred.  Guaranteed non-empty so the writer can
    always emit a non-null ``Generator.type`` field.

    Detection priority (the first signal to fire wins).  Specific
    classifications (a fuel family, a renewable technology) win over
    generic ones (the bare ``"thermal"`` category bucket); within the
    specific tier, name-encoded signals win over category lookups,
    which win over Fuel-attachment lookups:

    1. **Name-suffix family** (``fuel_family_of_generator``) — the
       PLEXOS thermal-suffix taxonomy (``_DIE``/``_HFO``/``_GNL``/…).
    2. **Renewable-tech suffix** (``renewable_tech_of_generator``) —
       CEN's ``_FV``/``_EO``.
    3. **Renewable PLEXOS category** — only when the category
       resolves to a *specific* renewable tag
       (``solar``/``wind``/``hydro``/``geothermal``).  This catches
       hydro generators which carry no name-suffix signal.  A generic
       ``"Thermal Gen N. Zone"`` category does NOT fire here — we let
       the fuel-attachment lookup find the carbon/biomasa/diesel/…
       sub-family first.
    4. **Fuel attachment** — if the first ``fuel_names`` entry maps
       via ``fuel_family_of_fuel``, use it; the orphan-recovery
       phantoms ride this branch via the inherited sibling fuel.
    5. **Generic PLEXOS category** — the remaining category match
       (``thermal``) when steps 1-4 didn't fire.  Catches CEN's
       fictitious thermal reserve units which carry a category but
       no Fuel FK.
    6. **Generic fallback** — ``thermal`` when the unit has any fuel
       signal (``fuel_names`` non-empty OR ``has_heat_rate``); else
       ``renewable``.
    """
    family = fuel_family_of_generator(name)
    if family is not None:
        return family
    tech = renewable_tech_of_generator(name)
    if tech is not None:
        return tech
    category_family = (
        family_from_plexos_category(category_name) if category_name else None
    )
    # Specific renewable category wins over fuel-attachment lookup
    # (hydro generators have no name suffix and may carry no fuel).
    if category_family in RENEWABLE_FAMILIES:
        return category_family
    if fuel_names:
        fuel_fam = fuel_family_of_fuel(fuel_names[0])
        if fuel_fam is not None:
            return fuel_fam
    # Generic category-thermal — only fires when steps 1-4 found
    # nothing more specific (e.g. fictitious thermal reserve units
    # whose name + fuel give no signal but PLEXOS labelled the
    # category as "Termicas Ficticias").
    if category_family is not None:
        return category_family
    if fuel_names or has_heat_rate:
        return FUEL_FAMILY_THERMAL
    return FUEL_FAMILY_RENEWABLE


def _recover_csv_only_thermals(
    xml_specs: list[GeneratorSpec],
    heat_rate_csv: dict[str, list[float]],
    vom_csv: dict[str, list[float]],
) -> list[GeneratorSpec]:
    """Build :class:`GeneratorSpec` for thermals in CSV but absent from XML.

    CEN PCP bundles ship a small set of generators in ``Gen_HeatRate.csv``
    (and ``Gen_VOMCharge`` + ``Gen_StartCost`` + ``Gen_ShutDownCost``) that
    have NO Generator ``t_object`` in ``DBSEN_PRGDIARIO.xml``. These are:

    * CCGT alternative dispatch modes — ``ATA-TG1A_GNL_X``,
      ``KELAR-TG1+TG2+TV_GNL_X``, ``MEJILLONES_3-TG+TV_GNL_X``,
      ``SAN_ISIDRO-TG+TV_GNL_X``, … (~42 names, all ``_GNL_X`` suffix).
    * Legacy CSV-only units — ``UJINA_U{5,6}_DIE``,
      ``EL_TOTORAL_U{2,3}``, ``COLMITO_GNL_B``.
    * Steam-turbine variants with no XML twin — ``LAGUNA_VERDE_T{G,V}``.

    PLEXOS ships ``Gen_Rating = (missing)`` for all of them so they have
    pmax = 0 and never dispatch, but the CSVs still carry the real heat
    rate, VO&M, and (for 44 of 46) startup / shutdown costs.  Recovering
    them as zero-capacity GeneratorSpec entries preserves the cost
    metadata for downstream auditing and keeps the generator catalogue
    in 1:1 sync with PLEXOS-side accounting.

    Each phantom inherits its bus and fuel-membership from the
    longest-common-prefix XML sibling, **biased to siblings whose fuel
    family matches the orphan's PLEXOS suffix tag** (see
    :data:`_FUEL_TAG_PREFIX`).  Examples:

    * ``ATA-TG1A_GNL_X`` (tag ``GNL`` → ``Gas_*``) → ``ATA-TG1A_GNL_A``,
      fuel ``Gas_EnelMejillones_A``.  Both the longest-prefix winner
      and the tagged winner are the same.
    * ``UJINA_U5_DIE`` (tag ``DIE`` → ``Diesel_*``) → the 9-char-prefix
      sibling ``UJINA_U5_HFO`` is **rejected** because it carries
      ``FuelOil_Collahuasi``; the next-best ``UJINA_U1_DIE`` (7-char
      prefix, fuel ``Diesel_Collahuasi``) wins.  Without the tag bias
      the orphan would silently under-price by ~15 % at audit-time.
    * ``EL_TOTORAL_U2`` (no recognised fuel tag; ``_U2`` is a unit
      index) → falls back to plain longest-prefix → ``EL_TOTORAL``,
      fuel ``Diesel_ElTotoral``.

    Phantoms with no sibling sharing a 6-character prefix are skipped —
    observed in CEN PCP 2026-03-15: ``LAGUNA_VERDE_T{G,V}`` (no other
    ``LAGUNA_VERDE_*`` generators in the XML).
    """
    xml_names = {s.name for s in xml_specs}
    siblings = [s for s in xml_specs if s.bus_name and s.fuel_names]

    def _longest_prefix(
        name: str, candidates: list[GeneratorSpec]
    ) -> GeneratorSpec | None:
        best, best_score = None, 0
        for sib in candidates:
            n = 0
            for a, b in zip(name, sib.name):
                if a != b:
                    break
                n += 1
            if n > best_score and n >= 6:
                best, best_score = sib, n
        return best

    def _best_sibling(name: str) -> GeneratorSpec | None:
        # First-pass: when the orphan name classifies to a canonical
        # fuel family (``UJINA_U5_DIE`` → ``"diesel"``,
        # ``ATA-TG1A_GNL_X`` → ``"gas"``), restrict the sibling pool to
        # XML neighbours whose Fuels classify to the same family.  This
        # is the only way to distinguish ``UJINA_U5_DIE`` (Diesel) from
        # its 9-char-prefix XML twin ``UJINA_U5_HFO`` (FuelOil) — the
        # raw longest-prefix winner would inherit the wrong fuel and
        # under-price the orphan by ~15 % at audit-time.  The same
        # ``fuel_family_of_*`` helpers feed ``FuelSpec.type_tag`` via
        # :func:`extract_fuels`, so the orphan-recovery rule and the
        # fuel-definition tag stay aligned by construction.
        family = fuel_family_of_generator(name)
        if family is not None:
            in_family = [s for s in siblings if _sibling_fuel_family_matches(s, family)]
            best = _longest_prefix(name, in_family)
            if best is not None:
                return best
        # Fallback: no fuel-family tag detected (e.g. ``EL_TOTORAL_U2``),
        # or the orphan's family is absent from every prefix-sibling.
        # Take the raw longest-prefix sibling — same behaviour as before
        # the family-aware step landed.
        return _longest_prefix(name, siblings)

    next_oid = max((s.object_id for s in xml_specs), default=0) + 10_000_001
    recovered: list[GeneratorSpec] = []
    for name, hr_vals in heat_rate_csv.items():
        if name in xml_names:
            continue
        if not hr_vals or hr_vals[0] <= 0.0:
            continue
        sib = _best_sibling(name)
        if sib is None:
            logger.info(
                "extract_generators: CSV-only thermal %r has no XML sibling "
                "sharing a ≥6-char prefix — skipping (no bus available)",
                name,
            )
            continue
        # Classify the phantom on the primary-energy axis using the
        # inherited fuel — phantoms carry HR > 0 so they're always
        # thermals; the sibling's fuel resolves the family
        # (``UJINA_U5_DIE`` → ``"diesel"``, ``ATA-TG1A_GNL_X`` →
        # ``"gas"``).  Falls back to bare ``"thermal"`` if the fuel
        # name doesn't match any canonical prefix.
        phantom_type_tag = primary_energy_of_generator(
            name=name,
            category_name=None,
            fuel_names=sib.fuel_names,
            has_heat_rate=True,
        )
        recovered.append(
            GeneratorSpec(
                object_id=next_oid,
                name=name,
                bus_name=sib.bus_name,
                type_tag=phantom_type_tag,
                pmin=0.0,
                pmax=0.0,
                heat_rate=hr_vals[0],
                vom_charge=(vom_csv.get(name) or [0.0])[0],
                fuel_transport=0.0,
                fuel_names=sib.fuel_names,
                pmax_profile=(),
                fuel_price_override=0.0,
                pmax_segments=(),
                heat_rate_segments=(),
                fixed_load_profile=(),
                initial_generation=0.0,
                initial_units=0.0,
                aux_use=0.0,
                # Record the inheritance parent so the writer can stamp
                # ``[gtopt-meta mode_variant=secondary
                # inherits_emission_from=<sib.name>]`` on the orphan's
                # description.  Downstream gtopt_marginal_units uses the
                # meta to inherit emission_rate from the parent if the
                # orphan ever ends up as the LP marginal (rare —
                # pmax=0 means it almost never dispatches, but the LP
                # basis can elect it at tie-break corners).
                inherits_emission_from=sib.name,
            )
        )
        next_oid += 1
    if recovered:
        logger.info(
            "extract_generators: recovered %d CSV-only thermals as "
            "zero-capacity audit entries (HR + VO&M preserved, fuel "
            "inherited from longest-prefix sibling)",
            len(recovered),
        )
    return recovered


#: A generator whose finalised ``pmax`` is positive but below this floor
#: (MW) is a degenerate stale-rating artifact, not a real plant — e.g.
#: ``SANTA_ROSA`` ships ``3.45e-10`` MW from PLEXOS.  Such a tiny pmax lands
#: as the ``commitment_gen_upper`` coefficient (``gen <= pmax*status``),
#: injecting a ~1e-10 matrix entry that blows the LP condition number
#: (kappa) to ~1e14.  Collapsing it to exactly ``0.0`` routes the unit
#: through the existing ``pmax == 0`` machinery (commitment dropped, safe
#: ``[0, 0]`` column).  The CEN pmax distribution has a clean gap — nothing
#: between ``3.45e-10`` MW (SANTA_ROSA) and ``0.01`` MW (smallest real
#: plant) — so this floor (1e-3 MW = 1 kW) catches only garbage ratings.
#: Empirically observed CEN garbage values: MUCHI = 4.03e-8 MW (40 nW —
#: arbitrary precision residual on an OFF unit), EL_MIRADOR = 3.31e-4 MW
#: (0.33 kW).  The previous 1e-4 floor caught MUCHI but missed
#: EL_MIRADOR; 1e-3 catches both without risking any real CEN unit
#: (smallest real dispatchable plant in CEN is ≥1 MW).
_PMAX_DEGENERATE_FLOOR_MW = 1.0e-3


def _warn_if_series_varies(
    field: str, name: str, series: list[float] | tuple[float, ...]
) -> None:
    """Warn when a PLEXOS per-period input series is about to be collapsed to a
    single scalar but is NOT constant across the horizon.

    Several inputs (heat rate, VO&M, fuel transport, turbine production factor)
    have NO per-block profile field on their gtopt target spec, so the converter
    keeps only the period-1 value.  On CEN PCP these are flat, so this is a
    no-op; but if a future bundle ships a genuinely time-varying series the
    variation would be silently lost — this surfaces it as a WARNING instead.
    Structural guard against the "profile silently collapsed to scalar" class.

    Only DEFINED periods are compared: these inputs are read with
    ``read_long(fill_forward=False)``, which zero-pads undefined periods (a
    sparse period-1-only CSV becomes ``[v, 0, 0, …]``), so 0 means "no row",
    not a real value.  Comparing the non-zero values avoids false-flagging that
    padding while still catching genuine intra-horizon variation among the
    periods PLEXOS actually defines.
    """
    defined = [v for v in series if v != 0.0]
    if defined and (max(defined) - min(defined)) > 1.0e-9:
        logger.warning(
            "extract: '%s' ships a time-varying %s (%.4g..%.4g across defined "
            "periods) but gtopt carries a single scalar — the per-period "
            "variation is NOT transferred.",
            name,
            field,
            min(defined),
            max(defined),
        )


def extract_generators(
    db: PlexosDb,
    bundle: PlexosBundle,
    *,
    nameplates_out: dict[str, float] | None = None,
) -> tuple[GeneratorSpec, ...]:
    """One :class:`GeneratorSpec` per Generator object.

    Bus resolved via ``Generator → Nodes`` collection. Fuel-memberships
    flag the unit as thermal (vs renewable). Per-unit per-hour
    availability comes from ``Gen_Rating.csv`` (long, band=1).
    Min stable level from ``Gen_MinStableLevel.csv`` (long, band=1,
    period 1 scalar since v0 collapses to first-period for static
    fields).  Fuel transport charge ($/MWh) comes from
    ``Gen_FuelTransportCharge.csv`` and is matched per-(generator, fuel)
    via the PLEXOS ``<gen_name><fuel_name>`` key convention.

    :param nameplates_out: optional dict populated in place with
        ``{gen.name: Max Capacity [MW]}`` for every generator whose
        PLEXOS ``Max Capacity`` static property is positive and finite.
        This is the PHYSICAL nameplate — read UNCONDITIONALLY, even for
        units PLEXOS holds offline (``Gen_Rating = 0`` → dispatch
        ``pmax = 0``).  It feeds the reservoir extraction-flow estimator
        (``apply_reservoir_flow_estimates(generator_capacities=...)``) so
        the reservoir ``fmax`` bound reflects full turbine capacity, NOT
        the maintenance-reduced weekly dispatch.  It is NOT written into
        the output JSON and never alters the dispatch ``pmax``.
    """
    bus_map = _gen_bus_map(db)
    fuel_map = _gen_fuel_map(db)

    pmax_profiles: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_Rating.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_Rating.csv")
        else {}
    )

    # ── PLEXOS Generator dispatch-state CSVs (forced + outage) ──────
    #
    # plexos2gtopt's original pmax extraction only honoured
    # ``Gen_Rating.csv`` (the nameplate × availability profile).
    # PLEXOS layers THREE more dispatch-state CSVs on top of that
    # — they were silently ignored, which on CEN PCP weekly 2026-04-22
    # gave the LP a deceptively-loose feasible set: 727 must-run
    # renewables, 1354 forced outages, and 1736 commit-override entries
    # were all invisible to gtopt.  Reading them here aligns gtopt
    # with PLEXOS's actual dispatch envelope.
    #
    # CSV semantics (per PLEXOS Help → Generator class properties):
    #
    #   * ``Gen_FixedLoad.csv`` (Fixed Load, MW/period): PLEXOS
    #     *required generation* (the generation variable is fixed to
    #     this value).  The writer maps it tech-dependently — hard
    #     equality ``pmin[t] = pmax[t] = fixed_load[t]`` for
    #     non-renewable units (real marginal cost: forced-dispatch /
    #     commitment trajectory), but a curtailable cap
    #     ``pmin[t] = 0, pmax[t] = fixed_load[t]`` for zero-cost
    #     renewables / run-of-river hydro so a transmission/commitment
    #     limit can't drive the LP infeasible.  See
    #     ``gtopt_writer.py::build_generator_array``.
    #
    #   * ``Gen_UnitsOut.csv`` (Units Out, # of units): derate
    #     ``pmax[t] *= (max_units − units_out[t]) / max_units``.
    #     For single-unit gens (max_units = 1), ``units_out = 1``
    #     means the gen is fully out for that period (pmax = 0).
    #     For multi-unit plants the derating is proportional.
    #
    #   * ``Gen_Commit.csv`` (Commit, ∈ {-1, 0, 1}):
    #         -1 ⇒ Endogenous: NO commitment on this gen — let the
    #              LP/MIP run without a ``CommitmentSpec`` (the
    #              default for 988 / 1792 = 55% of CEN PCP gens).
    #              Gens that have ``Commit = -1`` for ALL periods are
    #              skipped from ``CommitmentSpec`` extraction.
    #          0 ⇒ Don't Commit: emit a ``CommitmentSpec`` with the
    #              status fixed/free (writer's call); pmax is left
    #              untouched (per-period 0-availability already comes
    #              via Gen_UnitsOut or Gen_Rating = 0).
    #         +1 ⇒ Commit: must-run within ``[pmin, pmax]``; gtopt
    #              models this via the standard commitment binary.
    #
    # All three are LONG-FORMAT per-period CSVs (NAME,Y,M,D,P,BAND,
    # VALUE); ``read_long`` returns the per-hour vector over the
    # bundle's ``n_days`` horizon.
    fixed_loads: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_FixedLoad.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_FixedLoad.csv")
        else {}
    )
    units_out: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_UnitsOut.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_UnitsOut.csv")
        else {}
    )
    # ``Gen_AuxUse.csv`` (PLEXOS ``Auxiliary Use``): per-generator scalar
    # ``Name,Value`` station-service consumption EXPRESSED IN PERCENT
    # (0-100), NOT per-unit fraction.  Empirically confirmed on the CEN
    # PCP database: typical values are 0.07 (= 0.07%, modern CCGT) up
    # to ~30 (= 30%, old diesel); the file ships values > 1 routinely
    # (max 30.058) which would be impossible under a p.u. interpretation.
    #
    # Bug history: an earlier converter version filtered
    # ``0 < val < 1`` and stored ``val`` directly as p.u. — silently
    # dropping the high-percent gens AND treating mid-range values
    # (NEHUENCO at 0.9995% percent → 0.9995 raw) as 99.95% loss,
    # which the gtopt LP then injected as ``brow = 1 − 0.9995 =
    # 0.0005`` — disabling the unit entirely.  See issue tracker.
    #
    # Fix: divide by 100 to get the p.u. fraction; clamp at a
    # physically-reasonable upper bound (50%) and skip clearly-garbage
    # rows (negative, NaN, or above the cap) with a per-row warning.
    AUX_USE_MAX_PERCENT = 50.0  # physical aux-use upper bound
    aux_use_map: dict[str, float] = {}
    # PLEXOS ships Gen_AuxUse.csv in the bundle but does NOT apply it: the
    # solution's Auxiliary Use (prop 81) is 0 and its Total Generation Cost
    # carries no aux fuel.  Applying it makes gtopt burn ~54 GWh of
    # station-service fuel PLEXOS never spends — ≈ the entire +13% op-cost
    # gap on CEN PCP cases (and the raw values, e.g. COCHRANE 30.058, are
    # implausibly high for real station service).  Default to IGNORING it so
    # gtopt matches PLEXOS; opt in with --apply-generation-aux-use
    # (GTOPT_APPLY_GENERATION_AUX_USE=1) to model the generators'
    # station-service self-consumption.
    import os as _os

    _apply_aux = _os.environ.get("GTOPT_APPLY_GENERATION_AUX_USE", "0").lower() in (
        "1",
        "true",
        "yes",
    )
    if _apply_aux and bundle.has("Gen_AuxUse.csv"):
        n_skipped_high = 0
        sample_skipped: list[tuple[str, float]] = []
        with bundle.csv("Gen_AuxUse.csv").open(encoding="utf-8", newline="") as _aux_fh:
            for _row in csv.DictReader(_aux_fh):
                _nm = (_row.get("Name") or "").strip()
                try:
                    _val = float(_row.get("Value", "0") or "0")
                except ValueError:
                    continue
                if not _nm or _val <= 0.0:
                    continue
                if _val > AUX_USE_MAX_PERCENT:
                    # Above the physical envelope — likely a data
                    # corruption / unit confusion in the source CSV.
                    # Skip and record for the warning summary.
                    n_skipped_high += 1
                    if len(sample_skipped) < 5:
                        sample_skipped.append((_nm, _val))
                    continue
                # Convert percent → p.u. fraction.
                aux_use_map[_nm] = _val / 100.0
        if n_skipped_high:
            logger.warning(
                "Gen_AuxUse.csv: %d rows had Value > %g%% (physical "
                "aux-use upper bound) and were dropped; sample: %s%s",
                n_skipped_high,
                AUX_USE_MAX_PERCENT,
                ", ".join(f"{n}={v:.3f}" for n, v in sample_skipped),
                "" if n_skipped_high <= 5 else ", ...",
            )
    # ``Gen_Commit.csv`` is loaded and applied on the commitment
    # side (``extract_commitments`` skips gens with ALL values = -1).
    # Initial generation is per-generator scalar (PLEXOS ships only
    # period 1); read once with ``n_days=1`` and look up by name.
    initial_generations: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_IniGeneration.csv"), n_days=1)
        if bundle.has("Gen_IniGeneration.csv")
        else {}
    )
    # PLEXOS ``Generator.Units`` (``Gen_IniUnits.csv``): initial number
    # of units online at t=0 — {0, 1} flag per ``<plant>_U<k>`` Generator
    # in CEN PCP.  Round-tripped onto ``GeneratorSpec.initial_units`` for
    # the writer to emit as gtopt ``Generator.uini``.  Period 1 only
    # (initial-condition value).
    ini_units_long: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_IniUnits.csv"), n_days=1)
        if bundle.has("Gen_IniUnits.csv")
        else {}
    )
    # Per-generator nameplate "Max Units" property — denominator for
    # the proportional derating.  Falls back to 1.0 for single-unit
    # gens missing the explicit declaration (the common CEN PCP case).
    max_units_by_gen: dict[str, float] = {}
    for gen_obj in db.objects_of_class("Generator"):
        try:
            mu = db.static_property("Generator", gen_obj.object_id, "Max Units")
        except (LookupError, KeyError, TypeError):
            mu = None
        max_units_by_gen[gen_obj.name] = float(mu) if mu and mu > 0 else 1.0
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
    use_plexos_commit = _os.environ.get("GTOPT_USE_PLEXOS_COMMIT", "0").lower() in (
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
    use_plexos_gen_cap = _os.environ.get("GTOPT_USE_PLEXOS_GEN_CAP", "0").lower() in (
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

        # Capture the PHYSICAL nameplate (PLEXOS ``Max Capacity``, all
        # units) UNCONDITIONALLY — independent of the dispatch ``pmax``
        # computed below.  PLEXOS zeroes ``Gen_Rating`` (→ ``pmax = 0``)
        # for units it holds offline for the week (COLBUN_U2,
        # PEHUENCHE_U1, RALCO_U2, …); those stay ``pmax = 0`` for
        # PLEXOS-faithful dispatch, but the reservoir extraction-flow
        # estimator still needs the full turbine capacity to size the
        # ``fmax`` bound.  Skip None / non-positive / non-finite ratings.
        if nameplates_out is not None:
            nameplate = db.static_property("Generator", gen.object_id, "Max Capacity")
            if math.isfinite(nameplate) and nameplate > 0.0:
                nameplates_out[gen.name] = float(nameplate)

        # ``Gen_UnitsOut.csv`` is PLEXOS's authoritative outage signal
        # (Generator.Units Out, integer # of units offline).  It is
        # MOSTLY redundant with ``Gen_Rating[t] = 0`` (when CF=0 the
        # CSV writer sets Units Out = 1), but for a small set of
        # ALWAYS-offline thermal units the two disagree: 31 CEN-PCP
        # gens (TOCOPILLA-TG1/2, COLMITO_DIE, CONCON, PLACILLA,
        # ARICA_M2, EL_TOTORAL, LAS_VEGAS, LINARES, SANTA_LIDIA, …)
        # are forced offline ALL 168h via Units Out = 1 even though
        # ``Gen_Rating`` still ships their nameplate capacity (mostly
        # diesel peakers PLEXOS does not allow ST Schedule to
        # commit).  Without honouring Units Out, the gtopt MIP picks
        # those cheap diesels where PLEXOS forbids them.  All
        # observed values are 0 or 1 (every gen is single-unit), so
        # the application reduces to a hard mask: ``pmax[t] = 0`` for
        # every ``t`` with ``Units Out[t] > 0``.  Multi-unit gens
        # would derate proportionally; keep the formula general.
        units_out_profile: list[float] = (
            units_out.get(gen.name, []) if units_out else []
        )
        if units_out_profile and profile:
            peak_units = max(units_out_profile, default=0.0)
            max_units = peak_units if peak_units > 1.0 else 1.0
            new_profile = list(profile)
            n = min(len(new_profile), len(units_out_profile))
            for i in range(n):
                uo = units_out_profile[i]
                if uo > 0.0:
                    factor = max(0.0, 1.0 - uo / max_units)
                    new_profile[i] = new_profile[i] * factor
            profile = tuple(new_profile)

        # ``Gen_Commit`` uses an enum {Commit (+1), Don't Commit (0),
        # Endogenous (−1)} where −1 means "let the LP decide" (the
        # default for 58% of CEN PCP generators).  Interpreting −1 as
        # "force OFF" — the naive reading — would zero pmax on
        # > 1000 generators and break dispatch entirely.  This CSV is
        # read but applied only on the commitment side (skip
        # generators with ALL values = −1 from CommitmentSpec
        # extraction — see ``extract_commitments``).
        #
        # ``FixedLoad`` IS applied on the writer side (``pmin = pmax``
        # for non-renewables, curtailable ``pmax`` cap for zero-cost
        # renewables/RoR), not as an extra pmax derating.  See
        # ``gtopt_writer.py::build_generator_array``.

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
                for i, val in enumerate(new_profile):
                    units_p = commit.get(i + 1, max_units)
                    new_profile[i] = val * (units_p / max_units)
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
                for i, val in enumerate(new_profile):
                    cap_p = gen_cap.get(i + 1)
                    if cap_p is not None:
                        new_profile[i] = min(val, cap_p)
                profile = tuple(new_profile)
        # Use the max of the per-hour profile as the static pmax (gtopt
        # will multiply by pmax_factor when we emit the profile).
        # Skip the Max Capacity fallback when:
        #   * An EXPLICIT cap profile is active (``--use-plexos-gen-cap``)
        #     — we WANT pmax=0 if PLEXOS dispatched the unit at 0
        #     every period.  Without this guard the fallback overrides
        #     the cap and the LP free-runs at full nameplate.
        #   * Gen_Rating.csv ships an explicit zero profile for the
        #     unit (PLEXOS marks it out-of-service for the whole
        #     horizon).  Without this guard we silently restore the
        #     nameplate from ``Max Capacity``, letting the LP dispatch
        #     gens PLEXOS deliberately took offline — verified on
        #     CEN PCP weekly bundle: 32 units (PEHUENCHE_U1, COLBUN_U1,
        #     PANGUE_U1, ANTUCO_U2, ALFALFAL_2, ... 27 small ROR
        #     plants) totalling 1,428 MW of free generation capacity.
        # The fallback remains for XML-only schemas (118-Bus, RTS-96)
        # whose Gen_Rating.csv is genuinely absent (not all-zero).
        pmax = max(profile) if profile else 0.0
        explicit_cap = plexos_gen_cap and gen.name in plexos_gen_cap
        explicit_zero_profile = bool(profile)  # CSV shipped → respect zeros
        if pmax == 0.0 and not explicit_cap and not explicit_zero_profile:
            # Fallback: XML-only schemas (118-Bus, RTS-96) carry
            # ``Max Capacity`` on the System→Generators collection.
            pmax = db.static_property("Generator", gen.object_id, "Max Capacity")
        # Collapse a degenerate near-zero pmax (stale PLEXOS rating, e.g.
        # SANTA_ROSA = 3.45e-10 MW) to exactly 0.0 so the unit flows through
        # the ``pmax == 0`` path (commitment dropped, [0, 0] column) instead
        # of injecting a ~1e-10 ``commitment_gen_upper`` coefficient that
        # wrecks the LP condition number.  Zero the profile too: pmax is its
        # max, so every entry is already below the floor.
        if 0.0 < pmax < _PMAX_DEGENERATE_FLOOR_MW:
            pmax = 0.0
            if profile:
                profile = tuple(0.0 for _ in profile)
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
            _warn_if_series_varies("heat rate", gen.name, heat_rate_csv[gen.name])
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
            _warn_if_series_varies("VO&M charge", gen.name, vom_csv[gen.name])
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
                    _warn_if_series_varies(
                        "fuel transport charge", key, fuel_transport_csv[key]
                    )
                    fuel_transport = fuel_transport_csv[key][0]
                    fuel_transport_found = True
                    break
            if not fuel_transport_found and gen.name in fuel_transport_csv:
                _warn_if_series_varies(
                    "fuel transport charge", gen.name, fuel_transport_csv[gen.name]
                )
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
        # PLEXOS Generator.Fixed Load (Gen_FixedLoad.csv): per-period
        # forced-dispatch profile.  We pass through even when all
        # entries are zero so the writer can still recognise the
        # "must-be-zero" case (gen forced off but still listed).
        # Trim to the bundle's horizon length.
        fixed_load_profile_raw = fixed_loads.get(gen.name, ())
        fixed_load_profile: tuple[float, ...] = (
            tuple(fixed_load_profile_raw)
            if any(v != 0.0 for v in fixed_load_profile_raw)
            else ()
        )

        # PLEXOS Generator.Initial Generation: scalar value used to
        # seed the ramp-from-prior-window state in rolling/cascade
        # solves.  For the monolithic single-stage CEN PCP run it
        # documents the warm-start dispatch level for diagnostics.
        ini_gen_raw = initial_generations.get(gen.name, ())
        initial_generation = float(ini_gen_raw[0]) if ini_gen_raw else 0.0

        # PLEXOS Generator.Units (Gen_IniUnits.csv): initial unit count
        # online at t=0.  None when the CSV is missing or has no row
        # for this gen (so the writer can distinguish from explicit 0).
        ini_units_raw = ini_units_long.get(gen.name, ())
        initial_units = float(ini_units_raw[0]) if ini_units_raw else None

        # Primary-energy classification — derived from the PLEXOS name
        # suffix (``_DIE``/``_GNL``/``_FV``/``_EO``) + category
        # (``Solar Farms``/``Hydro Gen Group *``) + Fuel attachment.
        # Surfaces on ``GeneratorSpec.type_tag`` and ultimately on the
        # gtopt ``Generator.type`` JSON field via the writer's
        # hierarchical ``<top>:<sub>`` composer.
        gen_category = (
            db.categories_by_id.get(gen.category_id)
            if gen.category_id is not None
            else None
        )
        gen_fuel_names = fuel_map.get(gen.object_id, ())
        type_tag = primary_energy_of_generator(
            name=gen.name,
            category_name=gen_category,
            fuel_names=gen_fuel_names,
            has_heat_rate=heat_rate > 0.0,
        )
        out.append(
            GeneratorSpec(
                object_id=gen.object_id,
                name=gen.name,
                bus_name=bus_name,
                type_tag=type_tag,
                pmin=pmin,
                pmax=pmax,
                aux_use=aux_use_map.get(gen.name, 0.0),
                heat_rate=heat_rate,
                vom_charge=vom,
                fuel_transport=fuel_transport,
                fuel_names=gen_fuel_names,
                pmax_profile=profile,
                fuel_price_override=fuel_price_override,
                pmax_segments=pmax_segments,
                heat_rate_segments=hr_segments,
                fixed_load_profile=fixed_load_profile,
                initial_generation=initial_generation,
                initial_units=initial_units,
            )
        )
    # CEN-PCP-only: recover ~46 PLEXOS thermals that ship in
    # ``Gen_HeatRate.csv`` + ``Gen_VOMCharge.csv`` (+ Start/Shut costs)
    # but carry no XML Generator object — CCGT alternative dispatch
    # modes (``*_GNL_X``), legacy CSV-only units (``UJINA_U{5,6}_DIE``,
    # ``EL_TOTORAL_U{2,3}``, ``COLMITO_GNL_B``), and steam-turbine
    # variants (``LAGUNA_VERDE_T{G,V}``).  See
    # ``_recover_csv_only_thermals`` for the sibling-inheritance logic.
    out.extend(_recover_csv_only_thermals(out, heat_rate_csv, vom_csv))
    return tuple(out)


def extract_lines(
    db: PlexosDb,
    bundle: PlexosBundle,
    *,
    shadow_lines_all_off_out: set[str] | None = None,
) -> tuple[LineSpec, ...]:
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
    # Full-horizon read (``n_days × 24``): the hour-0 element still
    # drives the parallel-unit count, while the complete per-hour vector
    # feeds the per-block ``Line.in_service`` schedule (intra-week
    # maintenance / forced-outage windows from ``Lin_Units.csv``).
    units_wide: dict[str, list[float]] = (
        read_wide(
            bundle.csv("Lin_Units.csv"), drop_zero_cols=False, n_days=bundle.n_days
        )
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
            # All-zero across the horizon = mothballed / contingency-state-
            # inactive (PLEXOS's ``_I/_II/_III`` per-circuit shadows and
            # ``_SC`` Sin-Compensar variants stay at Units=0 until the
            # corresponding contingency activates).  PLEXOS would emit a
            # flow variable that's pinned to 0 — equivalent to "term
            # contributes 0" in any UC LHS that references the line.  We
            # don't emit a LineSpec, but we DO record the name so the
            # UserConstraint extractor can recognise such terms and drop
            # them silently (rather than fail-hard as if the name were a
            # typo).
            if shadow_lines_all_off_out is not None:
                shadow_lines_all_off_out.add(line.name)
            continue
        # Parallel-circuit count = the line's genuine capacity multiplier
        # (``tmax_ab × units`` in the writer).  Use the MAX over the
        # horizon, NOT hour-0: when hour-0 lands inside a maintenance
        # window the profile reads 0/1 there, and an hour-0 slice would
        # clamp a healthy 2-circuit corridor down to a single circuit (or
        # to the clamp-to-1 fallback).  The per-hour on/off availability
        # is carried separately by ``Line.in_service`` below, so the
        # scalar here should be the full circuit count; ``in_service``
        # then zeroes out the maintenance hours block-by-block.
        units = int(max(units_profile)) if units_profile else 1
        if units <= 0:  # defensive — all-zero already dropped above
            units = 1
        # Per-hour in-service flag for ``Line.in_service``: 1 where the
        # line has >=1 unit online, 0 during maintenance / forced-outage
        # hours (gtopt honours this per (stage, block) — line_lp.cpp:255).
        # Only carried when there is at least one out-hour (else the line
        # defaults to in-service everywhere and no schedule is emitted).
        # The writer aggregates this to per-block resolution with a
        # conservative ``min`` reducer (block OUT if any hour is out).
        #
        # NOTE: ``in_service`` is binary (full on/off) and that is the
        # correct granularity here — we deliberately do NOT scale
        # ``tmax_ab`` by the per-hour circuit count.  The operator-defined
        # rating already follows an N-1 security criterion: a 2-circuit
        # corridor is rated for safe single-circuit operation (or its
        # equivalent), so when one circuit drops for maintenance the
        # thermal limit is unchanged.  Per-block-units scaling would
        # double-count capacity the rating intentionally withholds.
        in_service_profile: tuple[int, ...] = ()
        if units_profile is not None and any(u <= 0.0 for u in units_profile):
            in_service_profile = tuple(1 if u > 0.0 else 0 for u in units_profile)
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
        # PLEXOS Enforce Limits: 0=Never, 1=Post-solve check only, 2=Always.
        # PLEXOS's INTERNAL default for unset is 0 (verified empirically
        # on CEN PCP 2026-04-07: 2 of 8 unset-EL lines — Tamaya110→
        # S-AA100_3B and Tamaya110→Salar110_4B — dispatched ABOVE their
        # rated cap with 0 hours congested in the PLEXOS sol, the
        # signature of EL=0).  Match that default rather than the
        # over-cautious EL=2 (which over-constrains lines PLEXOS itself
        # leaves unenforced).  The downstream EL=0 → soft-cap path
        # (``extended`` mode below) then applies the standard 2× free
        # envelope + soft penalty / 5× hard cap, so the LP still has a
        # finite bound but doesn't pay a phantom penalty in the
        # under-2× band where PLEXOS lets it flow freely.
        enforce_raw = db.static_property("Line", line.object_id, "Enforce Limits")
        enforce_limits = int(enforce_raw) if enforce_raw is not None else 0
        # PLEXOS EL=0 ("Never enforce") lines: gtopt's DC-OPF does NOT
        # physically limit an uncapped line, so dropping the cap lets the
        # LP route huge (often circulating) flows — e.g. 54,720 MW on the
        # 62 MW ``S-Km6100->Salar110`` 110 kV line.  But a plain HARD cap
        # at the rating over-constrains the radial pockets PLEXOS itself
        # runs above rating (``Capricornio110->LaNegra110`` at 2.7×, no
        # parallel route).  So model EL=0 lines as a SOFT cap instead:
        # free up to the rating, penalised between the rating and
        # ``headroom × rating``, hard at ``headroom × rating`` (the writer
        # applies the headroom + penalty via ``Line.tmax_normal_*`` +
        # ``overload_penalty``).  Orig EL=1/EL=2 lines keep their plain
        # hard cap.  The genuine exceptions named in ``--lift-line-caps``
        # stay GENUINELY uncapped (gtopt ``enforce_level = 0``: emit
        # ``tmax_ab`` for the loss PWL but skip the hard cap in the LP),
        # e.g. ``Capricornio110->LaNegra110``.  Reads
        # ``GTOPT_LIFT_LINE_CAPS`` (comma-separated names) which the CLI
        # propagates through plexos2gtopt.py.
        import os as _os_lift

        _lift_raw = _os_lift.environ.get("GTOPT_LIFT_LINE_CAPS", "")
        _lift_set = {n.strip() for n in _lift_raw.split(",") if n.strip()}
        # Distinguish an EXPLICIT user ``--lift-line-caps`` list from the
        # shipped default (auto) list: the default is the curated
        # ``DEFAULT_LIFT_LINE_CAPS_PLEXOS``, so any other non-empty value is a
        # deliberate user choice.  Explicit lifts take precedence over the
        # rating-based auto-lift suppression below; the default/auto list does
        # NOT lift a rated EL=0 line.
        from gtopt_shared.cli_flags import (  # noqa: PLC0415
            DEFAULT_LIFT_LINE_CAPS_PLEXOS,
        )

        _default_lift_set = {
            n.strip() for n in DEFAULT_LIFT_LINE_CAPS_PLEXOS.split(",") if n.strip()
        }
        _lift_is_explicit = bool(_lift_set) and _lift_set != _default_lift_set
        # PLEXOS defines this line's rating when it ships a Max Rating profile
        # (``tmax_series``), a Min Rating (``tmin_series``), or any Max Flow /
        # Max Rating scalar (``tmax > 0`` — guaranteed here, since tmax<=0 lines
        # were already skipped).  When it does, the automatic EL=0 lift is
        # suppressed: the rating is the operator's intent, not a missing cap.
        _has_tmax_mechanism = tmax > 0.0 or bool(tmax_series) or bool(tmin_series)
        # ``--no-lift-lines`` (GTOPT_NO_LIFT_LINES): the INVERSE of the lift
        # list — names PINNED to a plain HARD cap at their rating, overriding
        # the EL=0 soft-cap free band.  For genuine physical limits PLEXOS
        # never overloads despite EL=0 (the Chacao cable
        # ``PMontt220->Chiloe110``: 90 MW, PLEXOS peak 62 MW into the bounded
        # Chiloé island pocket).  The keep-vs-lift decision is internal to
        # PLEXOS's LP/relaxation and is not derivable from the input rating
        # attributes, so the pin list is hand-curated (default ships the
        # cable; see ``DEFAULT_NO_LIFT_LINES_PLEXOS``).
        _no_lift_raw = _os_lift.environ.get("GTOPT_NO_LIFT_LINES", "")
        _no_lift_set = {n.strip() for n in _no_lift_raw.split(",") if n.strip()}
        # EL=0 ("Never enforce") handling mode (CLI ``--el0-lines``):
        #   "strict" (DEFAULT) — treat EL=0 like EL=2: a plain hard cap at the
        #       nominal rating (no free band, no headroom).  Matches the PLEXOS
        #       solution, where only ~34/188 EL=0 lines are ever run above
        #       rating; those exceptions are LIFTED back to a soft cap via the
        #       ``--lift-line-caps`` default list.
        #   "extended" — relax: soft cap with a free over-rating band + penalty
        #       for EVERY EL=0 line (the behaviour described above).
        _el0_mode = _os_lift.environ.get("GTOPT_EL0_LINES", "strict").strip().lower()
        enforce_limits_orig = enforce_limits
        soft_cap = False
        soft_cap_lifted = False
        if line.name in _no_lift_set and enforce_limits == 0:
            # ``--no-lift-lines`` ONLY acts on genuine EL=0 lines — its whole
            # purpose is to suppress the EL=0 soft-cap free band.  An EL=1/EL=2
            # line is already a plain hard cap, so the pin is a no-op there and
            # we deliberately do NOT demote it (that would silently rewrite its
            # PLEXOS enforce level).  Pin to a plain HARD cap at the rating:
            # forward ``tmax_ab = Lin_MaxRating``, reverse
            # ``tmax_ba = |Lin_MinRating|`` (already set above), NO free band,
            # NO overload penalty.  Wins over both ``--lift-line-caps`` and the
            # EL=0 ``extended`` mode.
            enforce_limits = 2
            logger.info(
                "extract_lines: '%s' (--no-lift-lines) → hard cap at rating "
                "(tmax_ab=%.1f, tmax_ba=%.1f); EL=0 soft-cap free band "
                "suppressed.",
                line.name,
                tmax,
                abs(tmin) if tmin != 0.0 else tmax,
            )
        elif line.name in _no_lift_set:
            # Named in --no-lift-lines but NOT EL=0 (already hard-capped):
            # nothing to suppress — leave the PLEXOS enforce level untouched.
            logger.debug(
                "extract_lines: '%s' (--no-lift-lines) is EL=%d, not EL=0 — "
                "already hard-capped, pin is a no-op.",
                line.name,
                enforce_limits,
            )
        elif line.name in _lift_set and _lift_is_explicit:
            # EXPLICIT ``--lift-line-caps`` (a user list differing from the
            # shipped default) takes PRECEDENCE for ALL lines: lift to a soft
            # cap (3× free / 6× hard band) regardless of any rating mechanism.
            enforce_limits = 1
            soft_cap = True
            soft_cap_lifted = True
            logger.info(
                "extract_lines: '%s' (--lift-line-caps, explicit) → soft cap, "
                "free to 3x rating then penalised up to 6x (was EL=%d).",
                line.name,
                enforce_limits_orig,
            )
        elif enforce_limits == 0 and _has_tmax_mechanism and _el0_mode == "strict":
            # AUTOMATIC (default, ``--el0-lines strict``) EL=0 lifting is
            # SUPPRESSED when PLEXOS defines this line's rating (a ``tmax_ab``
            # Max Rating profile, a Min Rating, or a Max Flow scalar): the rating
            # is the operator's stated intent, so pin to a plain HARD cap at the
            # ORIGINAL rating instead of inflating it to the default 3× free / 6×
            # hard soft-cap band (which, with ``--loss-extend-overload`` ON, also
            # stretches the loss envelope to 6×).  This OVERRIDES the default
            # ``--lift-line-caps`` auto-list.  Lifting BY OPTION still takes
            # precedence: an EXPLICIT ``--lift-line-caps`` (handled above) or the
            # explicit ``--el0-lines extended`` mode (falls through below) lifts
            # regardless.  A radial pocket PLEXOS itself ran above rating may
            # turn the LP infeasible — surfaced, not silently re-lifted.
            enforce_limits = 2
            logger.info(
                "extract_lines: '%s' (EL=0 with PLEXOS rating) → hard cap at "
                "original rating %.1f; automatic lift suppressed.",
                line.name,
                tmax,
            )
        elif line.name in _lift_set:
            # DEFAULT/auto lift list (the shipped curated corridors): lifts
            # non-EL=0 entries that PLEXOS runs above rating — e.g. the EL=1
            # ``Capricornio110->LaNegra110``.  EL=0 rated entries were already
            # hard-capped just above (automatic lift suppressed).  The soft band
            # lets the LP push past the rating at a penalty without teleporting
            # GWs (the 6× hard cap blocks that).
            enforce_limits = 1
            soft_cap = True
            soft_cap_lifted = True
            logger.info(
                "extract_lines: '%s' (--lift-line-caps, default) → soft cap, "
                "free to 3x rating then penalised up to 6x (was EL=%d).",
                line.name,
                enforce_limits_orig,
            )
        elif enforce_limits == 0:
            if _el0_mode == "strict":
                # Act like EL=2: hard cap at the nominal rating.
                enforce_limits = 2
            else:
                enforce_limits = 1
                soft_cap = True
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
                soft_cap=soft_cap,
                soft_cap_lifted=soft_cap_lifted,
                in_service_profile=in_service_profile,
            )
        )
    # Adaptive per-line PWL loss-segment count.  PLEXOS-side OPF runs the
    # quadratic loss model ``P_loss = R·f²``; gtopt linearises each lossy
    # line with K equal-spaced breakpoints, picking up worst-case per-line
    # error ``L_max,i / (4·K_i²)`` where ``L_max,i = R_i · fmax_i²`` is the
    # peak loss at the rated flow.  Summing over lines and constraining
    # total absolute error ``Σ_i L_max,i / (4 K_i²) ≤ B`` (the budget,
    # ``loss_error_pct × Σ L_max``) gives a Lagrangian KKT solution where
    # the optimum allocates K segments with ``K_i ∝ L_max,i^(1/3)`` — see
    # docs/analysis/no-scale-reservoir-effective and the
    # ``project_loss_model_midpoint_envelope`` memory.  Lines with bigger
    # peak loss get more segments; tiny stubs collapse to the floor.
    # ``loss_error_pct = 0`` disables adaptation (legacy: every lossy
    # line gets the ceiling K).
    return _apply_loss_sos2_policy(_apply_adaptive_loss_segments(tuple(out)))


def _apply_adaptive_loss_segments(
    lines: tuple[LineSpec, ...],
) -> tuple[LineSpec, ...]:
    """Stamp ``LineSpec.loss_segments`` with the cube-root rule.

    Driven by env vars set in ``plexos2gtopt.py``:

      * ``GTOPT_LOSS_ERROR_PCT`` (default 0.01).  Positive ⇒ adaptive
        mode.  Zero or negative ⇒ uniform mode (every lossy line gets
        the same K).
      * ``GTOPT_NSEG_LOSSES``    Optional.  When set, it serves as the
        adaptive ceiling AND the uniform K.  When *unset*:
          - adaptive mode: ceiling defaults to **6**
          - uniform mode: K defaults to **4** (historic CEN PCP value)
        The two defaults are intentionally different so the
        no-argument adaptive run (the new default) doesn't reduce
        accuracy on heavy lines vs the historic uniform-K=4 path.

    Floor is fixed at 2 (a single secant is degenerate).  Lossless lines
    (R==0 or fmax==0) get ``loss_segments = 0`` so the writer omits the
    PWL curve entirely.

    Returns a new tuple with per-line overrides stamped.  Lines unchanged
    aside from the new field.
    """
    import os as _os

    try:
        err_pct = float(_os.environ.get("GTOPT_LOSS_ERROR_PCT", "0.01"))
    except ValueError:
        err_pct = 0.01
    nseg_env = _os.environ.get("GTOPT_NSEG_LOSSES")
    adaptive = err_pct > 0.0
    if nseg_env is not None and nseg_env.strip():
        try:
            nseg_user = int(nseg_env)
        except ValueError:
            nseg_user = None
    else:
        nseg_user = None
    floor = 2
    # Adaptive: ceiling = user value or 6.  Uniform: K = user value or 4.
    if adaptive:
        ceiling = max(floor, nseg_user if nseg_user else 6)
    else:
        ceiling = max(floor, nseg_user if nseg_user else 4)
    # Refinement A+B: extend the PWL envelope into the soft-cap overload
    # band when ``GTOPT_LOSS_EXTEND_OVERLOAD=1`` (``--loss-extend-overload``).
    # Mirrors gtopt_writer's headroom factors (2× regular soft_cap,
    # 4× soft_cap_lifted).  Off by default — the writer still pins
    # ``loss_envelope = orig tmax`` for every line in that case.
    extend_overload = _os.environ.get("GTOPT_LOSS_EXTEND_OVERLOAD", "0").strip() in (
        "1",
        "true",
        "yes",
        "on",
    )

    # Per-line peak loss in MW: ``L_max,i = R · envelope²`` where the
    # envelope mirrors the writer's PWL ``loss_envelope`` field.  Use the
    # ORIGINAL rating (LineSpec carries the pre-headroom value; the
    # writer only inflates ``tmax_ab`` by a headroom factor later when
    # the soft-cap path expands the hard cap), and include any DLR
    # per-hour profile peak so corridors with higher daytime ratings
    # (e.g. LoAguirre500->Polpaico500: 900/2078 MW) get the segments
    # sized for their max flow band, not the overnight floor.  Tmin
    # (reverse direction) is taken at absolute value since the loss
    # PWL is symmetric about f=0.  ``max_rating`` (PLEXOS emergency /
    # short-term rating) is INTENTIONALLY excluded — gtopt's
    # ``loss_envelope`` covers the realistic loading band, and the
    # 1.5-2× emergency margin is meant for the soft-cap overload
    # region above the PWL curve (the LP extrapolates the last slope
    # there).  Matches ``gtopt_writer._resolve_loss_layout`` /
    # ``loss_envelope`` precedence.
    def _peak_loss(ln: LineSpec) -> float:
        if ln.resistance <= 0.0:
            return 0.0
        profile_peak = (
            max(abs(x) for x in ln.tmax_ab_profile) if ln.tmax_ab_profile else 0.0
        )
        base_fmax = max(abs(ln.tmax_ab), abs(ln.tmin_ab), profile_peak)
        # Refinement A+B (gated by --loss-extend-overload): when the LP
        # can flow into the soft-cap overload band, size K_i for that
        # wider envelope so per-segment error stays bounded across the
        # actually-reachable flow range.  Multipliers mirror
        # ``gtopt_writer``'s headroom factors (2× regular soft_cap,
        # 4× soft_cap_lifted).  EL=1/EL=2 hard-cap lines: LP cannot
        # exceed tmax, so the base envelope is exact — no extension.
        if extend_overload:
            if ln.soft_cap_lifted:
                fmax = 4.0 * base_fmax
            elif ln.soft_cap:
                fmax = 2.0 * base_fmax
            else:
                fmax = base_fmax
        else:
            fmax = base_fmax
        return ln.resistance * fmax * fmax

    lossy = [(i, ln, _peak_loss(ln)) for i, ln in enumerate(lines)]
    lossy = [t for t in lossy if t[2] > 0.0]
    if not lossy or not adaptive:
        # Uniform mode (adaptive disabled) OR no lossy lines.  Stamp
        # every lossy line with the uniform K = `ceiling` (which in
        # uniform mode IS the K value, derived above with default 4).
        return tuple(
            dataclasses.replace(ln, loss_segments=ceiling if _peak_loss(ln) > 0 else 0)
            for ln in lines
        )

    # Cube-root rule: minimize Σ K subject to Σ L/(4 K²) ≤ B.
    # KKT ⇒ K_i = c · L_i^(1/3) with c = √(S / (4·B)),
    # S = Σ L_i^(1/3), B = err_pct · Σ L_i.
    L_total = sum(L for _, _, L in lossy)
    S = sum(L ** (1.0 / 3.0) for _, _, L in lossy)
    B = err_pct * L_total
    c = math.sqrt(S / (4.0 * B)) if B > 0 else float("inf")

    new_lines = list(lines)
    for i, ln, L in lossy:
        k_raw = c * (L ** (1.0 / 3.0))
        k = max(floor, min(ceiling, int(math.ceil(k_raw))))
        new_lines[i] = dataclasses.replace(ln, loss_segments=k)
    # Lossless lines: keep loss_segments=0 (writer omits the curve)

    # If the user picked ``--loss-pwl-layout dynamic``, layer the
    # mean-error allocator on top of the K assignments above.  See
    # ``_apply_dynamic_loss_layout`` for the algorithm.
    base_layout = _os.environ.get("GTOPT_LOSS_PWL_LAYOUT", "midpoint")
    if base_layout == "dynamic":
        return _apply_dynamic_loss_layout(tuple(new_lines), err_pct, lossy)

    return tuple(new_lines)


def _apply_dynamic_loss_layout(
    lines: tuple[LineSpec, ...],
    err_pct: float,
    lossy: list,
) -> tuple[LineSpec, ...]:
    """Per-line PWL LAYOUT assignment under the same ``err_pct`` budget.

    The adaptive K rule (``_apply_adaptive_loss_segments``) already
    bounds the WORST-CASE per-segment secant error by
    ``Σ L_i / (4 K_i²) ≤ err_pct · Σ L_i``.  Dynamic mode layers the
    layout decision on top so the SYSTEM-WIDE SIGNED MEAN error also
    stays within budget — with most lines on the fast ``uniform``
    layout (presolve eliminates the loss column) and only the heaviest
    contributors flipped to ``midpoint`` (loss column survives, but
    the negative-bias debias cancels the uniform lines' positive bias).

    Mean error per line (in the same R/V²·MW² units as ``L_max``):

      * uniform  layout at K segments:  ``+ L_max / (6 K²)``  (overstate)
      * midpoint layout at K segments:  ``- L_max / (12 K²)`` (understate)

    Algorithm (mirrors the KKT-style cube-root rule of phase 1):

      1. Compute mean_uniform_i = L_max,i / (6 K_i²) for every lossy line.
      2. If Σ mean_uniform_i ≤ err_pct · Σ L_max,i → keep all uniform
         (cheapest LP cost, budget already met).
      3. Else sort lines by mean_uniform_i descending and flip the
         heaviest one to midpoint.  Each flip changes the running
         signed mean by  Δ = -(L_max_i/(6 K²) + L_max_i/(12 K²))
                            = -L_max_i/(4 K²)
         which is EXACTLY the worst-case error of that line — i.e. a
         midpoint flip "pays back" exactly one worst-case-error worth
         of bias.  Stop when |running signed mean| ≤ budget.

    Returns a tuple of LineSpec with both ``loss_segments`` and
    ``loss_pwl_layout`` stamped.  Lossless lines (``loss_segments == 0``)
    are left untouched.
    """
    L_total = sum(L for _, _, L in lossy)
    budget = err_pct * L_total

    # ── Phase 1' — recompute K under the two-sided budget ────────────
    # The two-sided worst-case bound (Σ_uniform L/(4K²) ≤ budget AND
    # Σ_midpoint L/(4K²) ≤ budget) gives 2× total worst-case headroom
    # vs the unsigned single-sided budget the cube-root rule used in
    # ``_apply_adaptive_loss_segments``.  Re-run the cube-root rule
    # here with the effective ``2 × budget``: KKT gives
    #
    #     K_i = ⌈c · L_i^(1/3)⌉,  c = √(S / (4 · 2 · budget))
    #         = √(S / (4·B_old)) / √2
    #         ≈ K_i_old / √2  ≈ 71 % of Phase 1's K
    #
    # — i.e. ~29 % fewer LP segments per line on the unclamped middle
    # band.  Phase 2 below then balances ~half the lines to midpoint
    # so each side uses up its own ``budget``.  Floor=2 / ceiling
    # clamps (from ``GTOPT_NSEG_LOSSES`` env var) still apply.
    import os as _os_inner  # noqa: PLC0415  (local; matches the rest of the file)

    floor = 2
    ceiling = 6
    _nseg_env = _os_inner.environ.get("GTOPT_NSEG_LOSSES")
    if _nseg_env:
        try:
            ceiling = max(floor, int(_nseg_env))
        except ValueError:
            pass
    S_dyn = sum(L ** (1.0 / 3.0) for _, _, L in lossy)
    B_dyn = 2.0 * budget  # two-sided headroom
    c_dyn = math.sqrt(S_dyn / (4.0 * B_dyn)) if B_dyn > 0 else float("inf")

    # Re-stamp K per line under the looser dynamic budget.
    enriched: list[tuple[int, LineSpec, float, int]] = []
    new_lines = list(lines)
    for i, ln, L in lossy:
        if L <= 0.0:
            continue
        k_raw = c_dyn * (L ** (1.0 / 3.0))
        k = max(floor, min(ceiling, int(math.ceil(k_raw))))
        new_lines[i] = dataclasses.replace(new_lines[i], loss_segments=k)
        enriched.append((i, ln, L, k))

    # All-uniform signed mean error and worst-case sum (using the
    # Phase 1'-reduced K values stamped into ``new_lines`` above).
    running = sum(L / (6.0 * k * k) for _, _, L, k in enriched)
    all_uniform_worst = sum(L / (4.0 * k * k) for _, _, L, k in enriched)

    # Early return: all-uniform already satisfies BOTH the mean budget
    # AND the one-sided worst-case budget.  Without the worst-case
    # check the all-uniform path returned even when worst_uni was over
    # budget — that left Phase 1.5 starved of headroom AND emitted a
    # configuration that fails the documented two-sided budget invariant.
    if running <= budget and all_uniform_worst <= budget:
        for i, ln, _, _ in enriched:
            new_lines[i] = dataclasses.replace(new_lines[i], loss_pwl_layout="uniform")
        return tuple(new_lines)

    # Need midpoint promotions.  Sort by per-line mean-error
    # contribution descending — each iteration flips the worst
    # contributor, which subtracts its worst-case error from the
    # running signed total (see docstring).
    #
    # Stop condition (two-pronged so the greedy doesn't OVERSHOOT into
    # the negative budget zone): break when either (a) the running
    # mean has fallen inside ±budget, OR (b) flipping the next line
    # would move running FURTHER from zero than its current value.
    # Without (b) the loop happily keeps flipping past the
    # ``running == 0`` point and lands with abs(running) >> budget
    # on the negative side — same magnitude error, opposite sign,
    # zero improvement.
    enriched.sort(key=lambda t: t[2] / (t[3] * t[3]), reverse=True)
    layouts: dict[int, str] = {i: "uniform" for i, _, _, _ in enriched}
    # Two-sided worst-case tracking (refined 2026-05-29) — each layout's
    # error has a fixed sign, so the SYSTEM-WIDE worst-case is bounded
    # independently on each side: Σ_uniform L/(4K²) AND Σ_midpoint
    # L/(4K²) must each stay ≤ err_pct·ΣL.  Phase 1.5 below uses these
    # per-side sums so the cube-root rule's "all uniform with
    # worst_uni = budget" output gets headroom on the midpoint side
    # for K reduction.  Phase 2.5 (further below) extends Phase 2's
    # mean-only flipping with extra flips that reduce worst-case
    # imbalance while respecting the mean budget — without it
    # ``Σ_uniform`` stays pinned at ``budget`` and Phase 1.5 has no
    # uniform-side headroom to use.
    worst_uni = sum(L / (4.0 * k * k) for _, _, L, k in enriched)
    worst_mid = 0.0
    # ── Phase 2: original mean-budget-driven flipping (unchanged
    # contract; pins existing tests).  Each flip subtracts its
    # contribution from worst_uni and adds it to worst_mid.
    for i, _, L, k in enriched:
        if abs(running) <= budget:
            break
        contribution = L / (4.0 * k * k)
        next_running = running - contribution
        if abs(next_running) >= abs(running):
            # Flipping would not help; current state is the local min.
            break
        # Flip line i to midpoint: running changes by -L/(4 k²).
        layouts[i] = "midpoint"
        running = next_running
        worst_uni -= contribution
        worst_mid += contribution

    # ── Phase 2.5: extra flips to balance worst-case across layouts
    # so Phase 1.5 has uniform-side headroom for K reduction.  Only
    # flips that (a) don't burst the mean budget AND (b) strictly
    # reduce |worst_uni − worst_mid| are accepted.  Walk in
    # descending-contribution order again (re-iterate ``enriched``
    # which is already sorted).
    for i, _, L, k in enriched:
        if layouts[i] == "midpoint":
            continue
        contribution = L / (4.0 * k * k)
        next_running = running - contribution
        if abs(next_running) > budget:
            continue  # would burst mean budget
        old_imbalance = abs(worst_uni - worst_mid)
        new_imbalance = abs((worst_uni - contribution) - (worst_mid + contribution))
        if new_imbalance >= old_imbalance:
            continue  # would not improve worst-case balance
        # Flip
        layouts[i] = "midpoint"
        running = next_running
        worst_uni -= contribution
        worst_mid += contribution

    # ── Phase 1.5: try to reduce K on individual lines ─────────────
    # Phase 1 set K from the cube-root rule (worst-case bound) and
    # Phase 2 chose layouts to satisfy the mean-error budget.  Both
    # phases together may leave per-line K *higher than necessary*
    # — particularly when the cube-root rule didn't hit ceiling
    # clamps and the mean budget has slack.  Phase 1.5 hunts those
    # over-allocated K's and reduces them step-by-step.
    #
    # Per-line state for the budgets:
    #   worst_i (any layout)    =  L_i / (4 K_i²)        (unsigned magnitude)
    #   mean_i  (uniform)        =  + L_i / (6 K_i²)
    #   mean_i  (midpoint)       =  − L_i / (12 K_i²)
    #
    # **Two-sided worst-case bound** (signed-aware, refined 2026-05-29):
    # Each layout has a fixed worst-case error SIGN — uniform secants
    # always overstate (chord ≥ curve, ``+`` direction), midpoint
    # tangents always understate (tangent ≤ curve at breakpoints,
    # ``−`` direction).  At any LP operating point the system-wide
    # error is therefore bounded by
    #
    #     − Σ_midpoint L/(4K²)  ≤  system_error  ≤  + Σ_uniform L/(4K²)
    #
    # so for |system_error| ≤ err_pct·ΣL we need BOTH:
    #     Σ_uniform L/(4K²)  ≤ err_pct·ΣL     (positive-side bound)
    #     Σ_midpoint L/(4K²) ≤ err_pct·ΣL     (negative-side bound)
    #
    # This gives **2× total worst-case headroom** vs the unsigned
    # ``Σ all L/(4K²) ≤ budget`` formulation: each layout can carry up
    # to one full budget worth of worst-case independently.  Phase 1.5
    # exploits the headroom by reducing K_i (which increases that
    # line's L/(4K²) contribution to its own side) until the relevant
    # one-sided sum saturates.  Expected ~30% Σ K savings at
    # err_pct = 0.01–0.10 on CEN-PCP-shape systems.
    # ``worst_uni`` and ``worst_mid`` are already maintained by the
    # Phase 2 flipping loop above — reuse them as the starting state
    # for Phase 1.5 (avoid the redundant Σ pass).
    # Build a mutable K map for in-place reduction.
    K_map: dict[int, int] = {i: k for i, _, _, k in enriched}
    # Iterate lines by current K descending: reductions on high-K
    # lines free more LP segments per safe step.  Repeat passes
    # until no further reduction passes both budget checks.
    changed = True
    while changed:
        changed = False
        # Re-sort each pass: K's drift as we reduce.
        items = sorted(
            ((i, L) for i, _, L, _ in enriched),
            key=lambda t: K_map[t[0]],
            reverse=True,
        )
        for i, L in items:
            k = K_map[i]
            # ``floor`` is fixed at 2 in ``_apply_adaptive_loss_segments``
            # (a single secant is degenerate, collapses to linear loss
            # mode handled elsewhere).  Match that contract here so
            # Phase 1.5 never reduces K below 2.
            if k <= 2:
                continue
            new_k = k - 1
            delta_worst = L / (4.0 * new_k * new_k) - L / (4.0 * k * k)
            # Per-side worst-case check: only the side this line lives
            # on grows; the other side is unchanged.
            if layouts[i] == "midpoint":
                if worst_mid + delta_worst > L_total * err_pct:
                    continue  # negative-side budget would burst
            else:
                if worst_uni + delta_worst > L_total * err_pct:
                    continue  # positive-side budget would burst
            # Mean shift for this layout: removing the old contrib
            # and adding the new (always pushes |running| further
            # from zero because new_k < k → larger 1/k²).
            if layouts[i] == "midpoint":
                old_m = -L / (12.0 * k * k)
                new_m = -L / (12.0 * new_k * new_k)
            else:
                old_m = +L / (6.0 * k * k)
                new_m = +L / (6.0 * new_k * new_k)
            new_running = running - old_m + new_m
            if abs(new_running) > L_total * err_pct:
                continue  # mean budget would burst
            # Commit
            K_map[i] = new_k
            if layouts[i] == "midpoint":
                worst_mid += delta_worst
            else:
                worst_uni += delta_worst
            running = new_running
            changed = True

    # Stamp final (K, layout) on every lossy LineSpec.
    for i, _, _, _ in enriched:
        new_lines[i] = dataclasses.replace(
            new_lines[i],
            loss_segments=K_map[i],
            loss_pwl_layout=layouts[i],
        )
    return tuple(new_lines)


def _apply_loss_sos2_policy(
    lines: tuple[LineSpec, ...],
) -> tuple[LineSpec, ...]:
    """Stamp ``loss_secant_segments`` + ``loss_use_sos2`` on offender lines.

    Issue #504 task #5 — runs AFTER the adaptive K + layout passes
    (``_apply_adaptive_loss_segments`` ± ``_apply_dynamic_loss_layout``)
    so the segment count ``K`` is already finalised per-line.  The L-secant
    chord upper bound used here mirrors that ``K`` value: stamping
    ``loss_secant_segments = K`` and ``loss_use_sos2 = True`` instructs
    the gtopt LP to replace the single ``|f|`` aux with ``K`` segment
    cols and an SOS2 fill-order lock when (and only when) the line
    routes to ``line_losses_mode = tangent_signed_flow``.

    Sources (composed via UNION):

      * ``GTOPT_LOSS_SOS2_LINES`` (``--loss-sos2-lines``): explicit
        comma-separated line names.  Always honoured, no heuristic.
      * ``GTOPT_LOSS_SOS2_AUTO``  (``--loss-sos2-auto``): auto-rule
        selecting offenders by peak loss ``L_max = R·envelope²``.
          - ``off`` (default) ⇒ no auto pick.
          - ``heavy`` ⇒ top quartile by peak loss (≥ 75th percentile).
          - ``all-lossy`` ⇒ every line with ``R > 0`` and non-zero
            peak loss.

    No-op when both sources are empty / off — lines are returned
    unchanged so the policy is opt-in and cannot regress existing
    bundle outputs.  Zero-loss lines (R == 0 or envelope == 0) are
    never stamped: an L-secant chord would be vacuous (loss ≡ 0
    anyway) and SOS2 would add useless MIP cuts.
    """
    import os as _os

    explicit_raw = _os.environ.get("GTOPT_LOSS_SOS2_LINES", "").strip()
    explicit = {n.strip() for n in explicit_raw.split(",") if n.strip()}

    auto_mode = _os.environ.get("GTOPT_LOSS_SOS2_AUTO", "off").strip().lower()
    if auto_mode == "":
        auto_mode = "off"

    # Fast-path: nothing requested → no stamping, no allocation.
    if not explicit and auto_mode == "off":
        return lines

    def _peak_loss(ln: LineSpec) -> float:
        """Mirror ``_apply_adaptive_loss_segments._peak_loss`` (same envelope
        precedence: original tmax + DLR profile peak + soft-cap headroom
        when ``GTOPT_LOSS_EXTEND_OVERLOAD`` is set)."""
        if ln.resistance <= 0.0:
            return 0.0
        profile_peak = (
            max(abs(x) for x in ln.tmax_ab_profile) if ln.tmax_ab_profile else 0.0
        )
        base_fmax = max(abs(ln.tmax_ab), abs(ln.tmin_ab), profile_peak)
        extend_overload = _os.environ.get(
            "GTOPT_LOSS_EXTEND_OVERLOAD", "0"
        ).strip() in (
            "1",
            "true",
            "yes",
            "on",
        )
        if extend_overload:
            if ln.soft_cap_lifted:
                fmax = 4.0 * base_fmax
            elif ln.soft_cap:
                fmax = 2.0 * base_fmax
            else:
                fmax = base_fmax
        else:
            fmax = base_fmax
        return ln.resistance * fmax * fmax

    losses = [(i, ln, _peak_loss(ln)) for i, ln in enumerate(lines)]
    auto_idx: set[int] = set()

    if auto_mode == "all-lossy":
        auto_idx = {i for i, _, L in losses if L > 0.0}
    elif auto_mode == "heavy":
        lossy_only = [(i, L) for i, _, L in losses if L > 0.0]
        if lossy_only:
            sorted_L = sorted(L for _, L in lossy_only)
            # Top-quartile threshold: ≥ 75th percentile (nearest-rank,
            # inclusive at the cut — small populations include the
            # whole top group).  A 4-line lossy set picks 1; a 12-line
            # set picks 3; a single lossy line picks itself.
            idx_p75 = max(0, (3 * len(sorted_L)) // 4)
            threshold = sorted_L[idx_p75]
            auto_idx = {i for i, L in lossy_only if L >= threshold}

    # Build name→index for the explicit list and intersect with lossy
    # lines (skip stamping on R=0 lines even if the user named them —
    # vacuous chord, no MIP benefit, document it via the skip).
    name_to_lossy_idx = {ln.name: i for i, ln, L in losses if L > 0.0}
    explicit_idx = {name_to_lossy_idx[n] for n in explicit if n in name_to_lossy_idx}

    stamp_idx = auto_idx | explicit_idx
    if not stamp_idx:
        return lines

    new_lines = list(lines)
    for i in stamp_idx:
        ln = new_lines[i]
        # Mirror the line's per-line K (set by _apply_adaptive_loss_segments).
        # Skip if K is 0 (lossless OR adaptive pass declined to stamp);
        # an L=0 chord stamping would be ignored by gtopt anyway.
        k = ln.loss_segments
        if k <= 1:
            continue
        new_lines[i] = dataclasses.replace(
            ln,
            loss_secant_segments=k,
            loss_use_sos2=True,
        )
    return tuple(new_lines)


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
    # PLEXOS Battery.Max Power is a placeholder (2.0 MW on every CEN
    # PCP battery) — the real per-period MW rating lives in
    # ``Gen_Rating.csv`` against the ``BAT_<name>`` Generator object
    # that PLEXOS uses to model the storage's electrical port.
    # Without this fallback every gtopt battery gets pmax=2 MW
    # regardless of actual nameplate, losing ~2.8 GW of dispatch
    # capacity on the CEN PCP weekly bundle (BAT_DEL_DESIERTO real =
    # 198 MW vs gtopt = 2 MW, BAT_COYA_FV real = 141 MW vs gtopt = 2
    # MW, etc.).  Read Gen_Rating once here and consult it whenever
    # the static Max Power looks like the placeholder.
    gen_rating: dict[str, list[float]] = (
        read_long(bundle.csv("Gen_Rating.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_Rating.csv")
        else {}
    )
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
        # Override the placeholder Max Power with the Gen_Rating peak
        # for the matching ``BAT_<name>`` Generator (the canonical
        # location for the battery's actual per-period MW rating).
        _power_series = gen_rating.get(batt.name) or []
        gen_rating_peak = max(_power_series) if _power_series else 0.0
        max_power = max(max_power, gen_rating_peak)
        # Keep the full per-period rating when it VARIES (battery DLR) so the
        # writer can emit a per-block pmax_charge/pmax_discharge profile instead
        # of the scalar peak (which over-states power in the de-rated blocks).
        battery_power_profile = (
            tuple(_power_series) if len(set(_power_series)) > 1 else ()
        )
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
        # PLEXOS ``Max Cycles Day``: daily energy-throughput limit N
        # (cycles/day).  1.0 for all 41 CEN PCP batteries.  Mapped to
        # gtopt's ``Battery.max_cycles_day`` (HARD Σ discharge·Δt ≤
        # N·capacity per day, not a cost).  0.0 ⇒ no limit emitted.
        max_cycles_day = (
            db.static_property("Battery", batt.object_id, "Max Cycles Day") or 0.0
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
        # Pin end-of-horizon SoC to start-of-horizon SoC by default.
        # Without this, ``efin=0`` lets the LP freely bank energy
        # across the horizon (an off-spec terminal value that drives
        # BESS net-charge by ~12 GWh on the CEN PCP weekly bundle);
        # pinning ``efin=eini`` forces the LP to return the battery
        # to its initial state.  PLEXOS's actual MIP allows a small
        # net-discharge across the horizon (≈568 MWh / −0.6% of total
        # cycling on CEN PCP weekly 2026-04-22) — when calibrating
        # against PLEXOS, set ``GTOPT_BATTERY_PIN_EFIN=0`` (or the
        # ``--no-battery-efin-pin`` CLI flag) to drop the pin and let
        # gtopt's LP match PLEXOS's flexible terminal SoC.  The
        # default ``GTOPT_BATTERY_PIN_EFIN=1`` keeps the historic
        # behaviour (efin=eini hard pin) so existing test cases
        # don't drift.
        import os as _os_efin

        _pin = _os_efin.environ.get("GTOPT_BATTERY_PIN_EFIN", "1").strip()
        pin_efin = _pin not in ("0", "false", "False", "no", "NO", "off")
        out.append(
            BatterySpec(
                object_id=batt.object_id,
                name=batt.name,
                bus_name=bus_name,
                emin=0.0,
                emax=emax_mwh,
                eini=eini,
                efin=eini if pin_efin else 0.0,
                pmax_charge=max_power,
                pmax_discharge=max_power,
                power_profile=battery_power_profile,
                pmin_charge=pmin_charge,
                pmin_discharge=pmin_discharge,
                max_cycles_day=max_cycles_day,
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
    # DEBUG-ONLY: pin gtopt's ``Reservoir.efin`` to the PLEXOS-solution
    # End Volume (prop 646 at the LAST horizon period).  This reads the
    # SOLVED reservoir trajectory endpoint out of the solution .accdb —
    # i.e. it curve-fits gtopt's terminal storage to PLEXOS's answer, so it
    # is gated behind ``GTOPT_USE_PLEXOS_EFIN`` / ``--use-plexos-efin``
    # (default OFF), exactly like ``--use-plexos-commit`` /
    # ``--use-plexos-gen-cap``.  By DEFAULT the end-of-horizon target is the
    # INPUT last-day floor from ``Hydro_MinVolume.csv`` (the operational
    # target PLEXOS encodes by raising the min-volume floor at the final
    # period — e.g. CIPRESES 54.59 → 598.75), applied in the
    # ``elif emin_series`` branch below.  This keeps a standard convert
    # purely input-driven (no solution dependency for reservoir state).
    solution_efin: dict[str, float] = {}
    import os

    use_plexos_efin = os.environ.get("GTOPT_USE_PLEXOS_EFIN", "0").lower() in (
        "1",
        "true",
        "yes",
    )
    if (
        use_plexos_efin
        and bundle.accdb_cache_dir is not None
        and bundle.accdb_cache_dir.is_dir()
    ):
        from .plexos_block_layout import extract_storage_solution_efin

        sol = extract_storage_solution_efin(bundle.accdb_cache_dir)
        if sol:
            solution_efin = sol
            logger.info(
                "extract_reservoirs: GTOPT_USE_PLEXOS_EFIN=1 — pinned efin to "
                "PLEXOS-solution End Volume for %d storages (DEBUG; default "
                "uses the Hydro_MinVolume.csv last-day input floor)",
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
        # PLEXOS ``Hydro_*Volume.csv`` ships a TRUE per-period operational
        # floor / cap series (one value per hour).  Earlier code collapsed
        # this to a single scalar (``emin = static_emin``,
        # ``emax = min(emax_series)``), SILENTLY losing the time variation:
        #   * CANUTILLAR Max Volume steps 12,330.8 (h1-23) → 10,569.6
        #     (h24+) — the early high cap was discarded, so gtopt could
        #     never fill above the constant 10,570 even though PLEXOS holds
        #     ~10,690 in the first day.
        #   * ELTORO / L_Maule Min Volume step from 0 to an operational
        #     end-of-day floor (18,346 at h24; 9,007 at h94) which the
        #     scalar floor (static_emin) never enforced.
        # We now emit the FULL per-block profile, aggregating the hourly
        # series to gtopt blocks with the most-restrictive reducer:
        #   * emin → MAX over the block's hours (highest floor binds), then
        #     clamped at or above the static physical floor.
        #   * emax → MIN over the block's hours (lowest cap binds), bounded
        #     by the static physical cap when one is defined.
        # This keeps every block's bound feasible (it never tightens beyond
        # the worst hour inside the block) while preserving the real
        # operational variation.  The scalar fallback (``emin`` / ``emax``
        # above) is kept for reservoirs whose series is absent or constant.
        #
        # Whether the per-block emin BINDS in the LP is gated C++-side by
        # ``model_options.strict_storage_emin`` (default true for
        # plexos2gtopt's monolithic runs; plp2gtopt emits false for SDDP
        # iter-0 feasibility).  emax always binds per-block.
        #
        # ``GTOPT_EMIN_EOD_DAY1`` (``--emin-eod-day1``) is retained as a
        # legacy *restriction*: when set, only the end-of-day-1 (hour 24)
        # CSV floor is honoured for emin (all other hours fall back to the
        # static floor) — the old conservative behaviour for bundles where
        # the full per-block floor over-constrained a reservoir lacking an
        # nphi safety valve.  Default OFF ⇒ the full per-block floor.
        emin_profile: tuple[float, ...] = ()
        emax_profile: tuple[float, ...] = ()
        block_layout = getattr(bundle, "block_layout", ())
        if block_layout and (emin_series or emax_series):
            _eod_day1 = os.environ.get("GTOPT_EMIN_EOD_DAY1", "0") in (
                "1",
                "true",
                "True",
            )
            emin_per_block: list[float] = []
            emax_per_block: list[float] = []
            for intervals in block_layout:
                # 1-indexed hour intervals → 0-indexed CSV slot.  Under the
                # legacy --emin-eod-day1 restriction only hour 24 feeds the
                # emin floor; otherwise every hour in the block does.
                emin_hours = [
                    emin_series[h - 1]
                    for h in intervals
                    if 0 < h <= len(emin_series)
                    and emin_series[h - 1] > 0.0
                    and (not _eod_day1 or h == 24)
                ]
                emax_hours = [
                    emax_series[h - 1]
                    for h in intervals
                    if 0 < h <= len(emax_series) and emax_series[h - 1] > 0.0
                ]
                emin_per_block.append(
                    max([static_emin] + emin_hours) if emin_hours else static_emin
                )
                emax_per_block.append(
                    min([static_emax] + emax_hours)
                    if emax_hours and static_emax > 0.0
                    else (min(emax_hours) if emax_hours else static_emax)
                )
            # Only emit the profile when it actually varies across blocks
            # (otherwise the scalar emin/emax suffices and stays smaller).
            if len(set(emin_per_block)) > 1:
                emin_profile = tuple(emin_per_block)
            if len(set(emax_per_block)) > 1:
                emax_profile = tuple(emax_per_block)
        # ── End-of-horizon target ─────────────────────────────────────
        # DEFAULT (input-driven): the LAST-day end-of-day floor from
        # ``Hydro_MinVolume.csv`` (PLEXOS slot ``bundle.n_days * 24 - 1``).
        # PLEXOS encodes the end-of-horizon storage target by RAISING the
        # min-volume floor at the final period (e.g. CIPRESES 54.59 →
        # 598.75), so this input value IS the operational terminal target.
        # DEBUG override (``solution_efin``, populated only under
        # ``--use-plexos-efin``): the PLEXOS-SOLVED End Volume (prop 646),
        # which curve-fits gtopt's reservoir trajectory to PLEXOS's answer.
        # When the override is off (the default) ``solution_efin`` is empty
        # and the input last-day floor below is used — no solution
        # dependency for reservoir state.
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
        # cost of stored water.  In gtopt this scalar is now SUPERSEDED
        # by the per-reservoir piecewise slopes sourced from
        # ``Hydro_StoWaterValues.csv`` and emitted as a single
        # end-of-horizon boundary cut (FCF + per-reservoir water-value
        # slopes; wired via ``simulation.boundary_cuts_file``).  The
        # boundary cut is the authoritative terminal-volume pricing
        # surface — pricing storage above ``efin`` correctly under all
        # cases — so the legacy scalar ``Water Value`` + sentinel
        # ``never_drain`` clamp is obsolete.
        #
        # ``keep_sentinel`` defaults to False → the PLEXOS ``1e+30``
        # marker silently drops to ``None`` → ``water_value_gwh = 0.0``.
        # The boundary cut prices terminal storage uniformly across all
        # reservoirs (efin is always SOFT; the slack cost is derived in
        # the gtopt C++ side from boundary_cuts.csv).
        water_value_gwh = (
            db.static_property("Storage", storage.object_id, "Water Value") or 0.0
        )
        # ``never_drain`` disables the reservoir's drain (spill) variable
        # (writer sets ``spillway_capacity = 0``) so water can leave ONLY
        # through turbines, never spilled.  Reserved for ELTORO — the head
        # of the Laja cascade, which PLEXOS forces to refill to ``efin``
        # and never spills; without this guard a global spill mode
        # (``GTOPT_RESERVOIR_SPILL``) or an added ``Vert_*`` waterway would
        # give it a free spill escape.  NOT a hard-efin and unrelated to
        # terminal pricing (efin stays soft, cost derived in C++).
        never_drain = name in _NEVER_DRAIN_RESERVOIRS
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


def _vert_waterways_referenced_by_constraints(db: PlexosDb) -> frozenset[str]:
    """Return the names of every ``Vert_*`` waterway referenced by some
    PLEXOS UserConstraint via a ``Constraints`` membership (i.e. carrying
    a ``Flow Coefficient`` for that constraint).

    These spillway arcs MUST stay emitted as real ``Waterway`` rows in
    ``case.waterways`` rather than being collapsed onto
    ``Junction.drain_*``: collapsing them drops the LP column the
    constraint term binds to, forcing the converter to omit
    ``waterway("Vert_<X>").flow`` and leaving a stricter gen-only LHS
    than PLEXOS uses (e.g. ``ANGOSTURAeco`` becomes ``gen ≥ 38.5``
    instead of the faithful ``gen + 0.43·Vert_ANGOSTURA.flow ≥ 38.5``).

    Kept arcs are routed by the consumer
    (``extract_waterways``) per **PLEXOS-published topology**: when the
    Waterway carries a ``Storage To`` membership the arc connects
    source junction → that downstream reservoir (cascade routing,
    matches PLEXOS's water balance — e.g. ``Vert_PANGUE → ANGOSTURA``,
    ``Vert_B_Maule → COLBUN``); when no ``Storage To`` exists the arc
    falls back to a synthetic ``<source>_ocean`` drain junction
    (``Vert_ANGOSTURA`` is genuinely terminal in PLEXOS).  Active
    constraints are already emitted with ``penalty soft_floor_penalty``
    so a tighter LHS can never produce infeasibility — only a penalty
    (matches the "hydro min-flow must be soft" rule).

    The set is derived from the PLEXOS input — NOT a fixed allowlist —
    so if a bundle adds another ``Vert_*``-referencing constraint the
    converter auto-keeps that waterway too.
    """
    coll = db.collection_for_named("Waterway", "Constraint", "Constraints")
    if coll is None:
        return frozenset()
    p2c = db.parent_to_children(coll.collection_id)
    objs = db.object_by_id()
    return frozenset(
        objs[wid].name
        for wid, child_ids in p2c.items()
        if child_ids and wid in objs and objs[wid].name.startswith("Vert_")
    )


def _reservoir_is_pure_pondage(r: ReservoirSpec) -> bool:
    """True when a Reservoir is a pass-through / RoR pondage node.

    A reservoir qualifies for Junction-only demotion when its storage
    envelope is entirely zero AND nobody attaches a water-value /
    penalty / efin target / profile to it.  ``eini = 0`` is the
    discriminator: real reservoirs with any initial volume (PANGUE,
    MACHICURA, POLCURA, …) stay Reservoirs even if other fields are
    zero.  Shared by :func:`extract_case` (the actual demotion) and by
    the early real-reservoir set passed to :func:`extract_waterways`
    so the ``Vert_*`` spill collapse can avoid putting a drain on a
    real reservoir's own junction (drains belong on ending/ocean
    junctions only).
    """
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
    )


def extract_waterways(
    db: PlexosDb,
    bundle: PlexosBundle | None = None,
    forced_targets_out: list[tuple[str, str, float, tuple[float, ...]]] | None = None,
    ocean_sources_out: set[str] | None = None,
    junction_drain_configs_out: dict[str, dict[str, float | None]] | None = None,
    real_reservoir_names: frozenset[str] | None = None,
    reservoir_drain_enabled: bool = True,
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
    # ``Vert_*`` waterways referenced by some PLEXOS UserConstraint via
    # a ``Constraints`` membership — these MUST NOT be collapsed onto a
    # ``Junction.drain_*`` row (the constraint's ``waterway(...).flow``
    # term would then have no LP column to bind to and would be dropped,
    # producing a stricter gen-only LHS than PLEXOS uses).  Auto-derived
    # from the input so adding a new spillway constraint in PLEXOS
    # transparently keeps its waterway emitted.
    keep_as_waterway: frozenset[str] = _vert_waterways_referenced_by_constraints(db)
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
    # Spill-out ``Vert_*`` arcs dropped because the per-storage internal
    # drain (``Reservoir.spillway_cost``) is enabled and now provides the
    # single way out of the basin — keeping the ocean arc too would give the
    # LP a redundant escape path to arbitrage.  In-basin cascade arcs are
    # NOT dropped (they route water to a real downstream junction).
    dropped_spill_out: list[str] = []
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
            #
            # EXCEPTION: ``Vert_*`` arcs in ``keep_as_waterway`` are
            # explicitly referenced by UserConstraint Flow Coefficient
            # memberships (e.g. ``ANGOSTURAeco`` carries
            # ``0.43 * waterway("Vert_ANGOSTURA").flow``).  Collapsing
            # them would force the converter to drop the term and emit
            # a stricter gen-only LHS than PLEXOS.  Skip the collapse
            # for those names and fall through to the ocean-redirect
            # path so the Waterway is emitted (LP-equivalent: same
            # ``fmax``/``fcost``, spill still leaves the basin via the
            # synthetic ``<source>_ocean`` drain Junction).  The set is
            # auto-derived from the PLEXOS input — not a fixed allowlist
            # — so this stays correct when new spillway constraints land.
            # DOMAIN RULE (2026-06): a ``Junction.drain`` belongs ONLY on
            # the ENDING / ocean junctions, never on a real reservoir's
            # own junction.  A reservoir already disposes of excess water
            # through its internal ``spillway_cost`` drain
            # (storage_lp.cpp) — collapsing ``Vert_<reservoir>`` onto the
            # source junction would put a SECOND, redundant drain on a
            # node that does not need one, and (worse) lets the LP dump a
            # real reservoir's whole stored volume out of the basin for
            # free, suppressing downstream turbine dispatch (RALCO,
            # COLBUN, PEHUENCHE, …).  So when the spillway source is a
            # REAL reservoir (survives the pondage/RoR demotion below),
            # SKIP the collapse and fall through to the ocean-redirect
            # path: the drain then lands on the synthetic
            # ``<source>_ocean`` ENDING junction, where it belongs.
            #
            # Pass-through / RoR pondage junctions (zero-storage nodes
            # that get demoted to Junction-only — ISLA, LAJA_I, RUCUE, …)
            # are NOT in ``real_reservoir_names``.
            _src_is_real_reservoir = (
                real_reservoir_names is not None and f_name in real_reservoir_names
            )
            # MASS-CONSERVATION FIX (2026-06): a zero-storage pass-through
            # node CANNOT lose water — collapsing its ``Vert_*`` spill onto
            # a ``Junction.drain`` makes excess inflow VANISH instead of
            # flowing downstream (e.g. on 20251005 RUCUE took 14,606,
            # turbined 9,711 → QUILLECO and DRAINED the 1,317 excess away;
            # PLEXOS routes ``Vert_RUCUE`` → LAJA_I, keeping the spill in
            # the Laja cascade).  So for a pass-through SOURCE we DO NOT
            # drain: we emit ``Vert_*`` as a REAL Waterway from its
            # ``Storage From`` to its ``Storage To`` (PLEXOS membership),
            # letting the excess flow downstream where it can be re-turbined
            # or spilled further down the cascade.
            #
            # CAVEAT: if the pass-through spill's ``Storage To`` is a REAL
            # reservoir, routing into it would re-introduce the
            # reservoir→reservoir spill arbitrage the ocean-redirect path
            # below guards against (the LP could spill the upstream node to
            # cheaply fill a downstream storage).  Routing into ANOTHER
            # pass-through (RUCUE→LAJA_I, …) creates no such arbitrage
            # because a zero-storage node cannot accumulate.  So only keep
            # the spill as a downstream waterway when the destination is
            # itself a pass-through; otherwise fall back to the drain/ocean
            # path.
            _dst_is_real_reservoir = (
                real_reservoir_names is not None
                and t_name is not None
                and t_name in real_reservoir_names
            )
            _passthrough_route_downstream = (
                not _src_is_real_reservoir
                and ww.name not in keep_as_waterway
                and t_name is not None
                and not _dst_is_real_reservoir
            )
            if _passthrough_route_downstream:
                # Emit Vert_* as a real downstream Waterway (no drain).
                # Fall through to the normal WaterwaySpec emission below
                # with the PLEXOS-published ``t_name`` kept as junction_b.
                pass
            elif (
                junction_drain_configs_out is not None
                and ww.name not in keep_as_waterway
                and not _src_is_real_reservoir
            ):
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
            # ──────────────────────────────────────────────────────────
            # UC-referenced spillways (``keep_as_waterway``) default to
            # cascade routing — i.e. keep the PLEXOS-published
            # ``Storage To`` as the downstream junction — for topology
            # faithfulness with the cascade water balance the UC's
            # ``waterway(...).flow`` term refers to.
            #
            # Rationale: the dispatch-matching ocean default above was
            # justified for the *uniform* ocean-redirect of all
            # ``Vert_*`` arcs; for the *selective* UC-referenced keep
            # set the trade-off shifts.  Because PLEXOS itself attaches
            # a Flow Coefficient to these arcs, the constraint physics
            # cares about where the spilled water ends up — over-
            # routing to ocean creates a "water lost forever" penalty
            # the LP feels (e.g. ``PEHUENCHEmin``: ocean-redirecting
            # ``Vert_B_Maule`` makes spilling free-money lost and
            # pushes the LP to over-turbine PEHUENCHE to meet the
            # ≥ 18 floor, whereas PLEXOS recovers the spill at COLBUN).
            #
            # Falls back to ocean when PLEXOS publishes no
            # ``Storage To`` (genuinely terminal arc — e.g.
            # ``Vert_ANGOSTURA``).  ``GTOPT_VERT_ROUTING`` stays as a
            # manual override for diagnostic / dispatch-tuning runs.
            # ──────────────────────────────────────────────────────────
            kept_default_cascade = ww.name in keep_as_waterway
            if (
                _routing == "cascade"
                or kept_default_cascade
                or _passthrough_route_downstream
            ) and t_name is not None:
                # Keep the PLEXOS-published junction_b — no override.
                # ``_passthrough_route_downstream`` reaches here from the
                # mass-conservation fix above: a zero-storage source spilling
                # into another pass-through must route the water downstream,
                # never to ocean (which would re-introduce the lost-water bug).
                #
                # IN-BASIN CASCADE arcs (``Vert_PANGUE → ANGOSTURA``,
                # ``Vert_RUCUE → LAJA_I``, …) are genuine downstream routing,
                # not an escape path: they MUST be kept even when the
                # per-storage internal drain is enabled.
                pass
            elif reservoir_drain_enabled and not kept_default_cascade:
                # SPILL-OUT arc (sink-bound): this branch is reached only when
                # the arc was NOT routed in-basin above — i.e. it would have
                # been rewritten to a synthetic ``<source>_ocean`` drain that
                # carries water OUT of the basin.  With the per-storage
                # internal drain re-enabled (the new default, set on every
                # reservoir except ELTORO by ``build_reservoir_array``), that
                # ocean arc is a SECOND, redundant escape path — keeping both
                # gives the LP two equivalent valves to arbitrage between under
                # degeneracy.  Drop the spill-out arc entirely (and never
                # synthesise its ``<source>_ocean`` junction) so the reservoir's
                # own ``spillway_cost`` column is the single way out of the
                # basin.  Net: exactly ONE escape path per reservoir.
                #
                # EXCEPTION: UC-referenced arcs (``keep_as_waterway``) are NOT
                # dropped even when terminal — a PLEXOS UserConstraint Flow
                # Coefficient term (``waterway("Vert_<X>").flow``) needs the LP
                # column to bind to.  Those fall through to the ocean-redirect
                # else branch so the Waterway (and its ocean junction) survive.
                dropped_spill_out.append(ww.name)
                continue
            else:
                # Drain DISABLED (legacy ``Vert_*`` → ocean topology): emit the
                # spill-out arc and synthesise its ocean drain junction.
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
        # A ``Max Flow`` of the PLEXOS ``1e+30`` sentinel means UNBOUNDED
        # (no cap), NOT zero — it folds to 0.0 above.  Capture the raw value
        # so the "inactive diversion" rule below does not mistake an
        # uncapped extraction arc (e.g. ``Ext_Maule``: L_Maule -> LA_MINA,
        # Max Flow 1e+30) for a genuinely-zero one and close it.
        fmax_is_unbounded = (
            abs(
                db.static_property(
                    "Waterway", ww.object_id, "Max Flow", keep_sentinel=True
                )
            )
            >= 1.0e20
        )
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
        # Consumptive 1-ended forced diversion (Riego_/Caudal_Eco_/Filt_/Ext_
        # draining to a synthetic ``*_sink``): emit a SOFT FlowRight at the
        # SOURCE junction instead of a HARD fmin=fmax waterway, so a dry month
        # can under-deliver at the water-fail cost rather than going
        # infeasible (e.g. 03-15 LAJA_I).  The FlowRight is consumptive on
        # ``f_name`` — it debits the source balance exactly as the waterway
        # did; the sink credit was irrelevant (water leaves the basin).  The
        # full per-block profile is carried so a target that pauses (e.g.
        # irrigation only on day 1) is honoured block-by-block.
        t_name_is_sink = t_name is not None and t_name.endswith("_sink")
        if (
            forced_targets_out is not None
            and has_forced
            and forced_target > 0.0
            and not is_bypass
            and t_name_is_sink
        ):
            forced_targets_out.append((ww.name, f_name, forced_target, tuple(forced)))
            if t_name in synthetic_sinks:
                synthetic_sinks.remove(t_name)
            continue
        # INACTIVE diversion waterway — ``Caudal_Eco_*`` / ``Riego_*`` /
        # ``Ext_*`` whose Hydro_WaterFlows.csv column is all zeros AND
        # whose PLEXOS Min Flow / Max Flow static properties are unset.
        # Without fmin/fmax the writer leaves the arc UNCAPPED (default
        # ``fmax`` -> +inf), so the LP can discover it as a free water
        # path and drain the upstream reservoir (e.g. ``Ext_Maule``:
        # L_Maule -> LA_MINA).  The correct boundary-cut water value
        # currently keeps it idle, but pinning ``fmax = 0`` closes the
        # path structurally (mass-conservation hardening).
        _diversion_prefixes = ("Caudal_Eco", "Riego_", "Ext_")
        inactive = (
            ww.name.startswith(_diversion_prefixes)
            and not has_forced
            and not (fmin and fmin > 0.0)
            and not (fmax and fmax > 0.0)
            and not fmax_is_unbounded  # 1e30-sentinel = uncapped, not zero
        )
        if inactive:
            fmin = 0.0
            fmax = 0.0
            logger.info(
                "extract_waterways: inactivating %s (%s -> %s) — diversion "
                "waterway with zero CSV and no static Min/Max Flow; pin "
                "fmax = 0 so it can't leak water.",
                ww.name,
                f_name,
                t_name,
            )
        out.append(
            WaterwaySpec(
                object_id=ww.object_id,
                name=ww.name,
                storage_from=f_name,
                storage_to=t_name,
                fmin=fmin,
                fmax=fmax,
                forced_flow_profile=forced_profile,
                inactive=inactive,
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
    if dropped_spill_out:
        logger.info(
            "extract_waterways: dropped %d spill-out Vert_* arc(s) (sink-bound "
            "<source>_ocean) because the per-storage internal drain is enabled "
            "— the reservoir's own spillway_cost column is the single basin "
            "exit, so the ocean arc would be a redundant escape path; arcs: %s",
            len(dropped_spill_out),
            ", ".join(sorted(dropped_spill_out)),
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
    # ``csv_pf`` keeps the scalar (first nonzero, the constant /
    # fallback value); ``csv_pf_profile`` keeps the FULL per-period
    # series so the writer can emit a per-(stage, block) profile when
    # the head-dependent PF genuinely varies — gtopt
    # ``Turbine.production_factor`` is now per-block
    # (``OptTBRealFieldSched``), so the variation is no longer dropped.
    csv_pf: dict[str, float] = {}
    csv_pf_profile: dict[str, tuple[float, ...]] = {}
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
                        csv_pf_profile[k] = tuple(vals)
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
                pf_profile=csv_pf_profile.get(gen_obj.name, ()),
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


def _parse_res_timeslice_csv(path: Path, n_days: int = 1) -> list[str]:
    """Parse ``Res_Timeslice.csv`` → the active day-type slice per day.

    Layout: ``YEAR, MONTH, DAY, <slice_1>, <slice_2>, …`` with one row
    per calendar day.  Exactly one slice column carries the activation
    sentinel (``-1`` in CEN PCP; ``1`` is also accepted) marking the
    day-type pattern (``DO_1``/``LU_2``/``SA_2``/``TR_2`` — Domingo /
    Lunes / Sábado / Trabajo, variants 1-2) that governs that day's
    reserve requirements in :func:`_parse_res_requirement_csv`.

    Returns a list of ``n_days`` slice names (e.g.
    ``["TR_2", "TR_2", "TR_2", "SA_2", "DO_2", "LU_2", "TR_2"]``).  Days
    beyond the CSV (or with no active slice) get ``""`` so the caller
    falls back to its last-wins behaviour for those days.
    """
    slices: list[str] = []
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if header is None:
            return [""] * n_days
        slice_cols = header[3:]  # skip YEAR, MONTH, DAY
        for row in reader:
            active = ""
            for i, col in enumerate(slice_cols, start=3):
                if i < len(row) and row[i].strip() in ("-1", "1"):
                    active = col.strip()
                    break
            slices.append(active)
    # Pad / truncate to the LP horizon so each stage-day has a slice.
    if len(slices) < n_days:
        slices += [""] * (n_days - len(slices))
    return slices[:n_days]


def _parse_res_requirement_csv(
    path: Path,
    reserve_names: frozenset[str],
    n_days: int = 1,
    day_slices: list[str] | None = None,
) -> dict[str, list[float]]:
    """Parse ``Res_Requirement.csv``'s ``NAME, PATTERN, VALUE`` layout.

    Only rows whose ``NAME`` matches a known Reserve object are kept.
    PATTERN ``"<slice>,Hh"`` carries the day-type slice (``DO_1`` …
    ``TR_2``) and the hour ``Hh`` (1..24).

    When ``day_slices`` is supplied (from :func:`_parse_res_timeslice_csv`)
    the requirement is resolved **per calendar day**: day ``d`` uses the
    24-hour profile of ``day_slices[d]`` so weekdays / weekends / holidays
    get their distinct reserve targets.  Without it (legacy / no
    ``Res_Timeslice.csv``) the day-type field is ignored and the last-seen
    value per hour is replicated across all days.

    Returns ``{reserve_name -> (24*n_days)-element profile}`` matching the
    LP horizon; otherwise gtopt's ``FieldSched::optval`` reads past the
    array end (observed as ``2.83e+256`` lower bounds on
    ``reservezone_drequirement_*`` columns on the 7-day CEN PCP case).
    """

    hour_re = re.compile(r"H(\d+)")
    use_slices = bool(day_slices) and any(day_slices or [])
    # Per-reserve, per-(slice, hour) value table — only populated when we
    # have a timeslice mapping; otherwise we keep the flat last-wins path.
    by_slice: dict[str, dict[tuple[str, int], float]] = {}
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
            if use_slices:
                slc = pattern.split(",")[0].strip().strip('"')
                by_slice.setdefault(name, {})[(slc, hour)] = value
            else:
                series = out.setdefault(name, [0.0] * 24)
                series[hour - 1] = value

    if use_slices:
        assert day_slices is not None
        for name, table in by_slice.items():
            profile: list[float] = []
            for d in range(n_days):
                slc = day_slices[d] if d < len(day_slices) else ""
                for hour in range(1, 25):
                    # Fall back to any slice's value for this hour when the
                    # active slice is missing (empty day or absent row).
                    val = table.get((slc, hour))
                    if val is None:
                        val = next((v for (s, h), v in table.items() if h == hour), 0.0)
                    profile.append(val)
            out[name] = profile
        return out

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
    # Day-type slice per calendar day (``Res_Timeslice.csv``) selects which
    # of the 8 day-type reserve-requirement patterns governs each day, so
    # weekday / weekend / holiday targets are honoured instead of collapsing
    # all 8 to a single replicated 24-hour profile.
    day_slices: list[str] | None = None
    if bundle.has("Res_Timeslice.csv"):
        day_slices = _parse_res_timeslice_csv(
            bundle.csv("Res_Timeslice.csv"), n_days=bundle.n_days
        )
    csv_profiles: dict[str, list[float]] = {}
    if bundle.has("Res_Requirement.csv"):
        csv_profiles = _parse_res_requirement_csv(
            bundle.csv("Res_Requirement.csv"),
            reserve_names,
            n_days=bundle.n_days,
            day_slices=day_slices,
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
        # Requirement = the time-varying CSV profile ONLY (mirrors PLEXOS's
        # reported per-block RHS).  The static ``Min Provision`` floor is carried
        # SEPARATELY as ur_min/dr_min → ReserveZone.urmin/drmin, so the LP
        # enforces ``Σ pf·prov ≥ max(requirement, min)`` while the reported
        # requirement schedule still matches PLEXOS (it is NOT raised by the
        # floor on low-requirement hours).  Previously this folded
        # ``max(csv, min_provision)`` into the requirement, which over-reported
        # the RHS on low hours (e.g. CTF_DownMinProvision 94→293).
        requirement = tuple(float(v) for v in profile) if profile else ()
        ur_req: tuple[float, ...] = ()
        dr_req: tuple[float, ...] = ()
        if requirement:
            if is_down:
                dr_req = requirement
            else:
                ur_req = requirement
        ur_min_provision = (
            min_provision if (min_provision > 0.0 and not is_down) else 0.0
        )
        dr_min_provision = min_provision if (min_provision > 0.0 and is_down) else 0.0
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
                ur_min_provision=ur_min_provision,
                dr_min_provision=dr_min_provision,
                eligible_generators=tuple(sorted(set(eligibility.get(rsv.name, [])))),
                plexos_type=plexos_type,
                type_tag=type_tag,
                urcost=urcost,
                drcost=drcost,
            )
        )
    return tuple(out)


def _uc_reserve_provision_gens(db: PlexosDb) -> frozenset[str]:
    """Generator names referenced by a UserConstraint ``reserve_provision``
    coefficient (the ``Generator``-parent ``*Reserve Provision
    Coefficient`` kinds in :data:`_DIRECT_COEFFS`).

    PLEXOS allows a Constraint to force a generator's reserve provision
    even when that generator is NOT a member of any Reserve→Generator
    eligibility table.  Such a gen never appears in any reserve's
    ``eligible_generators`` so :func:`extract_reserve_provisions` would
    not emit its ``provision_<gen>`` row — the constraint reference would
    dangle and trip the strict converter's hard-fail.  Collecting these
    names lets the provision extractor synthesise the missing rows.
    """
    objs = db.object_by_id()
    out: set[str] = set()
    for (
        parent_class,
        coll_suffix,
        prop_name,
        gtopt_class,
        _acc,
        _tmpl,
    ) in _DIRECT_COEFFS:
        if gtopt_class != "reserve_provision" or parent_class != "Generator":
            continue
        coll = db.collection_for_named(parent_class, "Constraint", coll_suffix)
        if coll is None:
            continue
        prop_id = db.property_by_name(coll.collection_id, prop_name)
        if prop_id is None:
            continue
        for gen_name, _coeff in (
            pair
            for pairs in _index_coefficient_rows(
                db, coll.collection_id, prop_id, objs
            ).values()
            for pair in pairs
        ):
            out.add(gen_name)
    return frozenset(out)


def extract_reserve_provisions(
    reserves: tuple[ReserveSpec, ...],
    generators: tuple[GeneratorSpec, ...] = (),
    db: PlexosDb | None = None,
    committed_gens: frozenset[str] = frozenset(),
    extra_provision_gens: frozenset[str] = frozenset(),
    bundle: PlexosBundle | None = None,
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
    # Generators carrying real capacity (scalar ``pmax > 0`` OR a
    # non-empty ``pmax_profile``).  These keep their real ``urmax`` /
    # ``drmax`` caps and the urmin/drmin floor logic below.
    capacity_gens = {
        n for n, pmax in pmax_by_gen.items() if pmax > 0.0 or pmax_varies.get(n, False)
    }
    # Emit a ``ReserveProvision`` for EVERY generator a reserve
    # references — including zero-capacity combined-cycle config
    # variants (``TOCOPILLA-TG3_GN_A``, ``…_GNL_INF``, COLMITO_DIE, …).
    # PLEXOS reserve user_constraints (``CPF/CSF/CTF*MinProvision``,
    # ``*_Max_Operativo``, ``Special*``, ``SD_*``) reference
    # ``reserve_provision("provision_<config>")`` for ALL config
    # variants of a plant; if we drop the zero-capacity ones their refs
    # dangle and the strict converter/gtopt fail hard.
    #
    # The old code filtered these out because emitting provisions with
    # nonzero bounds / urmin floors on undispatchable gens made the
    # reserve-requirement constraint primal-infeasible.  The safe shape
    # is a STRICTLY ZERO-BOUNDED provision: ``urmax = drmax = pmax``
    # (== 0 here) AND ``urmin = drmin = 0`` (no floor).  A ``[0,0]``
    # column contributes exactly 0 to any reserve sum and forces
    # nothing, so it cannot recreate that infeasibility — it just makes
    # the reference resolve (config-exclusivity means only the single
    # active config is online, so a zero-capacity config is correctly a
    # 0-contribution).
    by_gen: dict[str, list[str]] = {}
    for rsv in reserves:
        for gen_name in rsv.eligible_generators:
            # Only attach to generators that actually exist as a
            # generator (have an LP column).  A reserve referencing an
            # unknown gen name is reported elsewhere, not synthesised.
            if gen_name not in pmax_by_gen:
                continue
            by_gen.setdefault(gen_name, []).append(rsv.name)
    # Generators referenced by a ``reserve_provision("provision_<gen>")``
    # coefficient in a UserConstraint but NOT a member of any
    # Reserve→Generator eligibility table (``MACHICURA_U1/U2``,
    # ``SAN_CLEMENTE`` in ``SD_2025084573_PteNegro_Colbun`` on CEN PCP
    # v22).  PLEXOS lets a constraint reference a provision variable for
    # a non-eligibility gen; the variable simply has no reserve-zone
    # requirement attached.  Emit a zone-less provision so the reference
    # resolves: a capacity-bearing gen (``MACHICURA_U*``, pmax > 0) gets
    # its real ``urmax = drmax = pmax`` cap; a zero-capacity one
    # (``SAN_CLEMENTE``, pmax == 0) stays the safe ``[0, 0]`` column.
    for gen_name in sorted(extra_provision_gens):
        if gen_name in pmax_by_gen:
            by_gen.setdefault(gen_name, [])
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
                    # Never floor a zero-capacity config variant: its
                    # provision must stay strictly ``[0, 0]`` (no urmin /
                    # drmin) so the column can't force any dispatch and
                    # re-introduce the reserve-requirement infeasibility
                    # the legacy ``pmax > 0`` filter prevented.
                    if gen_obj.name not in capacity_gens:
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
    # Per-(gen, direction) hourly MAX RESERVE caps from CFdata/.  This
    # is the AUTHORITATIVE upper bound on ``reserve_provision.up`` /
    # ``.dn`` columns — PLEXOS sol max == CFdata cap EXACTLY across
    # every binding pair (verified v0407 sol 2026-05-31).  Without
    # this, ``urmax = drmax = pmax`` leaves the LP DV 2-16000× too
    # loose, the PLEXOS-binding reserve UCs
    # (``CPF_Up5Calculation``, ``CPF_UpMinProvision``,
    # ``CSF_UpMinProvision``) don't bind in gtopt, and the BESS
    # ``UPStorageBound_BAT_*`` rows go -inf via dual cascade.
    # ``bundle`` is optional so existing callers / tests that bypass
    # the CFdata wiring (synthetic bundles without ``CFdata/``) keep
    # the legacy ``urmax = pmax`` fallback.
    urmax_profile_by_gen: dict[str, tuple[float, ...]] = {}
    drmax_profile_by_gen: dict[str, tuple[float, ...]] = {}
    if bundle is not None and (bundle.root / "CFdata").is_dir():
        for gen_name in by_gen:
            mru = _cf_maxresp_aggregate(bundle, gen_name, "MRU")
            mrd = _cf_maxresp_aggregate(bundle, gen_name, "MRD")
            if mru:
                urmax_profile_by_gen[gen_name] = tuple(mru)
            if mrd:
                drmax_profile_by_gen[gen_name] = tuple(mrd)

    return tuple(
        ReserveProvisionSpec(
            generator_name=gen_name,
            reserve_zones=tuple(sorted(zones)),
            urmax=pmax_by_gen.get(gen_name, 0.0),
            drmax=pmax_by_gen.get(gen_name, 0.0),
            urmin=urmin_by_gen.get(gen_name, 0.0),
            drmin=drmin_by_gen.get(gen_name, 0.0),
            urmax_profile=urmax_profile_by_gen.get(gen_name, ()),
            drmax_profile=drmax_profile_by_gen.get(gen_name, ()),
        )
        for gen_name, zones in sorted(by_gen.items())
    )


def _parse_sscc_activation_bess_csv(
    path: Path, n_days: int = 1
) -> dict[str, list[float]]:
    """Parse ``SSCC_Activation_BESS.csv`` → ``{reserve_zone -> per-block frac}``.

    Layout: ``Year, Pattern, <zone_1>, <zone_2>, …`` where the zone
    columns are the ``*_BESS`` reserve names (``CPF_RS_BESS`` …) and
    ``Pattern`` is a 2-hour band ``H<a>-<b>`` spanning the 24-hour day.
    Values are activation **percentages** (0..100) → divided by 100 to a
    p.u. fraction.  The 24-hour profile is replicated ``n_days`` times to
    match the LP horizon.

    Returns ``{zone -> (24*n_days)-element fraction profile}``.
    """
    band_re = re.compile(r"H(\d+)\s*-\s*(\d+)")
    with Path(path).open("r", encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        zone_cols = [
            c for c in (reader.fieldnames or []) if c not in ("Year", "Pattern")
        ]
        daily: dict[str, list[float]] = {z: [0.0] * 24 for z in zone_cols}
        for row in reader:
            m = band_re.search(row.get("Pattern") or "")
            if m is None:
                continue
            lo, hi = int(m.group(1)), int(m.group(2))
            for z in zone_cols:
                try:
                    frac = float(row.get(z) or 0.0) / 100.0
                except ValueError:
                    continue
                for hour in range(lo, hi + 1):
                    if 1 <= hour <= 24:
                        daily[z][hour - 1] = frac
    # Drop all-zero columns; replicate the 24-hour pattern across days.
    out: dict[str, list[float]] = {}
    for z, prof in daily.items():
        if any(v != 0.0 for v in prof):
            out[z] = prof * n_days if n_days > 1 else prof
    return out


def _bess_zone_eligibility(
    db: PlexosDb, battery_names: frozenset[str]
) -> dict[str, set[str]]:
    """``{*_BESS reserve zone -> set of eligible battery names}``.

    A battery is eligible for a ``*_BESS`` reserve when it (or one of its
    ``<bat>_CF_GEN_COMP`` / ``_CF_LOAD_COMP`` reserve-companion objects) is
    a member of the reserve's membership table.
    """
    out: dict[str, set[str]] = {}
    reserves = [r for r in db.objects_of_class("Reserve") if r.name.endswith("_BESS")]
    if not reserves:
        return out
    objs = db.object_by_id()
    by_id = {r.object_id: r.name for r in reserves}
    # The eligible batteries live in the ``Reserve → Battery`` collection
    # (``Batteries``) as direct battery objects; the ``Generators``
    # collection is checked too for completeness.  ``*_CF_*`` companion
    # objects are Constraint-class and intentionally ignored here.
    for child_class, coll_name in (
        ("Battery", "Batteries"),
        ("Generator", "Generators"),
    ):
        coll = db.collection_for_named("Reserve", child_class, coll_name)
        if coll is None:
            continue
        for m in db.memberships_of(coll.collection_id):
            zone = by_id.get(m.parent_object_id)
            child = objs.get(m.child_object_id)
            if zone is None or child is None:
                continue
            if child.name in battery_names:
                out.setdefault(zone, set()).add(child.name)
    return out


def extract_sscc_bess_provisions(
    db: PlexosDb,
    bundle: PlexosBundle,
    batteries: tuple[BatterySpec, ...],
) -> tuple[ReserveProvisionSpec, ...]:
    """BESS ancillary-services provisions from ``SSCC_Activation_BESS.csv``.

    Maps the per-time-pattern activation fraction onto the synthetic
    ``<battery>_gen`` discharge generator (``system.cpp::expand_batteries``)
    that gtopt auto-creates: one :class:`ReserveProvisionSpec` per
    (battery, eligible ``*_BESS`` zone), carrying the activation fraction
    as the per-block ``ur``/``dr`` provision factor (``_RS_BESS`` →
    up-reserve, ``_LW_BESS`` → down-reserve) and the battery's discharge
    power as ``urmax``/``drmax``.  Names are made unique per zone so the
    shared ``<battery>_gen`` generator can carry several provisions.
    """
    if not bundle.has("SSCC_Activation_BESS.csv") or not batteries:
        return ()
    factors = _parse_sscc_activation_bess_csv(
        bundle.csv("SSCC_Activation_BESS.csv"), n_days=bundle.n_days
    )
    if not factors:
        return ()
    bat_by_name = {b.name: b for b in batteries}
    eligibility = _bess_zone_eligibility(db, frozenset(bat_by_name))
    out: list[ReserveProvisionSpec] = []
    for zone, frac in sorted(factors.items()):
        members = eligibility.get(zone)
        if not members:
            continue
        is_up = "_RS_BESS" in zone
        # Pick the right CFdata subdir + prefix per BESS reserve zone.
        # Each SSCC provision is PER-(battery, zone) — one LP variable
        # per pair — so the per-zone urmax must use ONLY that zone's
        # CFdata file (NOT the SUM across all reserve subdirs that
        # extract_reserve_provisions uses for the single-column gen
        # case).  Mapping:
        #   CPF_*_BESS → CFdata/CPF/CPF_<bat>_<MRU|MRD>.csv
        #   CSF_*_BESS → CFdata/CSF/CSF_<bat>_<MRU|MRD>.csv
        #   CTF_*_BESS → CFdata/CTF/CTFON_<bat>_<MRU|MRD>.csv
        # If the zone family doesn't match (e.g. a future non-CPF/CSF/
        # CTF BESS zone), leave the profile empty so the writer falls
        # back to the scalar ``pmax_discharge`` legacy bound.
        if zone.startswith("CPF_"):
            cf_subdir, cf_prefix = "CPF", "CPF"
        elif zone.startswith("CSF_"):
            cf_subdir, cf_prefix = "CSF", "CSF"
        elif zone.startswith("CTF_"):
            cf_subdir, cf_prefix = "CTF", "CTFON"
        else:
            cf_subdir, cf_prefix = "", ""
        for bname in sorted(members):
            batt = bat_by_name.get(bname)
            power = batt.pmax_discharge if batt else 0.0
            if power <= 0.0:
                continue
            prof = tuple(frac)
            # Per-block MAX RESERVE CAPABILITY from CEN's CFdata for
            # this SPECIFIC (battery, zone) pair only.  PLEXOS files
            # are named under the battery (e.g.
            # ``CFdata/CPF/CPF_BAT_LA_CABANA_EO_MRU.csv``), NOT under
            # the synthetic ``<battery>_gen`` discharge gen gtopt
            # creates — so we look up against ``bname``.
            direction = "MRU" if is_up else "MRD"
            bess_profile: tuple[float, ...] = ()
            if cf_subdir and (bundle.root / "CFdata").is_dir():
                series = _cf_maxresp_series(
                    bundle, bname, cf_subdir, cf_prefix, direction
                )
                bess_profile = tuple(series)
            out.append(
                ReserveProvisionSpec(
                    generator_name=f"{bname}_gen",
                    reserve_zones=(zone,),
                    urmax=power if is_up else 0.0,
                    drmax=0.0 if is_up else power,
                    ur_provision_factor_profile=prof if is_up else (),
                    dr_provision_factor_profile=() if is_up else prof,
                    urmax_profile=bess_profile if is_up else (),
                    drmax_profile=() if is_up else bess_profile,
                    name=f"provision_{bname}_gen__{zone}",
                )
            )
    return tuple(out)


#: CFdata reserve subdirs + the prefix PLEXOS uses for each file.
#: CTFON (tertiary, ON-line) — files ``CTFON_<gen>_{MRU,MRD}.csv``.
#: CTFOFF (tertiary, OFF-line) — wired separately via constraint
#: coefficients (property 399), not part of MRU/MRD MW caps.
_CF_RESERVE_DIRS: tuple[tuple[str, str], ...] = (
    ("CPF", "CPF"),
    ("CSF", "CSF"),
    ("CTF", "CTFON"),
)


def _cf_maxresp_series(
    bundle: PlexosBundle, gen_name: str, subdir: str, prefix: str, direction: str
) -> list[float]:
    """Per-(gen, reserve-subdir, direction) per-hour MAX RESPONSE [MW].

    Reads ``CFdata/<subdir>/<prefix>_<gen>_<direction>.csv`` where:
      * ``subdir`` ∈ {"CPF", "CSF", "CTF"} (Primary / Secondary / Tertiary FC)
      * ``prefix`` ∈ {"CPF", "CSF", "CTFON"} — matches the filename prefix
      * ``direction`` ∈ {"MRU", "MRD"} (Margen Reserva Up / Down)

    These files are the **authoritative per-(gen, reserve-type, hour) MAX
    RESERVE CAPABILITY** PLEXOS uses to bound Reserve.Provision LP
    variables.  Verified 2026-05-31 on v0407 PLEXOS sol: the per-(gen,
    reserve) cap == PLEXOS sol max EXACTLY for every binding pair (e.g.
    ANDINA CPF_LW: PLEXOS sol max 65.0, CFdata cap 65.0).  The XML
    linkage is `t_membership(coll 159 Reserve→Generators) → t_data
    (property_id 1400 "Max Response") → t_text (Data File) → CSV
    filename`.

    Returns the full per-hour MW list (CEN PCP ships 168 = 7 days × 24h
    per file).  Empty when the file is absent or all-zero — caller
    should treat that as "no contribution from this reserve subdir".

    NOTE: this function replaced the misnamed ``_cpf_ramp_series`` that
    treated these MAX RESERVE values as RAMP RATES (MW/h), producing
    artificially crushed ramp ceilings (e.g. 0.01 MW/h on sentinel
    rows).  Ramp data lives in ``Gen_MaxRampDay.csv`` / DB ``Max Ramp
    Up/Down`` properties — see ``extract_commitments``.
    """
    path = bundle.root / "CFdata" / subdir / f"{prefix}_{gen_name}_{direction}.csv"
    if not path.is_file():
        return []
    vals: list[float] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            try:
                vals.append(float(row[-1]))  # VALUE is the last column
            except ValueError:
                continue  # header row ("VALUE")
    return vals if any(v > 0.0 for v in vals) else []


def _cf_maxresp_aggregate(
    bundle: PlexosBundle, gen_name: str, direction: str
) -> list[float]:
    """Aggregate per-hour MAX RESERVE [MW] across ALL reserve subdirs
    (CPF + CSF + CTFON) for one (gen, direction).

    PLEXOS imposes one cap per (gen, reserve-type) pair; gtopt's
    ``reserve_provision`` carries a single ``up`` / ``dn`` column per
    gen that covers ALL up-reserves (resp. down).  The conservative
    upper envelope for that single column is the SUM of the per-type
    MRU (or MRD) values — each reserve type CAN simultaneously call up
    to its own MRU MW, and the sum is the gen's maximum total
    contribution across all up-reserve commitments.

    Returns the per-hour aggregated MW list (length 168 on CEN PCP
    weekly), or empty when NO reserve subdir has populated data for
    this gen.
    """
    per_subdir = [
        _cf_maxresp_series(bundle, gen_name, subdir, prefix, direction)
        for subdir, prefix in _CF_RESERVE_DIRS
    ]
    populated = [s for s in per_subdir if s]
    if not populated:
        return []
    n = max(len(s) for s in populated)
    out = [0.0] * n
    for s in populated:
        for i, v in enumerate(s):
            if i < n:
                out[i] += v
    return out


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

    # PLEXOS ``Generator.Commit`` enum encoding (Gen_Commit.csv):
    #   -1 = NO commitment (LP-relaxed dispatch, no on/off binary
    #        modelled — typical for must-take renewables / RoR hydro;
    #        ~988 of 1792 generators on CEN PCP weekly 2026-04-22).
    #    0 = Endogenous (LP / MIP decides commitment — the standard
    #        UC behaviour, ~56 generators).
    #   +1 = Forced ON (commitment status pinned to 1 for that period
    #        — ~102 generators always-on plus mixed-period gens).
    #
    # When a generator's Gen_Commit profile is ALL -1, gtopt should
    # not emit a CommitmentSpec at all — let the LP run continuous
    # dispatch within [pmin, pmax] (the legacy behaviour for gens
    # without a Commitment object).  When ANY period is 0 or +1, the
    # generator participates in commitment as normal (the per-period
    # forcing for +1 is a follow-up; the current writer doesn't yet
    # plumb per-block ``fixed_status``).
    # ``Gen_Commit.csv`` is no longer used to gate CommitmentSpec
    # emission (every value ``-1`` = MIP-endogenous, every value
    # ``0`` = forced-off-this-period, every value ``+1`` = must-
    # commit-this-period).  Until the writer supports per-period
    # forced-status the file is purely informational here.
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
    # Read the FULL horizon (``n_days``) — Min Stable Level is a time
    # series (e.g. SANTA_MARIA: 170.53 MW for 20 h, 98.53 MW for 148 h)
    # and the writer emits the per-block profile; reading only day-1
    # (the old default) collapsed it to a single over-restrictive floor.
    msl_csv = (
        read_long(bundle.csv("Gen_MinStableLevel.csv"), n_days=bundle.n_days)
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
    # PLEXOS ``Generator.Commit`` per-period forcing (Gen_Commit.csv,
    # VALUE in {-1, 0, +1}): +1 = forced ON, 0 = forced OFF, -1 =
    # MIP-endogenous (free).  Carried onto ``CommitmentSpec`` so the
    # writer can pin gtopt's ``u`` (commitment status) via
    # ``must_run`` (all-+1 units) or per-block ``fixed_status`` —
    # honouring PLEXOS's must-run / don't-commit decisions through the
    # commitment variable itself, NOT by zeroing ``pmax``.
    gen_commit_csv = (
        read_long(bundle.csv("Gen_Commit.csv"), n_days=bundle.n_days)
        if bundle.has("Gen_Commit.csv")
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
        # gtopt commitment start / shutdown costs are scalars; PLEXOS
        # treats them as period-invariant.  Warn (don't silently drop) if
        # a future bundle ships a time-varying series.
        if sc:
            _warn_if_series_varies("start cost", name, sc)
        if sd:
            _warn_if_series_varies("shutdown cost", name, sd)
        startup_cost = sc[0] if sc else 0.0
        shutdown_cost = sd[0] if sd else 0.0
        initial_status = 1.0 if units and units[0] > 0.0 else 0.0
        # PLEXOS reports IniHoursUp / IniHoursDown as separate
        # non-negative scalars; the active one wins. gtopt's
        # ``initial_hours`` is signed (+ up, − down).
        ih_up = up[0] if up else 0.0
        ih_down = down[0] if down else 0.0
        # Round-trip the raw PLEXOS pair onto the CommitmentSpec so
        # the writer can emit ``ini_hours_up`` / ``ini_hours_down``
        # as informational fields on the gtopt JSON (the collapsed
        # signed ``initial_hours`` is still the LP-consumed value).
        # ``None`` when the CSV doesn't list this generator (distinct
        # from explicit 0).
        ini_hours_up_raw: float | None = float(up[0]) if up else None
        ini_hours_down_raw: float | None = float(down[0]) if down else None
        # PLEXOS publishes BOTH ``Gen_IniHoursUp`` and ``Gen_IniHoursDown``
        # for the same unit (verified for ANGOSTURA_U1..U3, COLBUN_U1..U2
        # on v0407: 732 generators carry both, 659 of them with
        # ``Gen_IniUnits = 0``).  A heuristic "ih_up wins when > 0" picks
        # the WRONG sign for OFF units — e.g. an OFF unit with 168h
        # downtime + 168h up-history would collapse to +168 (online for
        # 168h) instead of the correct −168 (offline for 168h).  Derive
        # the sign from ``initial_status`` (which itself comes from
        # ``Gen_IniUnits``) so the OFF/ON branch picks the matching
        # PLEXOS scalar; fall back to max-magnitude only when units is
        # absent (``initial_units is None``) AND both scalars are set.
        if units:
            if initial_status > 0.5:
                initial_hours = ih_up if ih_up > 0.0 else 0.0
            else:
                initial_hours = -ih_down if ih_down > 0.0 else 0.0
        elif ih_up >= ih_down:
            initial_hours = ih_up
        else:
            initial_hours = -ih_down
        # t_data fallbacks for static UC parameters.
        min_up = db.static_property("Generator", gen.object_id, "Min Up Time")
        min_down = db.static_property("Generator", gen.object_id, "Min Down Time")
        # PLEXOS ``Max Ramp Up / Down`` is published in MW/min.
        # gtopt's ``Commitment.ramp_up / ramp_down`` is in MW/hr
        # (commitment_lp.cpp multiplies by ``block.duration()`` hours
        # to get the per-block ramp envelope).  Convert with × 60.
        # Without this conversion COCHRANE_1 (PLEXOS Max Ramp Down =
        # 1.25 MW/min ≡ 75 MW/h; initial_power = 244.842 MW;
        # FixedLoad[block 1] = 119.942 MW) fires
        # ``commitment_ramp_down#1 = 0 ≤ −123.65`` and the LP becomes
        # primal-infeasible on the very first block.
        raw_ramp_up = db.static_property("Generator", gen.object_id, "Max Ramp Up")
        raw_ramp_down = db.static_property("Generator", gen.object_id, "Max Ramp Down")
        # Per-unit ramp limits come from PLEXOS's ``Max Ramp Up`` /
        # ``Max Ramp Down`` Generator properties (MW/min → ×60 for MW/h).
        # Previously the converter also read
        # ``CFdata/CPF/CPF_<gen>_MRU.csv`` / ``..._MRD.csv`` and treated
        # those values as ramp curves — that was a misinterpretation
        # (verified 2026-05-31): those files ship per-(gen, reserve, hour)
        # MAX RESERVE CAPABILITY in MW (PLEXOS property 1400 "Max
        # Response"), NOT ramp rates.  Reading them as ramps produced
        # artificially crushed ceilings (0.01 MW/h on sentinel rows) and
        # silently overwrote any DB ramp.  The CFdata files are now
        # consumed correctly by ``extract_reserve_provisions`` via
        # ``_cf_maxresp_aggregate``.
        ramp_up = raw_ramp_up * 60.0 if raw_ramp_up else 0.0
        ramp_down = raw_ramp_down * 60.0 if raw_ramp_down else 0.0
        # Per-block ramp profile is not currently sourced from any input
        # file (the only PCP ramp source is the static scalar above).
        # Kept as empty tuple so downstream code that probes for it works.
        ramp_up_profile: tuple[float, ...] = ()
        ramp_down_profile: tuple[float, ...] = ()
        # PLEXOS ``Run Up Rate`` is expressed in MW/min.  gtopt's
        # ``Commitment.startup_ramp`` is the maximum output [MW] the
        # unit can reach in the startup block.  Block duration in CEN
        # PCP is 1 hour, so the per-block envelope is
        # ``Run Up Rate × 60``.  When the CSV value is unset the
        # parser keeps the gtopt default (unset → no startup ramp
        # constraint, equivalent to startup_ramp = pmax).
        run_up_rate = db.static_property("Generator", gen.object_id, "Run Up Rate")
        startup_ramp = run_up_rate * 60.0 if run_up_rate else 0.0
        # PLEXOS ``Max Starts {Hour|Day|Week|Month|Year}`` (prop_id
        # 203..207) plus the per-horizon ``Max Starts`` (prop 202).
        # At solve time PLEXOS synthesises one
        # ``GenMaxStarts<Scope>_<gen>`` Constraint per non-zero entry;
        # the constraint's solution rows show up in the .accdb but the
        # input definition lives ONLY as a Generator-class property
        # (no Constraint object in t_object).  Read all five scopes and
        # pick the tightest applicable to the horizon — shorter scopes
        # win when populated (they're more restrictive per unit time):
        #   priority: Week > Day > Hour > Horizon > Month > Year
        # (Month / Year keep their PLEXOS spelling so the gtopt-side
        # ``MaxStartsScope`` enum collapses them to ``Horizon`` via its
        # alias entries — verified-by-tests round-trip.)
        max_starts_props = (
            ("week", "Max Starts Week"),
            ("day", "Max Starts Day"),
            ("hour", "Max Starts Hour"),
            ("horizon", "Max Starts"),
            ("month", "Max Starts Month"),
            ("year", "Max Starts Year"),
        )
        max_starts_count: int = 0
        starts_scope_value: str = ""
        for scope_key, prop_name in max_starts_props:
            raw = db.static_property("Generator", gen.object_id, prop_name)
            if raw and raw > 0.0:
                max_starts_count = int(round(raw))
                starts_scope_value = scope_key
                break
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
        # Hoist the GeneratorSpec lookup once — used by both the
        # Min-Stable-Level / pmax clamp and the Initial-Power /
        # FixedLoad sync below.
        gen_spec = next((g for g in generators if g.name == name), None)
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
        # Some PLEXOS CEN entries have ``Min Stable Level > Max
        # Capacity`` (e.g. ANCOA: pmin=19, pmax=8 from a stale or
        # legacy commit-floor) — this combined with ``Gen_Commit = 1``
        # (must commit) produces an LP-infeasible row
        # ``19·u_commit − gen ≤ 0`` since ``gen`` is bounded by 8.
        # PLEXOS itself dispatches ANCOA at 8 MW (= FixedLoad cap)
        # for the whole week without violating Min Stable Level —
        # i.e. PLEXOS implicitly demotes ``pmin`` to ``min(pmin,
        # max_pmax)`` when Fixed Load binds below it.  Mirror that.
        if gen_spec is not None:
            # PLEXOS treats Fixed Load as a hard equality
            # ``gen[t] = fixed_load[t]`` that OVERRIDES Min Stable
            # Level — the unit's actual dispatch is whatever Fixed
            # Load demands, regardless of the published MSL.  When
            # we land in gtopt's commitment-lower row
            # ``cmt_pmin · u_commit − gen ≤ 0`` with FixedLoad
            # pinning ``gen[t] < cmt_pmin``, the LP becomes
            # infeasible.  We saw this on CHIBURGO block 34
            # (block-aggregated FixedLoad = 3.23 < cmt_pmin = 3.64;
            # the per-hour raw FL values are 3.9/4.0/4.8/4.9, but
            # block 34 averages several of these down).
            #
            # A per-block clamp would need the block-aggregated
            # profile (not available in this extractor — the writer
            # does the aggregation).  Cleaner: drop ``cmt_pmin``
            # entirely for any gen that has a Fixed-Load profile.
            # Rationale: PLEXOS only respects MSL when Fixed Load
            # isn't binding, but if Fixed Load is published at all,
            # PLEXOS will let it win.  Setting ``cmt_pmin = 0`` here
            # leaves the actual dispatch level under FixedLoad's
            # control (still a hard equality) without making the
            # commitment row trigger an artificial MSL infeasibility.
            if gen_spec.fixed_load_profile and any(
                v > 0.0 for v in gen_spec.fixed_load_profile
            ):
                cmt_pmin = 0.0
            else:
                # No Fixed Load — still defensively clamp ``cmt_pmin``
                # to the smallest positive pmax in the raw profile so
                # block-aggregation rounding can't push pmax[t] below
                # cmt_pmin at a single block.
                positive_pmax = [
                    v for v in (gen_spec.pmax_profile or (gen_spec.pmax,)) if v > 0.0
                ]
                min_positive_pmax = min(positive_pmax, default=0.0)
                if cmt_pmin > min_positive_pmax > 0.0:
                    cmt_pmin = min_positive_pmax
        # Per-period Min Stable Level profile (full horizon).  PLEXOS
        # ships Min Stable Level as a time series; the scalar ``cmt_pmin``
        # above (period-1 value) over-constrains units whose floor drops
        # later in the week (e.g. SANTA_MARIA: 170.53 MW for 20 h, 98.53
        # MW for 148 h — gtopt previously pinned 170.53 everywhere and
        # could not back the unit down like PLEXOS).  Apply the same
        # fixed-load / pmax clamps element-wise; only keep the profile
        # when it genuinely varies (constant series use the scalar).
        cmt_pmin_profile: tuple[float, ...] = ()
        if msl_series and len(set(msl_series)) > 1:
            if (
                gen_spec is not None
                and gen_spec.fixed_load_profile
                and any(v > 0.0 for v in gen_spec.fixed_load_profile)
            ):
                prof = [0.0] * len(msl_series)
            else:
                cap = 0.0
                if gen_spec is not None:
                    pos_pmax = [
                        v
                        for v in (gen_spec.pmax_profile or (gen_spec.pmax,))
                        if v > 0.0
                    ]
                    cap = min(pos_pmax, default=0.0)
                prof = [min(v, cap) if (0.0 < cap < v) else v for v in msl_series]
            if len(set(prof)) > 1:
                cmt_pmin_profile = tuple(prof)
        # PLEXOS ``Initial Generation`` (Gen_IniGeneration.csv) →
        # gtopt ``Commitment.initial_power``: dispatch level at t=-1.
        # Pulled from the GeneratorSpec where the CSV was already
        # parsed during ``extract_generators``.
        initial_power = gen_spec.initial_generation if gen_spec else 0.0
        # When PLEXOS Fixed Load is positive on block 1 it acts as a
        # hard-equality (``Generation[1] = Fixed Load[1]``) that
        # bypasses Max Ramp Down — PLEXOS simply does NOT apply the
        # ramp constraint where Fixed Load binds.  gtopt's commitment
        # ramp row knows nothing of Fixed Load, so a fixed_load[0]
        # well below ``Initial Generation`` (e.g. COCHRANE_1:
        # 119.942 MW at block 1 vs 244.842 MW carried over) makes
        # the LP infeasible on block 1.  Align ``initial_power`` to
        # the binding Fixed Load value at block 1 so the ramp row
        # is trivially satisfied — mirrors PLEXOS's effective
        # treatment.
        if gen_spec and gen_spec.fixed_load_profile:
            fl0 = gen_spec.fixed_load_profile[0]
            if fl0 > 0.0:
                initial_power = fl0
        # PLEXOS effectively skips the ramp_down constraint on the
        # initial-power → block-0 transition: a unit whose
        # ``Gen_IniGeneration`` exceeds ``Gen_Rating[0] + Max Ramp Down``
        # still lands feasibly at ``Gen_Rating[0]`` in PLEXOS's hour-1
        # dispatch (verified on CEN-PCP 2026-05-17 GUACOLDA_1:
        # initial=120.9118 MW, pmax[0]=63.9118 MW, ramp_down=45 MW/h
        # → PLEXOS dispatches 63.9118 in hour 1, a 57 MW drop > 45).
        # gtopt's CommitmentLP enforces
        # ``initial_power − p[0] ≤ rd·u_init + sd·(1 − u_init)`` as
        # HARD (source/commitment_lp.cpp:537-563); without alignment
        # here, the LP goes infeasible by exactly
        # ``initial_power − pmax[0] − ramp_down``.
        #
        # Mirror PLEXOS's behaviour by capping ``initial_power`` to the
        # first-block pmax envelope whenever the gap exceeds what a
        # single ramp_down hour can absorb.  Preserves the unit's
        # initial commitment status (``u_init = 1``); only the carryover
        # dispatch level is trimmed to the maximum value the unit can
        # legitimately deliver at block 0.
        if (
            gen_spec
            and initial_power > 0.0
            and initial_status > 0.0
            and ramp_down > 0.0
        ):
            pmax_prof = gen_spec.pmax_profile or ()
            pmax0 = pmax_prof[0] if pmax_prof else (gen_spec.pmax or 0.0)
            if pmax0 > 0.0 and initial_power > pmax0 + ramp_down:
                logger.info(
                    "extract_commitments: %s initial_power %.4f → %.4f "
                    "(pmax[0]=%.4f, ramp_down=%.2f MW/h; PLEXOS-faithful "
                    "first-block cap — declared ramp can't bridge the gap)",
                    name,
                    initial_power,
                    pmax0,
                    pmax0,
                    ramp_down,
                )
                initial_power = pmax0
        # Threshold tiny ``initial_power`` to 0 — applied AFTER the
        # Fixed-Load override and the ramp_down cap so it catches every
        # path that can leave the field at a numerical-residual value.
        # PLEXOS Gen_IniGeneration / Gen_FixedLoad occasionally ships
        # numerical-noise values for plants that are physically OFF
        # (verified on jan18 PLEXOS20260118: ``uc_MUCHI`` ends up with
        # ``initial_power = 4.03e-08 MW`` from a tiny Fixed-Load[0]
        # value; ``uc_EL_MIRADOR`` at ``3.31e-04 MW``).  Both are
        # clearly intended as zero but each residual lands as the
        # ``Commitment[i].status × gen_upper`` coefficient over 168
        # blocks, dragging the LP coefficient ratio to 2.49e+07 and
        # the CPLEX kappa to ~10⁹–10¹⁰.  A 1 kW (1e-3 MW) threshold —
        # below the physical minimum tech of every CEN unit — zeros
        # these out without touching any real dispatch level.
        _MIN_INITIAL_POWER_MW = 1.0e-3
        if 0.0 < abs(initial_power) < _MIN_INITIAL_POWER_MW:
            initial_power = 0.0
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
            or initial_power
        )
        # Bridge: ALSO force-commit a unit that PLEXOS forces via
        # Gen_Commit (value 0 or +1 in some period) even when it carries
        # no commitment param of its own, so gtopt's committed set mirrors
        # PLEXOS for an apples-to-apples commitment comparison.  This is
        # safe now that renewable/RoR ``pmin`` is SOFT (0, curtailable):
        # a forced-OFF must-take unit simply curtails to 0, instead of the
        # old ``pmin=pmax`` (gen pinned >0) vs ``u=0`` infeasibility.
        # Verified clean otherwise: no Uniq family has 2+ configs forced
        # ON in the same block, and no forced unit has commitment.pmin >
        # pmax.  The pinned ``u`` (must_run/fixed_status, emitted in
        # build_commitment_array) is fixed in presolve, so the MIP cost is
        # negligible.
        _forced_commit = any(v in (0, 1) for v in gen_commit_csv.get(name, ()))
        if not any_param and not _forced_commit:
            continue

        # Drop commitments that have NO real on/off cost, NO Min
        # Stable Level floor, NO Min Up / Down time, AND NO ramp
        # limits.  Such commits add a u_commit binary to the MIP
        # that the LP can flip at zero economic consequence (no
        # startup cost, no shutdown cost, no >=pmin floor when
        # committed, no minimum contiguous-on/off duration, no
        # ramp-rate constraint) — pure noise that bloats the MIP
        # without changing the optimum.  Observed on 96 / 909 commits
        # in CEN PCP weekly: hydro/wind/solar with all seven fields
        # zero, plus a handful of gas/LNG fictitious INF proxies —
        # all with the wider ``initial_hours`` / ``noload_cost`` /
        # ``initial_power`` UC params (which qualify ``any_param``
        # above but on their own carry no commitment economics).
        # ...UNLESS PLEXOS forces this unit's commitment (Gen_Commit
        # 0/+1): keep it so the forced ``u`` has a variable to pin.
        if not any(
            (
                startup_cost,
                shutdown_cost,
                cmt_pmin,
                min_up,
                min_down,
                ramp_up,
                ramp_down,
                _forced_commit,
            )
        ):
            continue

        # PLEXOS ``Generator.Commit`` semantics revisited (2026-05-24):
        #   +1 = Yes — must commit
        #    0 = No  — don't commit (forced off this period)
        #   -1 = Endogenous — let PLEXOS's MIP decide via the
        #         standard commitment binary
        #
        # Earlier we skipped any generator with ``Gen_Commit = -1``
        # across the whole horizon on the theory that "-1 means no
        # commitment".  Validated against the PLEXOS solution
        # (.accdb prop 7 Units Generating) on CEN PCP 2026-04-22:
        # CAMPICHE (Gen_Commit = -1 always, $102k startup, 62.9 MW
        # MSL) actually cycles 1→0 mid-week in PLEXOS.  Same story
        # for 10/16 coal plants (ANGAMOS_1/2, NUEVA_VENTANAS,
        # HORNITOS, ANDINA, GUACOLDA_1/2/3/5, COCHRANE_2), 900+
        # thermals total.  Skipping their CommitmentSpec lets the
        # gtopt LP dispatch them freely with no startup cost,
        # producing the +$20 GWh CAMPICHE over-dispatch / +$25 GWh
        # SANTA_MARIA over-dispatch / etc. observed in the 2026-
        # 05-24 K=6 uniform run.
        #
        # Correct interpretation: ``Gen_Commit = -1`` = "MIP
        # decides" — emit the CommitmentSpec exactly as if any
        # other commitment-bearing parameter was present.  The
        # ``any_param`` filter above already excludes pure
        # renewables and RoR hydros (no startup_cost, no MSL, no
        # ramp data → ``any_param == False``).

        # Drop commitment for ``eff_pmax = 0`` units.  PLEXOS ships
        # every (configuration × fuel-band) variant of a combined-
        # cycle plant with ``Gen_Rating = 0`` for periods where it's
        # "not active" (a different alternate is on); the
        # ``*_Uniq`` mutex constraints enforce at-most-one variant
        # per plant.  When ALL variants of the same Uniq group
        # carry an active CommitmentSpec, gtopt's LP picks the
        # cheapest one (often a ``*_GNL_F`` variant with gcost
        # $3.80 < $5.40) to satisfy ``Σ status ≤ 1`` — but that
        # variant has ``pmax = 0`` so it can't actually dispatch.
        # The actually-dispatchable variant (``*_GN_A`` with
        # ``pmax > 0``) is then locked OFF.  Verified 2026-05-29 on
        # CEN PCP MIP K=4: QUINTERO_1A/B_GN_A reported 0 MWh vs
        # PLEXOS 6,496 / 6,516 MWh because the LP committed
        # QUINTERO_1A_GNL_F (pmax=0, gcost=$3.80, on 158/168 h) and
        # the Uniq constraint forced QUINTERO_1A_GN_A's status to 0.
        #
        # Fix: PIN ``status = 0`` for the CommitmentSpec when the
        # generator's effective pmax is zero across the horizon (no
        # ``pmax_profile`` row > 0 AND scalar ``pmax`` ≤ 0).  These
        # units genuinely can't dispatch; we must NOT drop the
        # CommitmentSpec entirely because downstream PAMPL UC
        # writers (SSCC reserve constraints, *_Uniq mutexes) inline
        # any ``commitment(X).status`` ref by moving the term to the
        # RHS as ``-1`` when X has no CommitmentSpec — corrupting
        # ``Σ status_i ≤ k`` into ``Σ status_remaining ≤ k − dropped``
        # which is structurally infeasible whenever ``dropped > k``
        # (CEN PCP: ATA_TG1A_GNL_SSCC ≤ 1 became ≤ -11 after dropping
        # 12 status terms).
        #
        # Instead, emit the CommitmentSpec with
        # ``commit_status_profile = (0,) * horizon_hours`` — the
        # writer translates an all-zero profile into per-block
        # ``fixed_status = 0`` (CPLEX presolve immediately fixes
        # ``u = 0``, zero LP cost) — preserving the variable for
        # PAMPL UC references and naturally giving the SSCC sums the
        # ``0`` contribution that PLEXOS itself reports for these
        # zero-rating variants (.accdb prop 7 Units Generating).
        #
        # The original motivation (let LP pick a dispatchable
        # variant in *_Uniq groups) is achieved exactly the same
        # way: pinning ``u_GNL_F = 0`` frees the budget under
        # ``Σ u ≤ 1`` for the dispatchable ``u_GN_A``.
        gen_spec_for_pmax = next((g for g in generators if g.name == name), None)
        force_off = False
        if gen_spec_for_pmax is not None:
            pmax_prof = gen_spec_for_pmax.pmax_profile or ()
            eff_pmax = max(pmax_prof) if pmax_prof else (gen_spec_for_pmax.pmax or 0.0)
            if eff_pmax <= 0.0:
                force_off = True
        if force_off:
            # Pin u = 0 for the whole horizon (zero-rating variant).
            # ``int(bundle.n_days) × 24`` matches the per-hour layout
            # used by every other Gen_*.csv loader in this module.
            commit_profile = (0,) * (max(int(bundle.n_days) or 1, 1) * 24)
        else:
            commit_profile = tuple(int(round(v)) for v in gen_commit_csv.get(name, ()))
        out.append(
            CommitmentSpec(
                generator_name=name,
                startup_cost=startup_cost,
                shutdown_cost=shutdown_cost,
                min_up_time=min_up,
                min_down_time=min_down,
                initial_status=initial_status,
                initial_hours=initial_hours,
                ini_hours_up=ini_hours_up_raw,
                ini_hours_down=ini_hours_down_raw,
                ramp_up=ramp_up,
                ramp_down=ramp_down,
                ramp_up_profile=ramp_up_profile,
                ramp_down_profile=ramp_down_profile,
                startup_ramp=startup_ramp,
                noload_cost=noload_cost,
                pmin=cmt_pmin,
                pmin_profile=cmt_pmin_profile,
                initial_power=initial_power,
                commit_status_profile=commit_profile,
                max_starts=max_starts_count,
                starts_scope=starts_scope_value,
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

    if not bundle.has("Hydro_AntucoBounds.csv"):
        return ()

    # Per-name per-DAY RHS series.  ``Hydro_AntucoBounds.csv`` ships one row
    # per day (columns NAME,YEAR,MONTH,DAY,PERIOD,Value with PERIOD=1), and the
    # bound genuinely varies day-to-day (e.g. ANTUCOmin 83.30→82.84 m³/s over a
    # week).  Keep the WHOLE series — rows arrive in chronological order — so it
    # can be carried as a per-block ``rhs_profile`` instead of flattened to the
    # first day (which would pin the whole horizon to day-1's discharge bound).
    rhs_days_by_name: dict[str, list[float]] = {}
    with Path(bundle.csv("Hydro_AntucoBounds.csv")).open(
        "r", encoding="utf-8", newline=""
    ) as fh:
        for row in csv.reader(fh):
            if not row or not row[0] or row[0] == "NAME":
                continue
            name = row[0]
            try:
                rhs_days_by_name.setdefault(name, []).append(float(row[5]))
            except (ValueError, IndexError):
                continue

    if not rhs_days_by_name:
        return ()

    # Map from generator name → production_factor (MW per m³/s).  When
    # unset the LP defaults to 1, so the constraint converts gen → flow
    # at a 1:1 ratio which is wrong for most plants but better than
    # dropping the constraint outright.
    pf_by_gen: dict[str, float] = {}
    for t in turbines:
        if t.production_factor and t.production_factor > 0.0:
            pf_by_gen[t.generator_name] = float(t.production_factor)

    # Index known generator names so we can drop membership rows whose
    # parent generator never made it into the converted case (e.g.
    # dropped during ``extract_generators`` because the bus was unknown
    # — referencing such a name would trip the gtopt strict resolver).
    # Generators with ``pmax = 0`` at every block are intentionally
    # KEPT in the LHS: gtopt's UC resolver treats element-known-but-
    # offline references as silent-zero contributions (see
    # ``element_column_resolver.hpp`` ``element_known = true`` branch
    # + ``user_constraint_lp.cpp`` ``no LP column for this block``
    # branch), so dropping the term changes nothing physically and
    # only obscures the audit trail vs PLEXOS, which itself enumerates
    # every membership in the constraint LHS regardless of UnitsOut /
    # commitment state.  Pre-fix behaviour over-filtered: ANTUCO_U2
    # (UnitsOut=1 day-of) and EL_TORO_U1 were silently dropped from
    # the LHS of ``ANTUCOmin / ANTUCOmax / ELTOROmax`` discharge UCs.
    known_gen_names: set[str] = {g.name for g in generators}

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
        if cstr_obj.name not in rhs_days_by_name:
            continue
        # Only drop the term when the parent generator isn't in the
        # converted case at all (PLEXOS object existed but our
        # extractor pruned it — typically a stranded sentinel gen).
        # ``known_gen_names`` is empty when no GeneratorSpec list was
        # supplied (older callers / test fixtures), in which case we
        # trust the PLEXOS membership unconditionally.
        if known_gen_names and gen_obj.name not in known_gen_names:
            continue
        members_by_constraint.setdefault(cstr_obj.name, []).append(gen_obj.name)

    out: list[UserConstraintSpec] = []
    for cstr_name, rhs_days in rhs_days_by_name.items():
        # Scalar fallback baked into the expression tail (day 1); the per-day
        # series is carried as ``rhs_profile`` below and OVERRIDES it per block.
        rhs = rhs_days[0]
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
        # Carry the day-to-day variation as a per-HOUR profile (each day's
        # bound held across its 24 hours); the writer block-aggregates it to
        # the gtopt ``rhs`` TB-schedule, overriding the day-1 scalar above at
        # every (stage, block).  Only emit when the bound actually varies —
        # a constant series is fully captured by the scalar tail.
        rhs_profile = (
            tuple(v for v in rhs_days for _ in range(24))
            if len(set(rhs_days)) > 1
            else ()
        )
        out.append(
            UserConstraintSpec(
                name=f"discharge_{cstr_name}",
                expression=expression,
                penalty=uc_penalty,
                rhs_profile=rhs_profile,
                description=(
                    f"PLEXOS hydro discharge bound '{cstr_name}': "
                    f"Σ generation/production_factor = turbine discharge "
                    f"[m³/s] {op} {rhs:g} — multi-unit plant penstock/flow "
                    f"limit (gen [MW] ÷ pf [MW/(m³/s)]) "
                    f"(File: Hydro_AntucoBounds.csv)"
                ),
            )
        )
    logger.info(
        "extract_hydro_discharge_user_constraints: emitted %d discharge UCs "
        "from Hydro_AntucoBounds.csv + Generator→Constraint memberships",
        len(out),
    )
    return tuple(out)


def _synthesise_pinned_flow_rights(
    forced_waterway_targets: list[tuple[str, str, float, tuple[float, ...]]],
    known_junction_names: frozenset[str],
) -> tuple[FlowRightSpec, ...]:
    """Convert pinned forced-flow waterways into soft FlowRights.

    For each ``(name, source_junction, target_m3s, profile)`` tuple
    captured by ``extract_waterways`` (Filt_Laja / Filt_Inv / Filt_Colb /
    Caudal_Eco_Ralco / Riego_RUCUE / Riego_LAJA_I etc.), emit a FlowRight
    at the SOURCE junction with:

      * ``target`` / ``target_profile``  (the operational delivery target;
        the per-hour ``profile`` is carried whenever it varies so a target
        that pauses — e.g. irrigation only on day 1 — is honoured
        block-by-block instead of soft-forcing the peak all horizon)
      * ``fcost``  ($/(m³/s)/h shortfall penalty)
      * ``fmax = 10 × target``  (large enough to also serve as a spill
        outlet when upstream has surplus — FlowRight's column drains the
        junction, so any flow above target is overflow)

    The paired hard Waterway + synthetic sink are DROPPED (the FlowRight is
    consumptive on the source junction), so the LP gets a soft delivery
    obligation priced at the unserved cost instead of an infeasibility when
    upstream is short.
    """
    # FlowRight unserved cost — high enough that the LP only
    # shortchanges the obligation when no feasible alternative
    # exists.  Previously 10 $/(m³/s)·h; bumped to 1000 so
    # irrigation / ecological-flow shortfalls cost ~100× more
    # than thermal generation and the LP avoids them by default.
    fcost_per_cumec_hour = 1000.0
    out: list[FlowRightSpec] = []
    dropped: list[tuple[str, str]] = []
    for name, source_junction, target, profile in forced_waterway_targets:
        if source_junction not in known_junction_names:
            # The forced obligation (irrigation / ecological / filtration)
            # cannot be attached — its source junction was never emitted, so
            # the FlowRight would silently vanish and the obligation would be
            # un-modelled (water free-routes through penstocks).  Record it so
            # the drop is visible instead of silent.
            dropped.append((name, source_junction))
            continue
        # Carry the per-hour profile only when the obligation varies; a flat
        # obligation collapses to the scalar ``target``.
        varies = bool(profile) and min(profile) != max(profile)
        out.append(
            FlowRightSpec(
                # ``soft_`` prefix: a SOFT delivery obligation priced at
                # ``fcost``.  ``target`` is the soft kink the LP is steered
                # toward; ``fmax = 10 × target`` lets it deliver more when
                # profitable.
                name=f"soft_{name}",
                junction_name=source_junction,
                purpose="forced_flow",
                fmin=0.0,
                fmax=target * 10.0,
                target=target,
                target_profile=tuple(profile) if varies else (),
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
    if dropped:
        logger.warning(
            "_synthesise_pinned_flow_rights: DROPPED %d forced-flow "
            "obligation(s) whose source junction was not emitted — the "
            "irrigation/ecological/filtration target is UN-MODELLED and its "
            "water will free-route through penstocks (inflating hydro "
            "generation). Fix the source-junction emission. Dropped: %s",
            len(dropped),
            ", ".join(f"{n}@{j}" for n, j in dropped),
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


#: PLEXOS timeslice tag grammar.  Two atom shapes:
#:
#:   ``H<a>-<b>``  — hour-of-day range, 1-indexed inclusive.  Hour ``h`` is
#:                   the block ending at ``h:00`` (so ``H16-20`` ⇒ blocks
#:                   indexed 16..20 in a 24h day, the 16:00→20:59 window).
#:   ``W<a>-<b>``  — day-of-week range, 1-indexed inclusive.  CEN PCP
#:                   convention (verified against ``IL_2024000947_ATACC2``):
#:                   ``W1=Sunday`` … ``W7=Saturday`` so ``W2-6`` ⇒ Mon-Fri.
#:
#: Atoms join with commas; comma-joined atoms of the SAME letter union (an
#: ``H1-8,H20-24`` tag covers hours 1-8 OR 20-24).  A tag mixing letters
#: intersects (``W2-6,H8-21`` ⇒ weekday Mon-Fri AND hours 8-21).
_TIMESLICE_ATOM_RE = re.compile(r"^([HW])(\d+)(?:-(\d+))?$")


def _expand_timeslice(
    tag: str,
    horizon_start: datetime | None,
    n_blocks: int,
    block_hours: float = 1.0,
) -> tuple[bool, ...]:
    """Expand a PLEXOS timeslice tag into a per-block boolean mask.

    Args:
        tag: PLEXOS timeslice string (``"H16-20"``, ``"H1-8,H20-24"``,
            ``"W2-6,H8-21"``).  Whitespace is tolerated.
        horizon_start: Run-horizon start datetime (from PLEXOS Horizon
            object). Required only for tags carrying ``W`` (weekday)
            atoms; ``H``-only tags ignore the calendar.  When ``None``
            and the tag uses ``W``, the weekday clause is skipped (mask
            falls back to hour-only — best-effort for unit tests
            without a Horizon date).
        n_blocks: LP horizon block count (typically 24 × n_days).
        block_hours: Block duration in hours.  Defaults to 1 — the CEN
            PCP hourly grid.  Sub-hourly grids (15-min = 0.25 h) would
            scale block→hour mapping accordingly.

    Returns:
        Length-``n_blocks`` boolean tuple where ``mask[i] = True``
        means the timeslice covers block ``i``.

    Raises:
        ValueError: when ``tag`` cannot be parsed.  Callers should
            catch and fall back to scalar RHS (logged as a warning).
    """
    atoms = [a.strip() for a in (tag or "").split(",") if a.strip()]
    if not atoms:
        raise ValueError(f"empty timeslice tag {tag!r}")
    hour_atoms: list[tuple[int, int]] = []
    weekday_atoms: list[tuple[int, int]] = []
    for atom in atoms:
        m = _TIMESLICE_ATOM_RE.match(atom)
        if not m:
            raise ValueError(f"unrecognised timeslice atom {atom!r} in {tag!r}")
        kind, lo_s, hi_s = m.group(1), m.group(2), m.group(3)
        lo = int(lo_s)
        hi = int(hi_s) if hi_s is not None else lo
        if kind == "H":
            hour_atoms.append((lo, hi))
        else:
            weekday_atoms.append((lo, hi))

    # Build hour-of-day mask (size 24).  Hours are 1-indexed in PLEXOS;
    # we store at index ``h-1`` so block index 0 corresponds to hour 1.
    if hour_atoms:
        hour_mask = [False] * 24
        for lo, hi in hour_atoms:
            for h in range(max(1, lo), min(24, hi) + 1):
                hour_mask[h - 1] = True
    else:
        hour_mask = [True] * 24

    # Build weekday mask (size 7).  PLEXOS ``W1=Sunday`` … ``W7=Saturday``;
    # Python's ``datetime.weekday()`` is ``Monday=0..Sunday=6``, so we
    # translate via ``plexos_w = (py_weekday + 1) % 7 + 1``.
    if weekday_atoms and horizon_start is not None:
        wd_mask = [False] * 7
        for lo, hi in weekday_atoms:
            for w in range(max(1, lo), min(7, hi) + 1):
                wd_mask[w - 1] = True
    else:
        wd_mask = [True] * 7

    out = [False] * n_blocks
    for b in range(n_blocks):
        # Hour-of-day index for this block, 0-based (block 0 = hour 1).
        hour_idx = int(b * block_hours) % 24
        if not hour_mask[hour_idx]:
            continue
        if weekday_atoms and horizon_start is not None:
            day_offset = int(b * block_hours) // 24
            py_wd = (horizon_start + timedelta(days=day_offset)).weekday()
            plexos_w = (py_wd + 1) % 7 + 1  # Mon(0)→W2, Tue(1)→W3, ..., Sun(6)→W1
            if not wd_mask[plexos_w - 1]:
                continue
        out[b] = True
    return tuple(out)


def _build_rhs_timeslice_profile(
    rhs_rows: list,
    base_value: float,
    horizon_start: datetime | None,
    n_blocks: int,
) -> tuple[float, ...]:
    """Overlay timeslice-tagged ``RHS`` rows on top of the base scalar.

    Each ``rhs_row`` with a ``.timeslice`` tag specifies a recurring
    hour-of-day / day-of-week window during which that row's value
    overrides ``base_value``.  When multiple tagged rows overlap the
    same block, the row with the highest ``data_id`` wins (PLEXOS
    MDB append semantics, mirroring :func:`_horizon_value` for the
    single-row case).

    Args:
        rhs_rows: All RHS rows (active + expired) the constraint
            carries.  Expired rows are skipped just like
            :func:`_horizon_value` would skip them.
        base_value: Fallback scalar for the blocks NOT covered by any
            timeslice tag, used only when no UNTAGGED active row exists
            in ``rhs_rows``.  When an untagged active row IS present its
            value supersedes ``base_value`` — that's PLEXOS's "default
            base" row (the row without a timeslice tag), which the
            caller's ``_horizon_value(prefer_min=True)`` would otherwise
            collapse with the tagged value.  Example: N_to_Nogales_N1
            carries an untagged RHS=430 (apply outside H9-18) plus a
            tagged RHS=402.3 (apply during H9-18); `_horizon_value` picks
            min(402.3, 430)=402.3 — wrong for the un-overlapped blocks.
        horizon_start: PLEXOS Horizon start datetime; needed for ``W``
            (weekday) atoms.  May be ``None`` for unit tests.
        n_blocks: LP horizon block count.

    Returns:
        Length-``n_blocks`` tuple of floats.  Empty when no tagged row
        is active (caller falls back to the scalar emit path).
    """
    # Tagged active rows — sorted ASC by data_id so last-write-wins overlay.
    tagged_rows = sorted(
        (
            r
            for r in rhs_rows
            if r.timeslice and _has_active_rhs_row([r], horizon_start)
        ),
        key=lambda r: r.data_id,
    )
    if not tagged_rows:
        return ()
    # Untagged active rows — the natural "default base" for blocks not
    # covered by any tag.  Multiple untagged rows: pick the same way
    # _horizon_value picks (smaller wins for ≤ sense; here we just take
    # min as the relaxed bound).  Fall back to ``base_value`` when there
    # is no untagged active row (the whole constraint is tag-only).
    untagged_active = [
        r.value
        for r in rhs_rows
        if not r.timeslice and _has_active_rhs_row([r], horizon_start)
    ]
    base = min(untagged_active) if untagged_active else base_value
    profile = [base] * n_blocks
    for row in tagged_rows:
        mask = _expand_timeslice(row.timeslice, horizon_start, n_blocks)
        for i, on in enumerate(mask):
            if on:
                profile[i] = row.value
    return tuple(profile)


def _block_in_date_window(
    block_idx: int,
    horizon_start: datetime,
    date_from: datetime | None,
    date_to: datetime | None,
    block_hours: float = 1.0,
) -> bool:
    """True when block ``block_idx`` overlaps ``[date_from, date_to]``.

    Block ``i`` covers the half-open hour range
    ``[horizon_start + i*block_hours, horizon_start + (i+1)*block_hours)``.
    A block is considered IN-window when its hour range intersects the
    closed PLEXOS date window — an undated boundary is treated as ±∞.
    Used by :func:`_build_rhs_date_overlay_profile` to localise a dated
    RHS override to the blocks it actually applies to.
    """
    block_start = horizon_start + timedelta(hours=block_idx * block_hours)
    block_end = block_start + timedelta(hours=block_hours)
    if date_from is not None and block_end <= date_from:
        return False
    if date_to is not None and block_start >= date_to:
        return False
    return True


def _build_rhs_date_overlay_profile(
    rhs_rows: list,
    base_value: float,
    horizon_start: datetime | None,
    n_blocks: int,
) -> tuple[float, ...]:
    """Overlay PARTIAL-horizon dated ``RHS`` rows on top of the base scalar.

    PLEXOS lets a constraint carry RHS overrides whose
    ``[date_from, date_to]`` window covers ONLY part of the run horizon
    (e.g. ``SD_2026030813_NvaPAzucar_Polpaico500_neg`` carries an
    undated base RHS=10000 plus a dated RHS=1600 active only for the
    4 blocks 2026-04-22 06:00-10:00).  ``_horizon_value`` collapses
    these to a single scalar; this builder produces the per-block
    profile so the LP sees RHS=1600 on the 4 in-window blocks and
    RHS=10000 on the other 164.

    Args:
        rhs_rows: Every RHS row the constraint carries.  Untagged rows
            are treated as the base (their value supersedes
            ``base_value`` if any are active and not timeslice-tagged).
            Tagged-only rows are NOT handled here (use
            :func:`_build_rhs_timeslice_profile`); they reach this path
            only via the caller's combined dispatch and are skipped.
        base_value: Fallback for blocks NOT covered by any dated row,
            used only when no UNTAGGED-UNDATED active row exists.
        horizon_start: PLEXOS Horizon start datetime.  When ``None`` the
            overlay cannot be computed and the function returns ``()``.
        n_blocks: LP horizon block count.

    Returns:
        Length-``n_blocks`` tuple, or ``()`` when no dated row would
        contribute to any block of the horizon.  The empty return lets
        the caller fall back to the legacy scalar path unchanged.
    """
    if horizon_start is None:
        return ()
    # Collect dated rows that touch the horizon at all.  A row is a
    # candidate overlay when its date window intersects ANY block — this
    # is the "partial-window" case that ``_horizon_value``'s all-or-
    # nothing horizon_start check rejects.
    dated_overlays = [
        r
        for r in rhs_rows
        if (r.date_from is not None or r.date_to is not None)
        and not getattr(r, "timeslice", None)
        and any(
            _block_in_date_window(i, horizon_start, r.date_from, r.date_to)
            for i in range(n_blocks)
        )
    ]
    if not dated_overlays:
        return ()
    # Untagged-undated active rows form the base (multiple → min, same
    # convention as :func:`_build_rhs_timeslice_profile`).  Fall back to
    # the caller-supplied ``base_value`` when no such row exists.
    untagged_undated = [
        r.value
        for r in rhs_rows
        if not getattr(r, "timeslice", None)
        and r.date_from is None
        and r.date_to is None
    ]
    base = min(untagged_undated) if untagged_undated else base_value
    profile = [base] * n_blocks
    # Highest-data_id overlay wins on overlapping blocks (MDB append semantics).
    for row in sorted(dated_overlays, key=lambda r: r.data_id):
        for i in range(n_blocks):
            if _block_in_date_window(i, horizon_start, row.date_from, row.date_to):
                profile[i] = row.value
    return tuple(profile)


# ───────────────────────────────────────────────────────────────────────
# Hydro per-plant min/max/ramp constraints — special handling
# ───────────────────────────────────────────────────────────────────────
#
# PLEXOS encodes per-plant turbine-discharge floors / caps / ramp limits
# as UserConstraints named after the plant: ``ANTUCOmin``, ``ANTUCOmax``,
# ``ANGOSTURAmin``, ``ANGOSTURAeco``, ``ELTOROmax``, ``PANGUEramp``,
# ``MACHICURAlagrampup``, ``COLBUNmax``, etc.  Each is structurally
# something like ``Σ_units generation ≥ <floor>`` or ``Σ ≤ <cap>`` or a
# ramp delta inequality on a single hydro plant's units.
#
# **In PLEXOS sol these constraints solve as HARD** (Slack=0,
# Shadow=large e.g. $7,999/MWh for ``ANTUCOmin``).  But PLEXOS gates
# them INTERNALLY on the unit's commit status: when PLEXOS keeps the
# plant OFF for a block, the min/max row is auto-relaxed (the LP isn't
# asked to satisfy ``gen ≥ floor`` when the unit is OFF — that would be
# infeasible).  This is part of PLEXOS's MIP / commitment formulation,
# not visible in the input XML.
#
# **gtopt does NOT have a native commit-gated UC primitive yet**, so
# hardening these names without the gate makes the LP primal-infeasible
# at blocks where the hydro is fractionally / fully off (verified on
# CEN PCP 2026-04-22: ``antucomin_constraint_1263_1_1_40`` reported
# infeasible by CPLEX presolve when added to the PLEXOS-HARD-promote
# list; the LP-relax can't satisfy ``Σ_ANTUCO_Ui gen ≥ 137`` at block
# 40 because OTHER constraints force the units to a lower aggregate).
#
# **Workarounds in place:**
#   * **Plan 2** (commit ``7d2c9bcd0``): auto-promote ``<NAME>min/max``
#     UCs to a Commitment.pmin/pmax on the matching hydro generator
#     when the LHS is a single ``generator(X).generation`` term.  Covers
#     ANTUCO/ELTORO/ANGOSTURA per-unit floors but NOT multi-unit
#     aggregations or ramp / lag rows.
#   * **soft tier ($10/MWh)** for everything Plan 2 doesn't catch:
#     the LP slacks the floor / cap when the inflow / commitment
#     pattern can't satisfy it, paying a small penalty.
#
# **CLI knob** ``--hydro-min-mode {soft, hard}`` (default ``soft``):
#   * ``soft`` (default): names matching the regex below are FILTERED
#     OUT of the PLEXOS-HARD list before promotion.  They fall through
#     to the normal soft-tier classification.
#   * ``hard``: names are kept in the PLEXOS-HARD list, promoted to
#     penalty=0.  WILL FAIL on most CEN PCP weekly cases at LP-relax
#     time (matching PLEXOS sol Activity = RHS exactly requires the
#     commit-gating PLEXOS provides internally and gtopt does not).
#     Available for debug / validation against a horizon where the
#     hydro commitment pattern happens to align between gtopt and
#     PLEXOS.
#
# The detection regex below matches single-plant-named hydro min/max /
# ramp / lag rows.  Names are case-sensitive.  Pure suffixes covered:
# ``min``, ``max``, ``eco``, ``ramp``, ``lagrampup``, ``lagrampdown``,
# ``rampdownact``, ``rampupact``, ``ramplogic``, ``PMax``, ``GENT4def``,
# ``GENT7def`` (def-eqs are wired separately under the Plan 2 path).
_HYDRO_GATED_UC_RE = re.compile(
    r"^(?P<stem>[A-Z][A-Za-z0-9_]*?)"
    r"(?:min|max|eco|ramp|lagrampup|lagrampdown|rampdownact|rampupact|"
    r"ramplogic|_PMax|maxramp|caudal_min_diario)$"
)
# The set of hydro plant stems is sized from a small canonical list to
# avoid accidentally matching non-hydro families like ``Gas_MaxOpDay*``
# (which already has its own daily_sum handling).  Stems verified
# against CEN PCP 2026-04-22.  Extending the list when a new dataset
# introduces a hydro plant is the expected maintenance.
_HYDRO_PLANT_STEMS = frozenset(
    {
        "ANTUCO",
        "ANGOSTURA",
        "CIPRESES",
        "COLBUN",
        "ELTORO",
        "LMAULE",
        "MACHICURA",
        "PANGUE",
        "PEHUENCHE",
        "POLPAICO",
        "POLCURA",
        "RALCO",
        "RAPEL",
        "SAUZAL",
        "SAUZALITO",
    }
)


def _is_hydro_min_max_uc(name: str) -> bool:
    """True when ``name`` is a per-plant hydro min/max/ramp UserConstraint.

    Matches names of the form ``<HYDRO_PLANT_STEM><suffix>`` where the
    stem is one of the canonical CEN PCP hydro plants and ``<suffix>``
    is one of the recognised min/max/ramp tokens (see ``_HYDRO_GATED_UC_RE``).
    """
    m = _HYDRO_GATED_UC_RE.match(name)
    if m is None:
        return False
    return m["stem"] in _HYDRO_PLANT_STEMS


@functools.cache
def _hydro_min_mode() -> str:
    """Return the active ``--hydro-min-mode`` (``soft`` or ``hard``).

    Default ``soft`` (LP-feasible fallback for runs where gtopt cannot
    reproduce PLEXOS's commit-gated relaxation of the hydro min/max
    constraints — see the module-level note above ``_HYDRO_GATED_UC_RE``
    for the full rationale).  Set ``GTOPT_HYDRO_MIN_MODE=hard`` (or pass
    ``--hydro-min-mode hard`` to plexos2gtopt) to keep these names in
    the PLEXOS-HARD-promote list — useful for debug / validation runs.
    """
    import os as _os

    val = _os.environ.get("GTOPT_HYDRO_MIN_MODE", "soft").strip().lower()
    if val not in ("soft", "hard"):
        logger.warning("GTOPT_HYDRO_MIN_MODE='%s' not recognised; using 'soft'", val)
        return "soft"
    return val


@functools.cache
def _load_plexos_hard_uc_list() -> frozenset[str]:
    """Names of PLEXOS-HARD UCs (audited from sol .accdb) to promote to HARD.

    Reads ``data/cen_pcp_hard_ucs.txt`` (one constraint name per line, an
    optional ``  # ...`` annotation per line is stripped).  Loaded once
    per process and cached.  Returns the legacy 2-name fallback
    (``MACHICURA_GENT4def``, ``PEHUENCHE_GENT7def``) when the file is
    missing or unreadable, preserving backwards-compatible behaviour.

    Hydro per-plant min/max/ramp names are filtered out by default
    (``--hydro-min-mode soft``) because they require commit-gating that
    gtopt does not provide — see the module-level note above
    ``_HYDRO_GATED_UC_RE``.  Pass ``--hydro-min-mode hard`` to keep
    them in the list (debug / validation only).
    """
    legacy = frozenset({"MACHICURA_GENT4def", "PEHUENCHE_GENT7def"})
    path = Path(__file__).parent / "data" / "cen_pcp_hard_ucs.txt"
    if not path.is_file():
        return legacy
    try:
        names: set[str] = set()
        for raw in path.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if line:
                names.add(line)
        if not names:
            return legacy
        # Apply the hydro-min-mode filter.
        if _hydro_min_mode() == "soft":
            removed = {n for n in names if _is_hydro_min_max_uc(n)}
            if removed:
                logger.info(
                    "hydro-min-mode=soft: kept %d hydro per-plant min/max/ramp "
                    "UCs at default soft tier (HARD would be infeasible "
                    "without commit-gating — see _HYDRO_GATED_UC_RE note): "
                    "%s",
                    len(removed),
                    ", ".join(sorted(removed)),
                )
            names -= removed
        else:
            kept = {n for n in names if _is_hydro_min_max_uc(n)}
            if kept:
                logger.warning(
                    "hydro-min-mode=hard: keeping %d hydro per-plant "
                    "min/max/ramp UCs in the HARD list — LP-relax may be "
                    "infeasible without commit-gating!  Names: %s",
                    len(kept),
                    ", ".join(sorted(kept)),
                )
        return frozenset(names)
    except OSError:
        return legacy


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


def _horizon_value(
    rows: list, horizon_start, *, prefer_min: bool = False
) -> float | None:
    """Select the effective PLEXOS property value for ``horizon_start``.

    PLEXOS property values can carry a ``[date_from, date_to]`` window
    (``t_date_from`` / ``t_date_to``).  A *dated* row is a temporary
    override active only while the simulation timestamp lies inside its
    window; *undated* rows are the permanent base value.  The CEN PRGdia
    bundles routinely ship several rows per constraint property — a base
    value plus narrow single-day windows that record historical binding
    events from past real-time runs.  ``_first_value`` (lowest data_id)
    ignores the windows entirely and frequently picks a stale historical
    override, which is wrong: validated against the RES20260422 solution,
    the lowest-data_id RHS matches PLEXOS in only 30 % of the 336
    ambiguous constraints, whereas this date-aware rule matches 99 %.

    Selection:
      1. Keep rows active at ``horizon_start``: undated base rows (priority
         0) and dated rows whose window covers it (priority 1).
      2. Among the highest priority present, pick the value.  For RHS
         (``prefer_min=True``) take the minimum — the relaxed bound PLEXOS
         applies when a constraint is effectively disabled (e.g. a
         ``Σ status >= 1`` disjunction whose live RHS is 0, freeing the
         unit to shut down); otherwise take the lowest-data_id value.
      3. Drop the ``±1E+30`` infinity sentinel.

    Falls back to undated/base rows when ``horizon_start`` is unknown or no
    dated row is active, so unit tests without Horizon dates behave as
    before.
    """
    if not rows:
        return None

    def _active_priority(row) -> int | None:
        df, dt_ = row.date_from, row.date_to
        if df is None and dt_ is None:
            return 0
        if horizon_start is None:
            return None
        if (df is None or df <= horizon_start) and (
            dt_ is None or dt_ >= horizon_start
        ):
            return 1
        return None

    active = [(p, r) for r in rows if (p := _active_priority(r)) is not None]
    if not active:
        # No row active at the horizon (all windows historical).  Fall back
        # to undated base rows, else the raw rows, so we never return None
        # for a constraint that does carry an RHS.
        base = [r for r in rows if r.date_from is None and r.date_to is None]
        active = [(0, r) for r in (base or rows)]

    top = max(p for p, _ in active)
    candidates = [r for p, r in active if p == top]
    vals = [
        r.value for r in candidates if r.value is not None and abs(r.value) < 1.0e20
    ]
    if not vals:
        return None
    if prefer_min:
        return min(vals)
    return min(candidates, key=lambda r: r.data_id).value


def _has_active_rhs_row(rows: list, horizon_start) -> bool:
    """True if any row is undated OR its ``[date_from, date_to]`` window
    covers ``horizon_start``.

    Distinguishes a genuinely-active value from one that ``_horizon_value``
    only returns as an *expired-window* fallback (all rows dated to a window
    that does not cover the run date).  Used to let an active ``RHS Day``
    override an RHS that is present only as a stale historical override
    (e.g. ``PANGUEcaudal_min_diario``: expired ``RHS=0.48`` for Nov–Dec 2025
    vs active ``RHS Day=0.691`` → 28.79 on the Apr-2026 run date).
    """
    for r in rows:
        df, dt_ = r.date_from, r.date_to
        if df is None and dt_ is None:
            return True
        if (
            horizon_start is not None
            and (df is None or df <= horizon_start)
            and (dt_ is None or dt_ >= horizon_start)
        ):
            return True
    return False


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
    horizon_start: datetime | None = None,
) -> dict[int, list[tuple[str, float]]]:
    """``{constraint_object_id -> [(parent_name, coefficient), …]}`` for one prop.

    Date filtering: PLEXOS stores coefficient amendments as additional
    ``<t_data>`` rows on the SAME membership rather than overwriting the
    original.  Before 2026-05-28 this indexer just *summed* every row,
    which on ``CPF_Up5Calculation`` produced ``12.7111 + 13.37 + 10.75 =
    36.83`` for the ``Generation_SEN`` coefficient instead of the live
    ``10.75`` (the prior two values' date windows expired in 2024 and
    Feb 2026).  Now we group by ``membership_id`` and use
    :func:`_horizon_value` to pick the active value, exactly the same
    selection rule the RHS path applies (priority 1 dated-active > 0
    undated > expired fallback).  ``horizon_start = None`` keeps the
    legacy "all rows active" behaviour for unit tests that supply an
    XML without a Horizon date.
    """
    mid_to_pair = _build_membership_pair_index(db, coll_id)
    # Group data rows by membership_id so multiple amendments to the
    # same (parent, constraint, property) tuple resolve to a single value.
    by_mid: dict[int, list] = {}
    for d in db.data_rows:
        if d.membership_id in mid_to_pair and d.property_id == prop_id:
            by_mid.setdefault(d.membership_id, []).append(d)
    per_constr: dict[int, list[tuple[str, float]]] = {}
    for mid, rows in by_mid.items():
        parent_oid, constr_oid = mid_to_pair[mid]
        parent_obj = objs.get(parent_oid)
        if parent_obj is None:
            continue
        value = _horizon_value(rows, horizon_start)
        if value is None or value == 0.0:
            continue
        per_constr.setdefault(constr_oid, []).append((parent_obj.name, value))
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


def _shift_at(buf: list[float], idx: int) -> float:
    """Grow ``buf`` with zeros up to ``idx`` and return ``buf[idx]``."""
    while idx >= len(buf):
        buf.append(0.0)
    return buf[idx]


def _set_shift(buf: list[float], idx: int, value: float) -> None:
    """Grow ``buf`` with zeros up to ``idx`` and set ``buf[idx] = value``."""
    while idx >= len(buf):
        buf.append(0.0)
    buf[idx] = value


_UC_OP_TXT = {"<=": "≤", ">=": "≥", "=": "="}


def _describe_user_constraint(
    name: str,
    expression: str,
    op: str,
    rhs: float,
    *,
    source_file: str,
    fuel_offtake: bool = False,
    from_rhs_custom: bool = False,
    inactive: bool = False,
    n_terms: int = 0,
    n_filtered: int = 0,
    tautology: bool = False,
) -> str:
    """Build a human-readable ``description`` for a UserConstraint.

    Describes provenance (PLEXOS Constraint name + source file), LHS
    variable kinds + units, and notable transformations applied during
    translation (combined-cycle consolidation drops, tautology
    downgrade, inactive-stub status).

    ``n_terms`` is the number of LHS terms actually emitted into
    ``expression``.  When ``n_terms <= 1`` the ``Σ`` prefix is omitted —
    the previous unconditional template lied about sums when the
    converter's combined-cycle consolidator (or PLEXOS itself) had only
    produced a single member.

    ``n_filtered`` counts PLEXOS Generator memberships that were
    consolidated away during translation (offline-gen / shadow-line /
    always-on-renewable / fuel-without-consumer drops tracked by
    ``silent_zero_drops``).  When > 0, the description annotates the
    original membership count so an operator reading the .pampl can
    verify the consolidation against PLEXOS source data.

    ``tautology`` flips the description's tail to flag the constraint
    as trivially satisfied under the variable's natural binary domain.

    gtopt LP variables and their units:
      ``generation`` / ``flow`` / ``charge`` / ``discharge`` — power [MW];
      reserve ``up`` / ``dn`` — reserve power [MW];
      ``status`` / ``startup`` / ``shutdown`` — commitment binary [0/1];
      ``value`` — PLEXOS Decision Variable (units per its definition).
    """
    parts: list[str] = []
    if "generator(" in expression and ".generation" in expression:
        parts.append("generator dispatch [MW]")
    if "line(" in expression and ".flow" in expression:
        parts.append("line flow [MW]")
    if "battery(" in expression and (
        ".charge" in expression or ".discharge" in expression
    ):
        parts.append("battery charge/discharge [MW]")
    if "reserve_provision(" in expression:
        parts.append("reserve provision [MW]")
    if "commitment(" in expression:
        # Detect which commitment accessors actually appear in the
        # expression rather than always labelling them "status".  Bundle
        # audit surfaced 41 single-term Σ rows where the comment said
        # "status" but the term used ``.startup`` / ``.shutdown``.
        accessors = [
            kind
            for kind in ("status", "startup", "shutdown")
            if f".{kind}" in expression
        ]
        kinds = "/".join(accessors) if accessors else "status"
        parts.append(f"commitment {kinds} [0/1]")
    if "decision_variable(" in expression:
        parts.append("decision variable")
    op_txt = _UC_OP_TXT.get(op, op)
    if fuel_offtake:
        desc = (
            f"PLEXOS fuel-offtake constraint '{name}': "
            f"Σ heat_rate·generation [fuel-energy/h] {op_txt} {rhs:g}"
        )
        if from_rhs_custom:
            desc += (
                " — daily gas cap (RHS = PLEXOS 'RHS Custom' × 1000 / horizon_hours)"
            )
    else:
        lhs = " + ".join(parts) if parts else "LP terms"
        # ``Σ`` only when there is actually a sum.  Single-term rows
        # arise when PLEXOS itself had one member, when gtopt's
        # combined-cycle consolidator collapsed N variants into one
        # primary config, or when offline/shadow filtering reduced the
        # term list.  Stamping ``Σ`` unconditionally misled operators
        # auditing the bundle against the PLEXOS source.
        sigma = "Σ " if n_terms > 1 else ""
        desc = f"PLEXOS Constraint '{name}': {sigma}{lhs} {op_txt} {rhs:g}"
    if n_filtered > 0:
        original = n_terms + n_filtered
        desc += (
            f" — consolidated from {original} PLEXOS member(s); "
            f"{n_filtered} subsumed by gtopt's element model "
            "(offline gens, shadow lines, always-on renewables, or "
            "fuel without consuming gens)"
        )
    if tautology:
        desc += (
            " — tautology under binary [0,1] domain (LP-redundant, "
            "kept as inactive stub for PLEXOS provenance audit)"
        )
    if inactive:
        desc += " — active=False (excluded from PLEXOS ST schedule / contingency row)"
    return f"{desc} (File: {source_file})"


_TAUTOLOGY_TERM_RE = re.compile(
    r"^([+-]?\d+(?:\.\d+)?)\s*\*\s*"
    r"commitment\([^)]+\)\.(status|startup|shutdown)\s*$"
)


def _is_tautology_single_term(terms: list[str], op: str, rhs: float) -> bool:
    """True when a 1-term ``[±]c × commitment(...).{status,startup,shutdown}``
    constraint is trivially satisfied by the column's natural binary
    domain ``[0, 1]``.

    Rule: with coefficient ``c`` on ``u ∈ [0, 1]`` the LHS range is
    ``[min(0, c), max(0, c)]``.  The constraint is then redundant iff:

      ``≤ rhs`` : ``rhs >= max(0, c)``    e.g. ``+1·u ≤ 1`` always true
      ``≥ rhs`` : ``rhs <= min(0, c)``    e.g. ``-1·u ≥ -1`` always true
      ``= rhs`` : ``c == 0`` and ``rhs == 0``  (degenerate)

    Returns False for any expression that isn't this exact 1-term
    commitment shape — broader tautology detection (multi-term, mixed
    classes) is out of scope here; this function only covers the
    specific SSCC / Uniq family surfaced by the bundle audit
    (``ATA_TG1B_DIE_SSCC``, ``SSCC_NVentanas_{Up,Down}``,
    ``COLMITO_Uniq``, ``CTFOFF_TOCOPILLA_TG3_GNL_B``, etc.).
    """
    if len(terms) != 1:
        return False
    m = _TAUTOLOGY_TERM_RE.match(terms[0].strip())
    if not m:
        return False
    coef = float(m.group(1))
    lhs_max = max(0.0, coef)
    lhs_min = min(0.0, coef)
    if op == "<=":
        return rhs >= lhs_max
    if op == ">=":
        return rhs <= lhs_min
    if op == "=":
        return coef == 0.0 and rhs == 0.0
    return False


def _parse_hydro_maxrampday_csv(path: Path) -> dict[str, list[float]]:
    """Parse ``Hydro_MaxRampDay.csv`` → ``{constraint_name -> per-day RHS}``.

    Layout (long): ``NAME, YEAR, MONTH, DAY, PERIOD, VALUE`` with one row
    per calendar day (PERIOD always 1).  These are the **daily ramp
    limits** that form the right-hand side of PLEXOS hydro ramp
    UserConstraints (e.g. ``RALCOramp_max_e1``).  PLEXOS ships the limit
    as a per-day time series; the constraint's ``t_data`` RHS is a single
    scalar, so without this overlay the daily variation is lost.

    Returns ``{name -> [day0, day1, …]}`` in calendar order.
    """
    out: dict[str, list[float]] = {}
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        for row in csv.reader(fh):
            if not row or not row[0] or row[0] == "NAME":
                continue
            try:
                out.setdefault(row[0].strip(), []).append(float(row[5]))
            except (ValueError, IndexError):
                continue
    return out


#: GWh→MWh scale for PLEXOS daily-ENERGY budgets (``RHS Day`` /
#: ``Hydro_MaxRampDay.csv``).  ``RHS Day`` ships in GWh
#: (``CANUTILLARreserve`` = 4.12 GWh ≈ 99.7% of CANUTILLAR's 172 MW × 24 h
#: daily max; ``RALCOramp_max_e*`` = 4.2 GWh ≈ 51% CF on RALCO_U1's 345 MW);
#: gtopt's ``daily_sum`` + ``constraint_type=energy`` row sums ``gen·Δt``
#: in MWh, so the LP RHS must be ``RHS_Day × 1000``.
_DAILY_ENERGY_RHS_SCALE = 1000.0


#: PLEXOS encodes "this SD_* line-security contingency is INACTIVE today" as an
#: undated base RHS at a no-limit sentinel — a value far above any real flow
#: the constraint's lines could carry.  Two magnitudes are observed in
#: practice (CEN PCP 2026-04-22 audit):
#:
#:   * ``100000`` MW — the "hard sentinel" used by ~50 % of contingency rows;
#:     no CEN line carries 100 GW
#:   * ``10000`` MW — the "soft sentinel" used by 259 pure-line-flow rows
#:     (``2024122225_Changos_*``, ``SD_2025126719_*``, ``SDCF_Rx*``, …); even
#:     when summed across multiple parallel lines (typical pattern
#:     ``0.5 × flow_A + 0.5 × flow_B ≤ 10000``) the LHS peaks well below 10 GW
#:
#: Both magnitudes share the same semantics: when the run date hits no dated
#: override the active RHS is this sentinel, so the constraint is inert (never
#: binds, in gtopt OR PLEXOS); emitting it only wrecks LP conditioning (kappa).
#: Pure-line-flow constraints at/above ``_SD_NOLIMIT_RHS_SENTINEL`` are
#: emitted as inactive stubs (see :func:`_is_nolimit_line_sentinel` and the
#: Fix 6 inactive-stub path).
_SD_NOLIMIT_RHS_SENTINEL = 10000.0

#: PLEXOS emits its "no-limit / contingency-off" RHS values at exact
#: round-magnitude sentinels: ``10000`` (soft) and ``100000`` (hard).
#: Anything BETWEEN these two values (e.g. ``CFRS_PEHUENCHE = 51515.1``)
#: is a *real* MW cap a CEN aggregate may bind on at peak, NOT a
#: sentinel — flagging it inactive silently disables a constraint
#: PLEXOS actively binds (verified 2026-06-03 against PCP_RES_20260118:
#: CFRS_PEHUENCHE bound 126.5 GWh at $38.38/MWh on hour 1).  Match
#: with a tight relative tolerance instead of the open-ended
#: ``>= _SD_NOLIMIT_RHS_SENTINEL`` threshold so legitimate large caps
#: stay active.
_SD_NOLIMIT_EXACT_SENTINELS: tuple[float, ...] = (10000.0, 100000.0)
_SD_NOLIMIT_REL_TOL: float = 1e-6


#: LHS variable kinds that may appear in a sentinel-pattern constraint —
#: all represent physical MW magnitudes that a 10000 / 100000 MW aggregate
#: cap cannot bind on.  Anything else in the LHS (commitment binaries,
#: user decision variables, fuel offtake in fuel units) signals real
#: structural semantics that the sentinel heuristic must not steamroll.
_SENTINEL_MW_KINDS = ("line(", "generator(", "reserve_provision(", "battery(")
_SENTINEL_NON_MW_KINDS = (
    "commitment(",
    "decision_variable(",
    "fuel(",
)


def _is_nolimit_line_sentinel(expression: str, rhs_val: float) -> bool:
    """True for an MW-aggregate constraint at the no-limit sentinel.

    The LHS must reference ONLY MW-magnitude terms (``line(...).flow``,
    ``generator(...).generation``, ``reserve_provision(...).up/.dn``,
    ``battery(...).charge/.discharge``) — any commitment binary, user
    decision variable, or fuel-offtake term signals real structural
    semantics and bypasses the sentinel check.

    Catches both the ``100000`` "hard sentinel" and the ``10000`` "soft
    sentinel" (the lowest sentinel triggers, so the threshold is
    ``_SD_NOLIMIT_RHS_SENTINEL``).  CEN PCP families:
      * 259 pure-line-flow rows (``SD_*``, ``SDCF_Rx*``, ``2024122225_*``)
      * ~30 pure-generator corridor-flow-proxy rows (``Gx_Colbun_Ancoa``,
        ``Gx_Pehuenche_Ancoa``, ``ANGmax``, ``Itahue_Cip`` …)
      * 20 mixed gen+reserve_provision aggregates (``KELAR_Max_Operativo``,
        ``ATA_Max_Operativo`` …)  pattern ``Σ gen + 2 Σ provision ≤ 10000``

    A legitimate cap at 10 GW is not realistic on any CEN single line,
    plant portfolio, or reserve aggregate; emitting these as inactive
    stubs (Fix 6 path) matches PLEXOS, which isn't enforcing them on
    the run date.
    """
    if any(kind in expression for kind in _SENTINEL_NON_MW_KINDS):
        return False
    # Must reference AT LEAST ONE MW kind (an expression with no
    # variable terms at all is not a sentinel — likely a parser bug).
    if not any(kind in expression for kind in _SENTINEL_MW_KINDS):
        return False
    # Match the EXACT sentinel values (10000 and 100000) with a tight
    # relative tolerance.  Any other large RHS (e.g.  ``51515.1``) is a
    # legitimate cap PLEXOS may bind on — don't silently disable it.
    abs_rhs = abs(rhs_val)
    return any(
        abs(abs_rhs - s) <= _SD_NOLIMIT_REL_TOL * s for s in _SD_NOLIMIT_EXACT_SENTINELS
    )


_GAS_MAXOPDAY_NAME_RE = re.compile(r"^Gas_MaxOpDay(\d+)_(.+)$")


def _consolidate_gas_maxopday_groups(
    specs: list["UserConstraintSpec"],
    *,
    block_layout: tuple[tuple[int, ...], ...],
    horizon_hours: float,
    soft_penalty: float,
) -> list["UserConstraintSpec"]:
    """Replace per-block ``Gas_MaxOpDay**X**_<group>`` specs with one
    consolidated ``daily_sum + energy + per-day RHS profile`` UC per
    (fuel, owner-suffix) group.

    Why: PLEXOS evaluates each ``Gas_MaxOpDayX_<group>`` as a per-DAY
    cumulative budget (``Σ_blocks_in_day_X hr·gen·Δt ≤ RHS_X``) for a
    single 24h window — NOT as a per-block uniform cap.  The current
    per-block emission instead enforces ``Σ_g hr·gen ≤ RHS_Custom × 1000
    / horizon_hours`` at EVERY block of the horizon, which over-tightens
    by ~168× vs PLEXOS's actual daily scope (verified via t_data_0
    pid-3069 Activity audit on RES20260422.accdb: Day0_Enel reports a
    single non-zero Activity = 22 at block 2, RHS displayed 0.131 per
    block, Σ slack = 0; Day1_Enel reports per-block Activity over
    blocks 7-24 with daily-cap = 1100 GJ).

    Consolidation: each PLEXOS (fuel, owner) group has 8 Day_X variants
    (Day0..Day7) sharing the same LHS (same fuel → same generators).
    We merge them into ONE UC with:
      * LHS: taken from any of the Day_X specs (they're identical)
      * ``daily_sum = True`` + ``constraint_type = "energy"``
      * ``rhs_profile`` of length n_blocks_per_stage: each block within
        gtopt's day ``d`` carries the per-day cap = (PLEXOS Day_{d+1}
        RHS_Custom × 1000) — gtopt's daily_sum aggregator picks the
        cap from the day-ending block (see
        ``source/user_constraint_lp.cpp:1027``).

    Day-X to gtopt-day mapping: PLEXOS Day0 is a "pre-horizon partial"
    cap that binds at hour 1-2; PLEXOS Day1..Day7 cover the 7 full days
    of the horizon.  We map gtopt day ``d`` → PLEXOS ``Day_{d+1}`` and
    drop PLEXOS Day0 (the LP's first 24h then runs with the wider
    Day1 cap, which is the conservative loose direction — Day0's tighter
    partial-start cap is lost; document as deferred work if it matters
    on a future case).

    Returns a NEW list with the Gas_MaxOpDay specs replaced (other
    specs pass through unchanged).
    """
    name_re = _GAS_MAXOPDAY_NAME_RE
    by_suffix: dict[str, list[tuple[int, "UserConstraintSpec"]]] = {}
    survivors: list["UserConstraintSpec"] = []
    for s in specs:
        m = name_re.match(s.name)
        # Inactive stubs (Fix-6 emissions from extract_user_constraints
        # for all-offline-gen LHSs: ``expression="0 <= 0", active=False``)
        # carry no LHS coefficients and zero RHS, so they're already
        # no-ops in the LP.  Pass them through verbatim — consolidating
        # them would discard the per-day-X audit name that downstream
        # diagnostic tooling (and the test_extract_user_constraints_
        # all_offline_emits_inactive_stub regression) checks for.
        if m and s.active is not False:
            by_suffix.setdefault(m.group(2), []).append((int(m.group(1)), s))
        else:
            survivors.append(s)
    if not by_suffix:
        return list(specs)

    # Determine the block grid the writer will use downstream.  Two
    # cases match the rest of the converter's wiring:
    #   * If ``block_layout`` is non-empty (PLEXOS-native aggregation):
    #     n_blocks_per_stage = len(block_layout); each block "k"
    #     contains the hours block_layout[k-1].
    #   * Otherwise (uniform hourly fallback): n_blocks = horizon_hours,
    #     each block covers one hour, hour h maps to block h.
    if block_layout:
        n_blocks = len(block_layout)
        # block-id (1-indexed) → calendar day index (0-indexed).
        # Use max(hours) for the block's day membership (the day the
        # block ENDS in), matching gtopt's daily_sum day-flush rule.
        block_to_day = [0] * n_blocks
        for k_zero, intervals in enumerate(block_layout):
            if intervals:
                block_to_day[k_zero] = (max(intervals) - 1) // 24
    else:
        n_blocks = max(int(horizon_hours), 1)
        block_to_day = [(h) // 24 for h in range(n_blocks)]

    # Recover per-day-X RHS values from each spec's expression tail.
    # extract_user_constraints emitted them as ``RHS_Custom × 1000 /
    # horizon_hours`` (per-block rate); recover the daily total by
    # multiplying back by horizon_hours.
    rhs_recover_re = re.compile(r"(?:<=|>=|=)\s*(-?[\d\.eE+-]+)\s*$")

    consolidated: list["UserConstraintSpec"] = []
    for suffix, items in sorted(by_suffix.items()):
        items.sort(key=lambda x: x[0])
        # All Day_X share the same LHS — pick the first to inherit
        # expression, penalty, active flag, etc.
        first_spec = items[0][1]
        # Recover each Day_X's daily-total RHS.
        day_x_to_rhs: dict[int, float] = {}
        for day_x, spec in items:
            m_rhs = rhs_recover_re.search(spec.expression)
            if m_rhs:
                per_block_rhs = float(m_rhs.group(1))
                day_x_to_rhs[day_x] = per_block_rhs * horizon_hours

        # Build per-block RHS profile.  For each gtopt day d, pick the
        # matching PLEXOS Day_X via an offset that lines up
        # ``items_count`` PLEXOS indices with ``horizon_days`` gtopt days:
        #
        #   offset = max(0, items_count - horizon_days)
        #   plexos_day_x = gtopt_day_d + offset
        #
        # - CEN PCP weekly: items_count=8 (Day0..Day7), horizon_days=7
        #   ⇒ offset=1 ⇒ gtopt day d ← PLEXOS Day_{d+1}.  PLEXOS Day0
        #   (pre-horizon partial-start cap) is dropped; LP's first 24h
        #   uses the wider Day1 cap.
        # - 1-day test bundle: items_count=1, horizon_days=1 ⇒ offset=0
        #   ⇒ gtopt day 0 ← PLEXOS Day_0.  Direct mapping.
        # - General: items_count <= horizon_days ⇒ direct mapping; tail
        #   gtopt days beyond the available PLEXOS Day_X range get the
        #   BIG sentinel (effectively unconstrained for that day).
        horizon_days_local = max(1, int(horizon_hours) // 24)
        plexos_offset = max(0, len(items) - horizon_days_local)
        BIG = 1e9
        rhs_profile = [BIG] * n_blocks
        for k_zero in range(n_blocks):
            gtopt_day_d = block_to_day[k_zero]
            plexos_day_x = gtopt_day_d + plexos_offset
            cap = day_x_to_rhs.get(plexos_day_x)
            if cap is not None:
                rhs_profile[k_zero] = cap

        # Build consolidated expression: same LHS as Day0_<suffix> (they
        # all share LHS), with a sentinel inline RHS that the profile
        # overrides at every meaningful block.
        lhs_expr = rhs_recover_re.sub("", first_spec.expression).rstrip()
        consolidated_expr = f"{lhs_expr} <= {BIG}"

        consolidated.append(
            UserConstraintSpec(
                name=f"Gas_MaxOpDay_{suffix}",
                expression=consolidated_expr,
                penalty=soft_penalty,
                active=first_spec.active,
                rhs_profile=tuple(rhs_profile),
                daily_sum=True,
                constraint_type="energy",
                # Step 4b (#54) — typed directive carrying the
                # constraint-family classification + scope.  ``kind``
                # tells any JSON consumer "this is a daily fuel-cap
                # constraint" without re-running the name regex.
                # ``scope`` carries the PLEXOS fuel-owner suffix
                # (e.g. ``gas_maxopday:Enel``) so the directive is
                # self-describing in the wire form.  The
                # ``daily_sum=True`` and ``constraint_type=energy``
                # literals above are kept verbatim — gtopt-side
                # ``ConstraintDirective::implies_daily_sum()`` already
                # OR's them on at LP-build time, but the explicit field
                # values are still useful for diff-readability and
                # don't depend on the directive being honoured.
                directive=ConstraintDirective(
                    kind="daily_budget",
                    scope=f"gas_maxopday:{suffix}",
                ),
                description=(
                    f"Gas_MaxOpDay consolidated for fuel-owner group "
                    f"'{suffix}' ({len(items)} PLEXOS Day_X rows merged "
                    f"into one daily_sum UC).  PLEXOS evaluates each "
                    f"Day_X as a per-day cumulative budget "
                    f"(Σ_blocks_in_day_X hr·gen·Δt ≤ RHS_X); the "
                    f"per-block RHS profile here broadcasts the per-day "
                    f"cap to every block in its day so gtopt's daily_sum "
                    f"aggregator (see user_constraint_lp.cpp:1027) picks "
                    f"up the right per-day RHS at each day-ending block."
                    f"  Days mapped: gtopt day d ← PLEXOS Day_{{d+1}}; "
                    f"PLEXOS Day0 partial-start cap is dropped (LP's "
                    f"first 24h uses the wider Day1 cap).  Soft at "
                    f"${soft_penalty:g}/unit since PLEXOS itself takes "
                    f"slack on these (Day1/Day4/Day6 have non-zero pid-"
                    f"3070 Slack in RES20260422.accdb).  "
                    f"File: DBSEN_PRGDIARIO.xml"
                ),
            )
        )

    return survivors + consolidated


#: Coefficient magnitude above which a decision-variable term in a
#: ``physical_output - M*dv <= 0`` availability gate is treated as a big-M
#: to be tightened.  PLEXOS ships ``M = 100000`` (the Decision Variable's
#: ``Value Coefficient``); M only needs to be >= the gated entity's real
#: max power, so the 1e5 coefficient is ~5 orders above the ~300 MW it
#: actually gates — a pure LP condition-number (kappa) liability.
_DV_BIGM_THRESHOLD_MW = 1.0e3

#: Matches ``<coeff> * <kind>("<name>").<attr>`` terms in a UserConstraint
#: expression (the AMPL grammar the gtopt constraint parser consumes).
_GATE_TERM_RE = re.compile(
    r"(?P<coeff>[+-]?\s*[0-9.eE+]+)\s*\*\s*"
    r"(?P<kind>generator|battery|decision_variable)"
    r'\("(?P<name>[^"]+)"\)\.(?P<attr>\w+)'
)


def _tighten_dv_bigm_expression(
    expr: str,
    pmax_by_gen: dict[str, float],
    pmax_discharge_by_bat: dict[str, float],
    pmax_charge_by_bat: dict[str, float],
) -> str | None:
    """Return a rewritten ``expr`` with oversized decision-variable big-M
    coefficients capped at the gated physical capacity, or ``None`` when
    nothing changed.

    Only touches the exact availability-gate pattern ``Σ output - M*dv <=
    0``: an upper-bound row with RHS 0 whose decision-variable coefficient
    magnitude both exceeds :data:`_DV_BIGM_THRESHOLD_MW` AND exceeds the
    summed physical capacity it gates.  Capping ``M`` to that capacity is
    semantics-preserving — the entity's own ``pmax`` bound already binds
    below ``M`` when the gate is open (``dv = 1``) — while collapsing a
    1e5 matrix entry to ~1e2.  Anything else is left byte-for-byte intact.
    """
    m_op = re.search(r"(<=|>=|=)\s*(-?[0-9.eE+]+)\s*$", expr)
    if m_op is None or m_op.group(1) != "<=":
        return None
    try:
        if abs(float(m_op.group(2))) > 1.0e-6:
            return None
    except ValueError:
        return None
    lhs = expr[: m_op.start()]
    terms = list(_GATE_TERM_RE.finditer(lhs))
    if not terms:
        return None

    gate_cap = 0.0
    dv_hits: list[tuple[re.Match[str], float]] = []
    for t in terms:
        try:
            coeff = float(t.group("coeff").replace(" ", ""))
        except ValueError:
            return None
        kind, name, attr = t.group("kind"), t.group("name"), t.group("attr")
        if kind == "generator" and attr == "generation":
            gate_cap += abs(coeff) * pmax_by_gen.get(name, 0.0)
        elif kind == "battery" and attr == "discharge":
            gate_cap += abs(coeff) * pmax_discharge_by_bat.get(name, 0.0)
        elif kind == "battery" and attr == "charge":
            gate_cap += abs(coeff) * pmax_charge_by_bat.get(name, 0.0)
        elif kind == "decision_variable" and attr == "value":
            dv_hits.append((t, coeff))
    if gate_cap <= 0.0 or not dv_hits:
        return None

    new_lhs = lhs
    changed = False
    # Replace right-to-left so earlier match spans stay valid.
    for t, coeff in reversed(dv_hits):
        if abs(coeff) >= _DV_BIGM_THRESHOLD_MW and abs(coeff) > gate_cap:
            capped = -gate_cap if coeff < 0.0 else gate_cap
            token = f"- {gate_cap:g}" if capped < 0.0 else f"+ {gate_cap:g}"
            new_lhs = new_lhs[: t.start("coeff")] + token + new_lhs[t.end("coeff") :]
            changed = True
    if not changed:
        return None
    return new_lhs + expr[m_op.start() :]


def _tighten_decision_variable_bigm(
    ucs: tuple[UserConstraintSpec, ...],
    pmax_by_gen: dict[str, float],
    pmax_discharge_by_bat: dict[str, float],
    pmax_charge_by_bat: dict[str, float],
) -> tuple[UserConstraintSpec, ...]:
    """Cap oversized decision-variable big-M gates across all UCs."""
    out: list[UserConstraintSpec] = []
    n_capped = 0
    for uc in ucs:
        new_expr = _tighten_dv_bigm_expression(
            uc.expression, pmax_by_gen, pmax_discharge_by_bat, pmax_charge_by_bat
        )
        if new_expr is not None:
            out.append(dataclasses.replace(uc, expression=new_expr))
            n_capped += 1
        else:
            out.append(uc)
    if n_capped:
        logger.info(
            "tightened decision-variable big-M on %d availability-gate UCs "
            "(M -> gated capacity; kappa fix)",
            n_capped,
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
    shadow_lines_all_off: frozenset[str] | None = None,
    always_on_gens: frozenset[str] | None = None,
    unusable_provisions: frozenset[str] | None = None,
    stats_out: dict[str, int] | None = None,
    lax_refs: bool = False,
    reserves: tuple[ReserveSpec, ...] = (),
    plexos_legacy: bool = False,
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
      3. ``emitted_names`` is the set of every element gtopt will
         actually emit (ALL generators regardless of pmax — gtopt
         resolves an offline / ``pmax==0`` gen leniently to a 0
         contribution; lines; batteries; synthetic ``<bat>_gen``
         generators; commitments ``uc_<gen>`` / ``uc_<bat>_gen``;
         reserve_provisions by their ACTUAL emitted names; decision
         variables; waterways; reservoirs).  A direct-coefficient term
         whose referenced element is NOT in that set — and which the
         BESS zone-suffix reconciliation (Fix 1) cannot repair — is
         NOT silently dropped.  It is COLLECTED.  After ALL constraints
         are processed, if any unresolved references were collected,
         ONE :class:`UnresolvedConstraintReferenceError` is raised
         listing every offending reference (with the closest emitted
         name as a hint), and the converter exits non-zero.  Pass
         ``None`` to skip validation entirely (unit-test convenience).
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
    # PLEXOS ``RHS Custom`` — the RHS over a *custom time period* (here
    # the run horizon).  Used by the daily gas-offtake constraints
    # (``Gas_MaxOpDay*``, 272 on CEN PCP) which carry NO plain ``RHS``,
    # so without this they were dropped → gas plants over-dispatched.
    # PLEXOS reports the constraint dimensionless and evaluates the
    # effective RHS as ``RHS_Custom × 1000 / horizon_hours`` — verified
    # against the RES solution (ratio = 1000/168 = 5.952, EXACTLY
    # constant across all 124 binding Gas_MaxOpDay constraints / every
    # fuel).  The result is a per-period (per-hour) cap, so the
    # fuel-offtake per-block daily split below is bypassed for it.
    prop_rhs_custom = db.property_by_name(sys_coll.collection_id, "RHS Custom")
    # PLEXOS ``RHS Day`` — the right-hand side of a per-DAY budget,
    # shipped in GWh.  Used by the daily-ENERGY constraints
    # (``CANUTILLARreserve`` = 4.12 GWh ≈ 99.7% of CANUTILLAR's 172 MW ×
    # 24 h max).  Combined with a ``generator(...).generation`` LHS this
    # becomes a gtopt ``daily_sum`` + ``constraint_type=energy`` row:
    # ``Σ_day gen·Δt ≤ RHS_Day × 1000`` [MWh].  The ×1000 GWh→MWh scale
    # is applied once below (``_DAILY_ENERGY_RHS_SCALE``).
    prop_rhs_day = db.property_by_name(sys_coll.collection_id, "RHS Day")
    # PLEXOS evaluates ``RHS Custom`` over the full RUN HORIZON, so the
    # divisor must be the real horizon length (168 h on CEN PCP), NOT
    # ``bundle.n_days × 24``: ``n_days`` is the input-CSV day count (== 1
    # here), so the old expression collapsed to 24 h and inflated every
    # ``Gas_MaxOpDay`` RHS by ``168 / 24 = 7×`` (confirmed against the RES
    # solution: gtopt = PLEXOS × 7 across all 124 constraints).  Take the
    # horizon-day count from the XML ``Horizon`` object name (the
    # authoritative CEN signal, e.g. ``Coordinador_diario_1H_7d`` → 7);
    # fall back to ``n_days or 7`` only when the name is unparseable.
    from .plexos_block_layout import infer_horizon_days_from_input

    _xml_path = getattr(_bundle, "xml_path", None)
    _horizon_days = (
        infer_horizon_days_from_input(_xml_path) if _xml_path is not None else None
    ) or (_bundle.n_days or 7)
    horizon_hours = float(_horizon_days * 24)
    rhs_custom_factor = 1000.0 / horizon_hours if horizon_hours > 0.0 else 0.0
    # Source file for the ``description`` provenance tag (PLEXOS XML DB).
    _uc_source_file = getattr(getattr(_bundle, "xml_path", None), "name", None) or (
        "DBSEN_PRGDIARIO.xml"
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
    #
    # PLEXOS ``Include in ST Schedule`` semantics:
    #   (no entry) = use PROJECT default → INCLUDE
    #            0 = explicitly EXCLUDE from ST Schedule
    #           -1 = use PROJECT default (sentinel form) → INCLUDE
    #          ≥ 1 = explicitly INCLUDE
    #
    # Dual-row resolution (2026-05-30): many CEN PCP constraints carry
    # TWO rows, one untagged (``0`` = exclude) and one tagged with a
    # Scenario object (``-1`` = include).  PLEXOS evaluates the tagged
    # row only when its Scenario is enabled in the Model being run.
    # We resolve by:
    #   1. ask PlexosDb for the active scenario id set (Model →
    #      Scenarios collection)
    #   2. partition rows by tag: (active, default, inactive)
    #   3. active-tagged > default (untagged) > inactive-tagged
    #   4. apply the standard {0 → exclude} rule to the winner
    # Drops 27 wrongly-emitted CEN PCP rows: 15× InflexibilityRule,
    # 8× ManageableRule, 4× ElToroOnlyCPF (each Scenario inactive in
    # ``PRGdia_Full_Definitivo``).
    include_st_excluded: set[int] = set()
    active_scen_ids = db.active_scenario_ids()

    # Sort each constraint's ST-Schedule rows by priority:
    #   tier 0: tagged with an ACTIVE scenario  (override fires)
    #   tier 1: untagged (default — always applies)
    #   tier 2: tagged only with INACTIVE scenarios (ignored entirely)
    # Within the same tier, lowest ``data_id`` wins (matches the
    # legacy single-row ordering).
    def _row_tier(r: PlexosDataRow) -> int:
        tags = db.tag_for_data.get(r.data_id, [])
        if not tags:
            return 1
        if any(t in active_scen_ids for t in tags):
            return 0
        return 2

    if prop_include_st is not None:
        for constr_oid, constr_mid in sys_mid_by_constr.items():
            rows = db.data_for(constr_mid, prop_include_st)
            if not rows:
                continue
            best = min(
                (r for r in rows if _row_tier(r) < 2),
                key=lambda r: (_row_tier(r), r.data_id),
                default=None,
            )
            if best is None:
                # Every row is gated by an inactive scenario → PLEXOS
                # treats the constraint as if no value were set → use
                # project default (INCLUDE).
                continue
            val = best.value
            if val is not None and val == 0.0:
                include_st_excluded.add(constr_oid)
    if include_st_excluded:
        logger.info(
            "PLEXOS Constraint: %d/%d explicitly excluded via "
            '"Include in ST Schedule" (after scenario-tag resolution); '
            "emitting with active=False.",
            len(include_st_excluded),
            len(sys_mid_by_constr),
        )

    objs = db.object_by_id()
    # Horizon start (used by both the RHS-overlay and the coefficient-row
    # date filter below).  Hoisted from its later occurrence so the
    # ``_index_coefficient_rows`` calls inside the direct/derived/
    # reserve indexers (which all run BEFORE the per-constraint loop)
    # get the date-active value selection instead of summing expired
    # plus live coefficient amendments.  ``None`` when the bundle has
    # no Horizon date (unit-test fixtures), preserving legacy
    # "include all rows" behaviour.
    horizon_start = db.horizon_start

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
                _index_coefficient_rows(
                    db, coll.collection_id, prop_id, objs, horizon_start
                ),
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
                db, fuel_coll.collection_id, prop_id, objs, horizon_start
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
        per_constr = _index_coefficient_rows(
            db, coll.collection_id, prop_id, objs, horizon_start
        )
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
                db, rp_coll.collection_id, rp_prop_id, objs, horizon_start
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

    # Reserve→Battery eligibility (BESS-side companion of
    # ``reserve_to_providers``).  Needed for ``BAT_*_CF_GEN_COMP`` /
    # ``CF_LOAD_COMP`` battery-mode complementarity rows: PLEXOS evaluates
    # those with a ``Reserve→Constraint Provision Coefficient`` × battery-
    # scoped sum, but the generator-only expansion above can't reach the
    # SSCC zone-suffixed ``provision_<bat>_gen__<reserve>`` names.  Drop
    # the reserve's BESS direction into ``reserve_direction`` using the
    # zone-suffix convention (``*_LW_BESS`` → dn, ``*_RS_BESS`` → up).
    reserve_to_battery_providers: dict[str, list[str]] = {}
    bess_elig_coll = db.collection_for_named("Reserve", "Battery", "Batteries")
    if bess_elig_coll is not None:
        for parent, children in db.parent_to_children(
            bess_elig_coll.collection_id
        ).items():
            rsv_obj = objs.get(parent)
            if rsv_obj is None:
                continue
            if rsv_obj.name not in reserve_direction:
                upper = rsv_obj.name.upper()
                if "_LW" in upper:
                    reserve_direction[rsv_obj.name] = "dn"
                elif "_RS" in upper:
                    reserve_direction[rsv_obj.name] = "up"
                else:
                    reserve_direction[rsv_obj.name] = "up"
            for cid in children:
                bat_obj = objs.get(cid)
                if bat_obj is not None:
                    reserve_to_battery_providers.setdefault(rsv_obj.name, []).append(
                        bat_obj.name
                    )

    # Decision Variable → Definition (coll 711): DEFINITIONAL equation
    # memberships.  PLEXOS uses this special collection to declare "this
    # constraint DEFINES the value of this DV" with an IMPLICIT +1
    # coefficient (no explicit Value Coefficient row in t_data).  Used by
    # 4 CEN PCP weekly constraints:
    #   * MACHICURA_GENT4def        defines GEN_T4_MACHICURA
    #   * PEHUENCHE_GENT7def        defines GEN_T8_PEHUENCHE
    #   * CTFOFF_CIPRESES_U3_T2def  defines OFF_T2_CIPRESES_U3
    #   * CTFOFF_COLBUN_U2_T2def    defines OFF_T2_COLBUN_U2
    # Without this expansion the DV term silently drops from the LHS,
    # leaving a degenerate residual equation that conflicts with the
    # per-unit bounds (e.g. MACHICURA_GENT4def collapsed to ``-gen_U1
    # - gen_U2 = 0`` forcing both units off all 168 h, breaking
    # MACHICURAmin >= 12 MWh for 108 / 111 blocks).  Detected during
    # the v11.5 HARD-classification experiment which surfaced the
    # infeasibility CPLEX named ``machicura_gent4def_constraint_*``.
    definition_dv_index: dict[int, list[str]] = {}
    def_coll = db.collection_for_named("Decision Variable", "Constraint", "Definition")
    if def_coll is not None:
        for m in db.memberships_of(def_coll.collection_id):
            dv_obj = objs.get(m.parent_object_id)
            if dv_obj is None:
                continue
            definition_dv_index.setdefault(m.child_object_id, []).append(dv_obj.name)

    # Per-constraint battery membership index: ``{constraint_oid -> [bat_name, …]}``.
    # Used to SCOPE the Reserve×Battery provision cross-product to the
    # battery members of a given constraint (e.g. ``BAT_TOCOPILLA_CF_GEN_COMP``
    # restricts the 0.3 × Σ provision_<bat>_gen__CSF_LW_BESS sum to just
    # BAT_TOCOPILLA / BAT_TOCOPILLA_AUX, not all BESS providers).
    constraint_battery_members: dict[int, list[str]] = {}
    bat_constr_coll = db.collection_for_named("Battery", "Constraint", "Constraints")
    if bat_constr_coll is not None:
        for m in db.memberships_of(bat_constr_coll.collection_id):
            bat_obj = objs.get(m.parent_object_id)
            if bat_obj is None:
                continue
            constraint_battery_members.setdefault(m.child_object_id, []).append(
                bat_obj.name
            )

    # Pre-build a set of generator names that are HYDROS — i.e. have
    # no Fuel membership.  PLEXOS UserConstraints whose ENTIRE LHS
    # references hydros are typically per-reservoir operational floors
    # (``PANGUEcaudal_min_diario``, ``ANGOSTURAmin``, ``ANTUCOmin``,
    # ``ELTOROmin``, ``PANGUEramp``, ``ANGOSTURAmaxramp``, …).  PLEXOS
    # Per-tier soft penalties live in :mod:`_uc_policy` (single source of
    # truth shared with :func:`_uc_policy.classify` below).  Kept as local
    # bindings so the few call sites that still need the scalar values
    # (e.g. the Gas_MaxOpDay consolidator) read them by name rather than
    # by magic number.
    _HYDRO_UC_SOFT_PENALTY = _uc_policy.HYDRO_SOFT
    _RESERVE_PROVISION_SUM_PENALTY = _uc_policy.RESERVE_PROV_SUM

    out: list[UserConstraintSpec] = []
    # Constraints dropped because their LHS cannot be faithfully represented
    # (no supported terms, or a partial form after dropping unsupported
    # coefficients).  Reported via ``stats_out`` for the conversion report.
    lhs_dropped = 0
    sd_sentinel_dropped = 0  # no-limit line-security sentinels (see helper)
    # Unresolvable UserConstraint references collected across ALL
    # constraints.  Each entry is ``(constraint_name, "class(\"name\").attr",
    # gtopt_key)`` for a direct-coefficient term whose referenced element is
    # NOT in ``emitted_names`` and could NOT be reconciled (Fix 1).  Instead
    # of silently dropping the term — or the whole constraint — we COLLECT
    # every such reference and, after the build loop, raise ONE big error
    # listing all of them (with the closest emitted name as a hint).  This
    # mirrors gtopt's strict JSON parser, which errors on an unknown field
    # rather than ignoring it.  Empty list ⇒ clean convert.
    unresolved_refs: list[tuple[str, str, str | None]] = []
    unsupported_rhs_shift_warns: set[str] = set()
    # ``horizon_start`` was hoisted near the top of this function (after the
    # ``objs`` map) so the coefficient indexers can apply date filtering.
    # Per-day hydro ramp RHS overlay (``Hydro_MaxRampDay.csv``): supplies
    # the daily-varying right-hand side for the ``*ramp*`` hydro
    # UserConstraints whose ``t_data`` carries only a static scalar.
    ramp_day_rhs: dict[str, list[float]] = {}
    if _bundle.has("Hydro_MaxRampDay.csv"):
        ramp_day_rhs = _parse_hydro_maxrampday_csv(_bundle.csv("Hydro_MaxRampDay.csv"))
    for constr in constraints:
        # Fix 6: per-constraint counter of LHS terms silently dropped
        # because their physical contribution is provably zero at this
        # run date.  Three categories funnel into this counter:
        #   * shadow Lines with ``Lin_Units=0`` all-horizon (Fix 3 —
        #     the PLEXOS contingency state is inactive),
        #   * fully-offline Generators (``pmax == 0`` and ``pmax_profile``
        #     absent or all-zero — gtopt never materialises a generation
        #     column for them, but the term mathematically contributes
        #     ``coeff × 0 = 0`` to the LHS),
        #   * ``Fuel.Offtake`` terms whose fuel has no consuming
        #     generators in the emitted set (Σ over zero gens = 0).
        # When EVERY LHS term lands in one of these three buckets, the
        # constraint reduces to ``0 <op> rhs`` at this run date.
        # Previously dropped at the empty-LHS guard, which left
        # spurious gaps in the PLEXOS-sol → gtopt audit.  Now emit an
        # inactive stub (``0 <op> 0``) carrying the PLEXOS name + a
        # description noting the no-op cause, so the bundle preserves
        # provenance and the constraint can be re-activated in a
        # follow-up run where the contingency / gens come online.
        silent_zero_drops = 0
        mid = sys_mid_by_constr.get(constr.object_id)
        if mid is None:
            continue
        sense_val = _horizon_value(db.data_for(mid, prop_sense), horizon_start)
        # RHS uses the relaxed (min) value among horizon-active rows: PLEXOS
        # ships dated historical overrides that ``_first_value`` would pick
        # by mistake, forcing units committed (e.g. CAMPICHE / coal staying
        # on all week instead of shutting down like PLEXOS).  See
        # ``_horizon_value``.
        rhs_rows = db.data_for(mid, prop_rhs)
        # Scenario-tag resolution (symmetric with the "Include in ST
        # Schedule" resolver above): PLEXOS applies an RHS row only when
        # its Scenario tag is enabled in the Model being run.  A row
        # tagged ONLY with inactive scenarios (``_row_tier`` == 2) is a
        # losing OVERRIDE whenever an untagged/active-tagged row competes
        # for the same constraint — keeping it folds an inactive-band
        # value into the emitted RHS.  Drop those tier-2 rows, BUT ONLY
        # when at least one tier<2 row survives: when EVERY row is tier-2
        # the lone scenario-tagged value IS the effective RHS PLEXOS
        # applies (mirrors the Include-ST resolver's "all rows inactive →
        # fall back to the value" behaviour), so dropping it would wrongly
        # delete the whole constraint.
        #
        # Verified on CEN PCP DATOS20251005 ``Reg_SouthZone``: base
        # RHS=320 + untagged H11-17→230 win, while tagged H9-10→196.6 and
        # H18-20→187.42 (inactive scenario 4157) are suppressed —
        # PLEXOS's solution RHS is exactly {320, 230}, NOT the 4-level
        # profile gtopt emitted before this filter.  Conversely
        # ``CTF_DownMinProvision`` carries a SINGLE tier-2 row (293, tag
        # 4114): PLEXOS still applies it, so it is kept.
        _kept = [r for r in rhs_rows if _row_tier(r) < 2]
        if _kept:
            rhs_rows = _kept
        rhs_val = _horizon_value(rhs_rows, horizon_start, prefer_min=True)
        # PLEXOS timeslice tags (``<t_text class_id=76>``: ``H16-20``,
        # ``H1-8,H20-24``, ``W2-6,H8-21``) modulate individual RHS rows
        # to a recurring hour-of-day / day-of-week pattern.  When ANY
        # active row carries a timeslice tag, build a per-block RHS
        # profile by overlaying tagged-value masks on top of the
        # untagged (or all-blocks) base.  This is what makes
        # ``Campiche_starting`` cap starts only during evening peak,
        # ``Commit_Atacama_*`` allow CC startup during shoulder hours,
        # ``PANGUEramp`` enforce 60 MW/h during ramp windows, etc.
        # Falls back silently to the scalar path when no timeslice tag
        # is present on any active row.  See :func:`_expand_timeslice`.
        rhs_timeslice_profile: tuple[float, ...] = ()
        if rhs_val is not None and any(
            r.timeslice for r in rhs_rows if _has_active_rhs_row([r], horizon_start)
        ):
            try:
                rhs_timeslice_profile = _build_rhs_timeslice_profile(
                    rhs_rows, rhs_val, horizon_start, _horizon_days * 24
                )
            except ValueError as exc:
                logger.warning(
                    "constraint %s: timeslice expansion failed (%s); "
                    "falling back to scalar RHS",
                    constr.name,
                    exc,
                )
        # PLEXOS date-window overrides whose window covers only PART of
        # the run horizon — ``_horizon_value`` collapses them to a single
        # scalar via its all-or-nothing horizon_start membership check,
        # so partial-window contingency rows (``SD_2026030813_NvaPAzucar_
        # Polpaico500_*`` with RHS=1600 active 06:00-10:00 on Apr 22,
        # ``SD_2026036857_LVilos_*`` with RHS=896 active 23:00-06:00)
        # silently fall back to their undated RHS=10000 sentinel and get
        # stubbed as inactive — yet PLEXOS pays $4.15M of soft slack on
        # these four constraints alone, proving they bind on the in-window
        # blocks.  Build a per-block profile that applies the dated value
        # exactly on the blocks it covers and the undated base on the rest.
        # When the resulting profile already has a (lower-precedence)
        # timeslice profile, prefer the timeslice — date-overlay is
        # invoked only when no timeslice overlay applied.
        rhs_date_overlay_profile: tuple[float, ...] = ()
        if (
            not rhs_timeslice_profile
            and horizon_start is not None
            and any(
                (r.date_from is not None or r.date_to is not None) and not r.timeslice
                for r in rhs_rows
            )
        ):
            rhs_date_overlay_profile = _build_rhs_date_overlay_profile(
                rhs_rows,
                rhs_val if rhs_val is not None else 0.0,
                horizon_start,
                _horizon_days * 24,
            )
            # When the date overlay activates, the constraint is no
            # longer at the sentinel for every block.  Reset the scalar
            # ``rhs_val`` to a representative non-sentinel value (the
            # min) so the ``_is_nolimit_line_sentinel`` check below does
            # not stub the entire UC inactive.
            if rhs_date_overlay_profile:
                non_sentinel = [
                    v
                    for v in rhs_date_overlay_profile
                    if abs(v) < _SD_NOLIMIT_RHS_SENTINEL
                ]
                if non_sentinel:
                    rhs_val = min(non_sentinel, key=abs)
        # ``_horizon_value`` falls back to an expired dated row when no row is
        # active at the run date and no undated base exists — that value is a
        # stale historical override, not the live bound.  Flag it so an active
        # ``RHS Day`` (below) can take over.
        rhs_from_expired_only = bool(rhs_rows) and not _has_active_rhs_row(
            rhs_rows, horizon_start
        )
        # Fall back to the custom-time-period RHS (Gas_MaxOpDay*): apply
        # PLEXOS's ``× 1000 / horizon_hours`` evaluation so the value
        # matches the solved effective RHS.  ``rhs_from_custom`` marks
        # the result as a per-period cap (skip the daily /24 split).
        rhs_from_custom = False
        if rhs_val is None and prop_rhs_custom is not None and rhs_custom_factor > 0.0:
            rhs_custom = _horizon_value(
                db.data_for(mid, prop_rhs_custom), horizon_start, prefer_min=True
            )
            if rhs_custom is not None:
                rhs_val = rhs_custom * rhs_custom_factor
                rhs_from_custom = True
        # PLEXOS ``RHS Day`` — a per-DAY budget in GWh.  Some daily-energy
        # constraints (e.g. ``CANUTILLARreserve`` = 4.12 GWh) carry NO plain
        # ``RHS`` / ``RHS Custom``, only ``RHS Day``; without seeding it here
        # they hit the ``rhs_val is None`` guard below and are dropped.  Seed
        # the scalar from ``RHS Day`` (GWh) — the GWh→MWh ×1000 scale is
        # applied later, only when this turns out to be a daily-ENERGY
        # (``generator(...).generation`` LHS) constraint.
        rhs_from_day = False
        if prop_rhs_day is not None:
            day_rows = db.data_for(mid, prop_rhs_day)
            # Use RHS Day when there is no plain RHS, OR when the only plain
            # RHS is an expired historical override AND an ACTIVE RHS Day
            # exists (the live daily budget then wins — e.g. PANGUE).
            use_day = rhs_val is None or (
                rhs_from_expired_only and _has_active_rhs_row(day_rows, horizon_start)
            )
            if use_day:
                rhs_day = _horizon_value(day_rows, horizon_start, prefer_min=True)
                if rhs_day is not None:
                    rhs_val = rhs_day
                    rhs_from_day = True
        # Hydro daily-ramp constraints (e.g. ``RALCOramp_max_e1``) carry NO
        # ``RHS`` / ``RHS Custom`` in the PLEXOS DB — their right-hand side
        # lives entirely in ``Hydro_MaxRampDay.csv``.  Without seeding it
        # here the constraint hits the ``rhs_val is None`` guard below and
        # is silently dropped.  Use the first per-day value as the scalar
        # seed; the per-block overlay further down replaces it with the
        # full day-varying schedule.
        ramp_day_present = bool(ramp_day_rhs.get(constr.name))
        if rhs_val is None and ramp_day_present:
            rhs_val = ramp_day_rhs[constr.name][0]
        # PLEXOS default for unset ``Sense`` (verified on CEN PCP
        # 2026-04-22): 62 constraints carry ``RHS=0`` with no explicit
        # Sense — PLEXOS evaluates these as equality (``=``).  Affected
        # families: ``_ConfTGA*`` / ``_ConfTGB*`` / ``_ConfTV`` plant-
        # config linking (ATA / KELAR / NEHUENCO / SANISIDRO / CANDELARIA
        # / MEJILLONES / QUINTERO / TALTAL — 24 total),
        # ``BAT_*_CF_GEN_COMP`` / ``CF_LOAD_COMP`` battery-reserve
        # composition (10), ``*_CPF_Simmetry`` regulation-reserve
        # symmetry (10), ``Inertia_Calculation_e*`` (2) and other
        # equality-form internals (16).  Previously dropped at this
        # guard → ATA / KELAR / NEHUENCO / SANISIDRO mutex broken →
        # multiple plant configs committed simultaneously (e.g. 14
        # ATA configs in every block of mip_v2).
        if sense_val is None and rhs_val is not None:
            sense_val = 0.0  # PLEXOS default: equality
        if sense_val is None or rhs_val is None:
            continue
        op = _SENSE_OP.get(sense_val)
        if op is None:
            logger.debug("constraint %s has unknown Sense %s", constr.name, sense_val)
            continue
        # ── Sense override for BAT_*_CF_GEN_COMP / CF_LOAD_COMP ──
        # The ``_SENSE_OP`` table maps ``0.0 → ">="`` (legacy default
        # for the broad set of unset-Sense constraints PLEXOS ships),
        # but for BAT_<bat>_CF_<dir>_COMP the legacy ``>=`` makes no
        # physical sense — it would force ``reserve_provision ≥
        # activity / 0.3`` (a LOWER bound forcing the battery to
        # ALWAYS commit reserve when active), which doesn't match
        # either physics or PLEXOS.
        #
        # Two defensible choices:
        #
        #   * ``<=`` (DEFAULT, physical): ``reserve_provision ≤
        #     activity / 0.3`` — UPPER bound on reserve from
        #     operational engagement.  Captures the real physics
        #     ("you can't promise more reserve than you can deliver")
        #     without forcing the LP to commit reserve unnecessarily.
        #     Avoids the negative-LMP artifact (no equality-binding
        #     opportunity cost contaminating downstream prices).
        #
        #   * ``=`` (PLEXOS-faithful, opt-in via ``--plexos-legacy``):
        #     ``reserve_provision = activity / 0.3`` — tight
        #     complementarity that matches PLEXOS's auto-expansion of
        #     these constraints.  Produces the negative LP shadow
        #     prices (-$31 to -$54 in v0407 sol) and the corresponding
        #     northern-BESS-bus negative LMPs (Andes220 -$6.38,
        #     MariaElena220 -$1.30).  Use for PLEXOS-comparison tests;
        #     not recommended for production gtopt because the
        #     negative duals propagate through the LP shadow price
        #     stack.
        #
        # Surgical scope: only for ``_is_bat_complementarity`` family;
        # the other ~52 default-equality constraints (``_ConfTGA*``,
        # ``*_CPF_Simmetry``, ``Inertia_Calculation_e*``) keep the
        # legacy ``>=`` pending a separate sense-audit.
        if sense_val == 0.0 and _uc_policy._is_bat_complementarity(constr.name):
            op = "=" if plexos_legacy else "<="
        penalty_val = (
            _horizon_value(db.data_for(mid, prop_penalty), horizon_start)
            if prop_penalty
            else None
        )
        penalty_val = (
            _horizon_value(db.data_for(mid, prop_penalty), horizon_start)
            if prop_penalty
            else None
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
        # Track parent generator names referenced (any class).  Used
        # below to soften UCs whose entire LHS references hydros —
        # PLEXOS gates hydro per-reservoir floors on commitment
        # status internally; our extractor emits them as hard floors
        # which collide with off-state dispatch.
        referenced_gen_names: set[str] = set()
        # Per-block RHS shift bookkeeping — accumulated from both the
        # Fuel.Offtake daily-to-block budget split AND the derived
        # Curtailed / Available Capacity coefficients below.  Hoisted
        # to the top of the per-constraint scope so all downstream
        # sections can append to it.
        gen_pmax_by_name = pmax_by_gen or {}
        gen_pmax_profiles = pmax_profiles_by_gen or {}
        rhs_shift_per_block: list[float] = []

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
                ref_name = name_tmpl.format(name=parent_name)
                # The term is unresolvable when EITHER the underlying
                # PLEXOS parent object was not emitted (``allowed_parent``)
                # OR the synthesized gtopt element name was not emitted
                # (``allowed_ref``).  Both funnel into the same
                # collect-and-fail path below — a parent that gtopt
                # never emits cannot have a resolvable child reference.
                parent_missing = (
                    allowed_parent is not None and parent_name not in allowed_parent
                )
                ref_missing = allowed_ref is not None and ref_name not in allowed_ref
                if parent_missing or ref_missing:
                    # A Battery reserve-provision coefficient references
                    # the BARE battery name, so the direct builder produced
                    # the plain ``provision_<bat>`` — which the SSCC emitter
                    # never creates (its names are ZONE-suffixed,
                    # ``provision_<bat>_gen__<ZONE>``).  Route the term to
                    # EVERY zone-suffixed provision matching the
                    # coefficient's DIRECTION (from the KIND-derived
                    # ``accessor``, else the constraint name) and the
                    # constraint's reserve TYPE (CPF / CSF / both).  A
                    # per-battery row (``CSF_LW_MIN_BAT_<bat>``) yields one
                    # zone; an aggregate / storage-bound row
                    # (``CPF_DownMinProvision``, ``UP/DOWNStorageBound_*``)
                    # may yield a SUM over both reserve types — emit one
                    # term per matching zone.
                    if gtopt_class == "reserve_provision" and parent_class == "Battery":
                        direction = _bess_provision_direction(constr.name, accessor)
                        bess_refs = (
                            _bess_matching_provisions(
                                parent_name, direction, constr.name, allowed_ref
                            )
                            if direction is not None
                            else []
                        )
                        if bess_refs:
                            for rn in bess_refs:
                                terms.append(
                                    _format_coefficient(coeff, first=not terms)
                                    + f'reserve_provision("{rn}").{direction}'
                                )
                                coefficients.append(coeff)
                            continue
                    # Fix 5 (PLEXOS ``BAT_*_AUX`` virtual buffer): the
                    # converter drops PLEXOS's auxiliary battery
                    # modeling artifacts (``BAT_DEL_DESIERTO_AUX``,
                    # ``BAT_TOCOPILLA_AUX``, ``BAT_MANZANO_FV_AUX``,
                    # ``BAT_DON_HUMBERTO_FV_AUX``,
                    # ``BAT_LA_CABANA_EO_AUX`` — see the ``_AUX``
                    # filter at extract_batteries:1870), since gtopt
                    # models a single battery covering both energy
                    # dispatch + reserve provision.  But PLEXOS's
                    # ``BAT_<name>_CF_GEN_COMP`` / ``CF_LOAD_COMP``
                    # reserve-flow composition constraints reference
                    # the AUX battery's discharge / charge in their
                    # LHS.  Redirect the term to the main battery
                    # (``BAT_<name>_AUX`` → ``BAT_<name>``) when the
                    # main is emitted — this preserves the
                    # composition equation against the single
                    # battery's dispatch column, the closest gtopt
                    # equivalent of PLEXOS's two-battery model.
                    #
                    # CRITICAL — battery activity-flow expansion for
                    # ``BAT_*_CF_GEN_COMP`` / ``CF_LOAD_COMP``: PLEXOS's
                    # ``BAT_<name>_AUX`` is a synthetic "mirror battery"
                    # whose ``Generation`` variable equals the main's
                    # ``Load`` when charging (verified on v0407 PLEXOS
                    # sol: AUX.gen = MAIN.load at midday for all 4
                    # northern batteries).  Emitting only
                    # ``-1 × main.discharge`` (the naive AUX→main
                    # rewrite) gives a degenerate constraint at solar
                    # peak — ``main.discharge = 0`` while charging,
                    # so the LHS is trivially zero regardless of how
                    # much reserve the battery promises.  Result: the
                    # LP free-rides on reserve provision without any
                    # operational coupling, missing PLEXOS's negative
                    # LMPs at the northern BESS buses (Andes220
                    # min -$6.38, MariaElena220 min -$1.30 — verified
                    # 2026-05-31).
                    #
                    # The faithful expansion is the L1 of net dispatch:
                    # ``aflow = charge + discharge``.  Both legs share
                    # the same coefficient (PLEXOS's AUX.gen tracks
                    # MAIN.load on the charge side at α=1.0; tracks
                    # MAIN.gen on the discharge side at α≈0.4-0.5 with
                    # per-block variation we can't recover from XML —
                    # use 1.0 as a conservative-but-correct upper
                    # bound that keeps the constraint binding at the
                    # right times.  Future tuning may apply per-block
                    # coefficients from ``ReserveUsageTxCompensation.csv``).
                    if (
                        parent_class == "Battery"
                        and parent_missing
                        and parent_name.endswith("_AUX")
                    ):
                        main_name = parent_name[:-4]
                        if allowed_parent is not None and main_name in allowed_parent:
                            redirected_ref = name_tmpl.format(name=main_name)
                            # Emit the primary leg (the original accessor,
                            # typically ``.discharge`` for Generation
                            # Coefficient on AUX).
                            terms.append(
                                _format_coefficient(coeff, first=not terms)
                                + f'{gtopt_class}("{redirected_ref}").{accessor}'
                            )
                            coefficients.append(coeff)
                            # For BAT_*_CF_GEN_COMP / CF_LOAD_COMP add
                            # the COMPLEMENTARY leg with the SAME
                            # coefficient so the constraint binds in
                            # both battery operating modes (the L1 of
                            # net dispatch).  Without this, the
                            # constraint never binds at solar peak when
                            # battery is charging (main.discharge = 0).
                            if (
                                gtopt_class == "battery"
                                and _uc_policy._is_bat_complementarity(constr.name)
                            ):
                                complement = (
                                    "charge" if accessor == "discharge" else "discharge"
                                )
                                terms.append(
                                    _format_coefficient(coeff, first=not terms)
                                    + f'{gtopt_class}("{redirected_ref}").{complement}'
                                )
                                coefficients.append(coeff)
                            logger.debug(
                                "constraint %s: redirected dropped "
                                "auxiliary battery '%s' → '%s' (main "
                                "battery handles dispatch + reserves in "
                                "gtopt's single-battery model)",
                                constr.name,
                                parent_name,
                                main_name,
                            )
                            continue
                    # Fix 3 (PLEXOS contingency-state shadow Lines): when a
                    # Line term references a parent whose ``Lin_Units.csv``
                    # profile is all-zero across the horizon, PLEXOS itself
                    # pins the line's flow to 0 — the term mathematically
                    # contributes 0 to the LHS regardless of its
                    # ``Flow Coefficient``.  This is exactly how PLEXOS
                    # models inactive contingency states (the ``_I/_II/_III``
                    # per-circuit shadows and the ``_SC`` Sin-Compensar
                    # variants stay at Units=0 until their contingency
                    # activates).  Drop the term silently (matches PLEXOS's
                    # zero-flow contribution) rather than fail-hard as if
                    # the name were a typo — a future case where a
                    # contingency IS active will need real per-circuit
                    # emission, but for the inactive-contingency days that
                    # dominate PCP runs the term is genuinely 0.
                    if (
                        parent_class == "Line"
                        and shadow_lines_all_off is not None
                        and parent_name in shadow_lines_all_off
                    ):
                        silent_zero_drops += 1
                        logger.debug(
                            "constraint %s: dropping term for shadow Line "
                            "'%s' (Lin_Units=0 all-horizon, PLEXOS "
                            "contingency-state inactive; term contributes 0)",
                            constr.name,
                            parent_name,
                        )
                        continue
                    # Fix 4 (renewable without Commitment row): wind/solar
                    # plants run intermittently but PLEXOS treats their
                    # ``commitment.status`` as a constant ``1`` (always
                    # committed — there's no on/off decision for a wind
                    # farm or PV array).  The converter does NOT emit a
                    # ``CommitmentSpec`` for these plants (no Min Stable
                    # Level / Start Cost / Min Up-Down on the PLEXOS side),
                    # so any UC term ``coeff * commitment("uc_<gen>").status``
                    # has no LP column to bind to.  In PLEXOS's LP the term
                    # contributes a constant ``coeff * 1`` to the LHS;
                    # mirror that by ABSORBING the contribution into the
                    # RHS (``rhs_val -= coeff``), exactly the same physics
                    # without needing a phantom commitment variable.
                    #
                    # Example: ``CSF_MinUnits``' renewable terms (11 plants,
                    # each ``coeff=1``) shift the RHS from 3 to ``3-11=-8``,
                    # making the residual ``Σ status(committable) ≥ -8``
                    # which is trivially satisfied by the remaining
                    # commitments — matching PLEXOS exactly (Slack=0..24.6,
                    # Marginal=0 in PLEXOS solution).
                    #
                    # ``startup`` / ``shutdown`` terms on always-on gens
                    # contribute 0 (the unit never transitions), so we
                    # just drop them without an RHS shift.
                    if (
                        parent_class == "Generator"
                        and gtopt_class == "commitment"
                        and always_on_gens is not None
                        and parent_name in always_on_gens
                    ):
                        if accessor == "status":
                            rhs_val -= coeff
                            logger.debug(
                                "constraint %s: absorbing always-on "
                                "commitment.status of renewable '%s' into "
                                "RHS (coeff=%g shifted; new rhs_val=%g)",
                                constr.name,
                                parent_name,
                                coeff,
                                rhs_val,
                            )
                        else:
                            logger.debug(
                                "constraint %s: dropping commitment.%s "
                                "for always-on renewable '%s' (never "
                                "transitions; term contributes 0)",
                                constr.name,
                                accessor,
                                parent_name,
                            )
                        continue
                    # Fix 2: the referenced element was never emitted and
                    # could not be reconciled (BESS Fix above did not
                    # apply).  Do NOT silently drop the term, and do NOT
                    # quietly drop the whole constraint — COLLECT the bad
                    # reference and FAIL HARD after every constraint has
                    # been walked (one big error listing them all).  This
                    # mirrors gtopt's strict JSON parser, which errors on
                    # an unknown field rather than ignoring it.
                    unresolved_refs.append(
                        (
                            constr.name,
                            f'{gtopt_class}("{ref_name}").{accessor}',
                            gtopt_key,
                        )
                    )
                    continue
                # Fix 5 (fully-offline generator → zero dispatch /
                # zero reserve provision): when a UC term references
                # ``generator("<X>").generation`` or
                # ``reserve_provision("provision_<X>").{up,dn}`` for a
                # gen whose ``pmax == 0`` AND whose ``pmax_profile`` is
                # absent or all-zero (PANGUE_U1 on this PCP day,
                # ANTUCO_U2, COLBUN_U1, NEHUENCO_1-FA_GN_A, …), PLEXOS
                # resolves the dispatch / provision to 0 in its LP —
                # the term contributes ``coeff × 0 = 0`` to the LHS.
                #
                # gtopt's per-block resolver throws on such a
                # reference because the LP column / attribute was
                # never materialised for the zero-pmax gen (see
                # ``element_column_resolver.cpp`` — strict on missing
                # ``.generation``; ``reserve_provision_lp.cpp:383-392``
                # — doesn't register ``.up`` / ``.dn`` when bounds are
                # ``[0, 0]``).  Mirror PLEXOS by simply dropping the
                # term (no RHS shift; ``coeff × 0 = 0`` is the same
                # arithmetic).  Symmetric with the always-on renewable
                # shift on the ``commitment.status`` side, and with the
                # fuel-offtake skip below.
                offline_attr = parent_class == "Generator" and (
                    (gtopt_class == "generator" and accessor == "generation")
                    or (gtopt_class == "reserve_provision" and accessor in ("up", "dn"))
                )
                if offline_attr and pmax_by_gen is not None:
                    # Only activate the offline-drop when the caller
                    # actually supplied pmax data — bare callers
                    # (legacy unit tests) get the legacy behaviour of
                    # emitting the term unconditionally.
                    pmax = gen_pmax_by_name.get(parent_name, 0.0)
                    profile = gen_pmax_profiles.get(parent_name)
                    profile_all_zero = profile is None or not any(
                        p > 0.0 for p in profile
                    )
                    if pmax == 0.0 and profile_all_zero:
                        silent_zero_drops += 1
                        logger.debug(
                            "constraint %s: dropping term for fully-"
                            "offline generator '%s' (pmax=0, no/all-zero "
                            "profile; %s.%s contributes 0)",
                            constr.name,
                            parent_name,
                            gtopt_class,
                            accessor,
                        )
                        continue
                # Fix 6 (zone-less provision → .up/.dn not materialised):
                # provisions emitted with an empty ``reserve_zones`` tuple
                # (the ``extra_provision_gens`` path for UC-referenced-
                # but-not-Reserve-member generators) don't get ``.up``
                # / ``.dn`` AMPL columns in gtopt's
                # ``reserve_provision_lp.cpp`` — without zone
                # participation the provision has no balance row to
                # contribute to.  PLEXOS resolves the term to 0 (no
                # zone → no headroom).  Drop the term silently.
                if (
                    gtopt_class == "reserve_provision"
                    and accessor in ("up", "dn")
                    and unusable_provisions is not None
                    and ref_name in unusable_provisions
                ):
                    logger.debug(
                        "constraint %s: dropping term for zone-less "
                        "provision '%s'.%s (no reserve_zones → no LP "
                        "headroom column; contributes 0)",
                        constr.name,
                        ref_name,
                        accessor,
                    )
                    continue
                var_ref = f'{gtopt_class}("{ref_name}").{accessor}'
                terms.append(_format_coefficient(coeff, first=not terms) + var_ref)
                coefficients.append(coeff)
                # Record parent gen names that we actually emitted
                # (so the post-loop hydro-classification skips terms
                # that were filtered out above).
                if parent_class == "Generator":
                    referenced_gen_names.add(parent_name)

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
        # PLEXOS ``Offtake Coefficient`` UCs translate verbatim as
        # ``α × fuel("<name>").offtake`` using gtopt's native FuelLP
        # offtake decision variable (per-block ``Y_f[b]`` bound by
        # ``Y_f − Σ hr·dur·gen = 0`` — see source/fuel_lp.cpp).  This
        # replaces the earlier per-generator expansion
        # ``Σ_g α·hr_g·gen_g``: same physics, ONE LHS term instead of
        # N, the coefficient is the PLEXOS one (no heat-rate baked in),
        # and the offline-gen leniency lives in FuelLP::add_to_lp's
        # ``is_active(stage)`` + ``hr <= 0`` guards rather than being
        # duplicated here.  When emitted_names is supplied AND the fuel
        # is absent from it (rare — happens if extract_fuels skipped
        # the fuel for some reason), fall through to the legacy per-gen
        # expansion so the constraint still gets emitted.
        allowed_fuels = emitted_names.get("Fuel") if emitted_names is not None else None
        # Modern ``α × fuel(name).offtake`` emission is gated OFF by
        # default pending a gtopt-side bug fix: FuelLP::add_to_lp
        # registers the offtake LP column conditionally on
        # ``gen.is_active(stage)`` + ``hr > 0`` + ``gcols.find(buid)``,
        # which leaves some fuel/(stage, block) cells without a
        # column even though gens consume the fuel.  The strict UC
        # resolver then errors "unknown attribute 'offtake' on fuel
        # 'X'" at LP-build time.  Set env ``GTOPT_USE_FUEL_OFFTAKE=1``
        # to opt back in once the FuelLP edge case is resolved
        # (test_fuel_offtake.cpp already covers the simple cases).
        import os as _os_fuel

        _use_fuel_offtake = _os_fuel.environ.get(
            "GTOPT_USE_FUEL_OFFTAKE", ""
        ).strip() in ("1", "true", "yes")
        for fuel_name, alpha in fuel_offtake_index.get(constr.object_id, ()):
            if _use_fuel_offtake and (
                allowed_fuels is None or fuel_name in allowed_fuels
            ):
                # Modern path: single ``α × fuel(name).offtake`` term.
                # Filter fuels with NO emitted consumers (FuelLP early-exit).
                consuming = fuel_to_gens.get(fuel_name, ())
                if allowed_gens is not None:
                    consuming = [g for g in consuming if g in allowed_gens]
                if not consuming:
                    silent_zero_drops += 1
                    continue
                var_ref = f'fuel("{fuel_name}").offtake'
                terms.append(_format_coefficient(alpha, first=not terms) + var_ref)
                coefficients.append(alpha)
                is_fuel_offtake = True
                continue
            # Legacy per-gen expansion (default — see env var above).
            for gen_name in fuel_to_gens.get(fuel_name, ()):
                if allowed_gens is not None and gen_name not in allowed_gens:
                    continue
                hr = gen_heat_rate.get(gen_name, 0.0)
                if hr == 0.0:
                    continue
                if pmax_by_gen is not None:
                    pmax = gen_pmax_by_name.get(gen_name, 0.0)
                    profile = gen_pmax_profiles.get(gen_name)
                    profile_all_zero = profile is None or not any(
                        p > 0.0 for p in profile
                    )
                    if pmax == 0.0 and profile_all_zero:
                        # Symmetric with the direct-coefficient offline-gen
                        # drop (Fix 5): increment silent_zero_drops so the
                        # Fix 6 inactive-stub path catches the constraint
                        # when EVERY consuming gen lands offline (e.g.
                        # ``Gas_MaxOpDay*_Colbun_GNL_INF`` references the
                        # ``_INF`` infinity tier whose gens have pmax=0).
                        silent_zero_drops += 1
                        logger.debug(
                            "constraint %s: dropping fuel-offtake term "
                            "for fully-offline generator '%s' "
                            "(pmax=0, no/all-zero profile)",
                            constr.name,
                            gen_name,
                        )
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
        if is_fuel_offtake and not rhs_from_custom:
            # GWh→MWh fuel-unit scale: when the RHS comes from PLEXOS's
            # ``RHS Day`` property (input-CSV units = GWh of fuel-energy)
            # but the LHS coefficients are emitted in MWh-equivalent
            # units, the cap must be scaled by 1000 to align the
            # per-block RHS with PLEXOS sol's reported per-block bound.
            # Verified on CEN PCP RES20260422 (``Diesel_OffTakeDay``):
            #
            #   raw RHS Day  = 9.277   (PLEXOS XML)
            #   gtopt before = 0.386   (= 9.277 / 24, missing scale)
            #   PLEXOS sol   = 386.54  (= 9.277 × 1000 / 24)
            #   ratio        = 1000   → apply ``_DAILY_ENERGY_RHS_SCALE``
            #
            # Applied here (NOT via the ``is_daily_energy`` branch at
            # line ~7440, which is explicitly excluded for fuel-offtake
            # to avoid double-scaling).  Plain ``RHS`` (without ``Day``)
            # is in native LP units and must NOT be scaled — gated on
            # ``rhs_from_day`` so synthetic-test fuel-offtake UCs that
            # carry a plain ``RHS`` (e.g. unit-test fixtures) pass
            # through unchanged.
            if rhs_from_day:
                rhs_val *= _DAILY_ENERGY_RHS_SCALE
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
                _set_shift(
                    rhs_shift_per_block,
                    idx,
                    _shift_at(rhs_shift_per_block, idx) + shift,
                )

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
                # Fix 5 (symmetric with the direct-coefficient + ramp +
                # fuel-offtake offline-gen skips): zero-pmax gen with
                # no/all-zero profile has a [0, 0]-bounded
                # ReserveProvisionSpec; gtopt's
                # ``reserve_provision_lp.cpp`` doesn't register
                # ``.up``/``.dn`` AMPL accessors on a zero-bounded
                # provision, so the term throws.  PLEXOS resolves it
                # to 0 (no available headroom on an offline unit) —
                # drop the term.  Only active when the caller
                # supplies ``pmax_by_gen``.
                if pmax_by_gen is not None:
                    _pmax = gen_pmax_by_name.get(gen_name, 0.0)
                    _profile = gen_pmax_profiles.get(gen_name)
                    if _pmax == 0.0 and (
                        _profile is None or not any(p > 0.0 for p in _profile)
                    ):
                        logger.debug(
                            "constraint %s: dropping reserve-zone "
                            "provision term for fully-offline gen '%s' "
                            "(pmax=0, no/all-zero profile; "
                            "[0,0]-bounded provision contributes 0)",
                            constr.name,
                            gen_name,
                        )
                        continue
                # Fix 6 (symmetric: zone-less provision drops here too)
                if unusable_provisions is not None and ref_name in unusable_provisions:
                    logger.debug(
                        "constraint %s: dropping reserve-zone term for "
                        "zone-less provision '%s' (no reserve_zones; "
                        "contributes 0)",
                        constr.name,
                        ref_name,
                    )
                    continue
                var_ref = f'reserve_provision("{ref_name}").{direction}'
                terms.append(_format_coefficient(alpha, first=not terms) + var_ref)
                coefficients.append(alpha)

        # 2b'. BESS-side Reserve.Provision expansion.
        #     PLEXOS BAT_*_CF_GEN_COMP / CF_LOAD_COMP constraints couple
        #     a battery's discharge/charge against its own SSCC reserve
        #     provision: ``α × Σ provision[bat, rsv] − Gen[bat] ≤ 0`` (or
        #     LOAD variant).  The Reserve×Constraint Provision Coefficient
        #     applies to (rsv, bat) pairs where BOTH the reserve and the
        #     battery are members of this constraint.  Without this loop
        #     the term silently dropped (the constraint emitted a
        #     degenerate ``-1 * battery(...).discharge = 0`` row, marked
        #     inactive) — leaving ~$48k of PLEXOS-binding reserve cost
        #     unenforced on CEN PCP 2026-04-22 (10 BAT_*_CF_*_COMP rows).
        #     The SSCC emitter names each BESS provision
        #     ``provision_<bat>_gen__<rsv>``; redirect via the AUX→main
        #     mapping (BAT_*_AUX → BAT_* / BAT_*_FV_AUX → BAT_*_FV) so
        #     constraint Battery memberships that reference the PLEXOS
        #     auxiliary variable still resolve to the emitted main name.
        bat_members = constraint_battery_members.get(constr.object_id, ())
        if bat_members:
            allowed_bat = (
                emitted_names.get("Battery") if emitted_names is not None else None
            )
            for rsv_name, alpha in reserve_provision_index.get(constr.object_id, ()):
                providers = reserve_to_battery_providers.get(rsv_name, ())
                if not providers:
                    continue
                direction = reserve_direction.get(
                    rsv_name, "dn" if rsv_name.upper().endswith("_LW_BESS") else "up"
                )
                for bat_name in bat_members:
                    if bat_name not in providers:
                        continue
                    # AUX→main redirect (mirrors the Generation/Load
                    # Coefficient logic above): if the AUX battery isn't
                    # in the emitted set, route the term to the main
                    # battery's SSCC provision instead.
                    effective_bat = bat_name
                    if (
                        allowed_bat is not None
                        and bat_name not in allowed_bat
                        and bat_name.endswith("_AUX")
                        and bat_name[:-4] in allowed_bat
                    ):
                        effective_bat = bat_name[:-4]
                    ref_name = f"provision_{effective_bat}_gen__{rsv_name}"
                    if allowed_rp is not None and ref_name not in allowed_rp:
                        logger.debug(
                            "constraint %s: dropping BESS reserve term "
                            "for unemitted provision '%s'",
                            constr.name,
                            ref_name,
                        )
                        continue
                    var_ref = f'reserve_provision("{ref_name}").{direction}'
                    terms.append(_format_coefficient(alpha, first=not terms) + var_ref)
                    coefficients.append(alpha)

        # 2c. Decision Variable → Definition (coll 711) — IMPLICIT +1
        #     coefficient on the DV.  See ``definition_dv_index`` build
        #     site above for the full rationale (PLEXOS definitional
        #     equations like ``MACHICURA_GENT4def``).  Without this
        #     loop the LHS collapses to ``-gen_U1 - gen_U2 = 0``
        #     instead of ``-gen_U1 - gen_U2 + DV = 0`` (where the DV
        #     absorbs the gen sum to make the equation hold).  When
        #     the DV is unemitted (rare; should not happen in
        #     practice because every Definition-DV corresponds to a
        #     gtopt ``decision_variable`` entry), log + skip.
        allowed_dv = (
            emitted_names.get("DecisionVariable") if emitted_names is not None else None
        )
        for dv_name in definition_dv_index.get(constr.object_id, ()):
            if allowed_dv is not None and dv_name not in allowed_dv:
                logger.debug(
                    "constraint %s: dropping Definition DV term for "
                    "unemitted decision_variable '%s'",
                    constr.name,
                    dv_name,
                )
                continue
            var_ref = f'decision_variable("{dv_name}").value'
            terms.append(_format_coefficient(1.0, first=not terms) + var_ref)
            coefficients.append(1.0)

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
                        _set_shift(
                            rhs_shift_per_block,
                            idx,
                            _shift_at(rhs_shift_per_block, idx) + alpha * cap_cf,
                        )
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
                    # synthesised ``uc_<battery>_gen`` Commitment.
                    # ``System::expand_batteries`` now creates this
                    # commitment UNCONDITIONALLY for every battery
                    # (relaxed continuous ``u ∈ [0, 1]`` when the
                    # battery has no commitment economics — see
                    # ``source/system.cpp``), so the reference always
                    # resolves at LP build.  Mirrors PLEXOS, which
                    # synthesises an internal commitment binary from
                    # the battery's ``Units`` property for every
                    # battery.
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
                    #
                    # Fix 5 (offline gen): if the gen has ``pmax = 0``
                    # AND no/all-zero profile, BOTH the current and
                    # prior generation columns are unmaterialised in
                    # gtopt's LP; PLEXOS resolves them both to 0.
                    # Skip both terms (``α × (0 − 0) = 0``) — matches
                    # PLEXOS and the direct-coefficient + fuel-offtake
                    # skips above.  Only active when the caller
                    # supplies ``pmax_by_gen``.
                    if pmax_by_gen is not None:
                        _pmax = gen_pmax_by_name.get(parent_name, 0.0)
                        _profile = gen_pmax_profiles.get(parent_name)
                        if _pmax == 0.0 and (
                            _profile is None or not any(p > 0.0 for p in _profile)
                        ):
                            logger.debug(
                                "constraint %s: dropping ramp_delta terms "
                                "for fully-offline generator '%s' "
                                "(pmax=0, no/all-zero profile)",
                                constr.name,
                                parent_name,
                            )
                            continue
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

        # Fix 2: an unresolvable direct-coefficient term (element never
        # emitted, BESS Fix could not repair it) was already RECORDED in
        # ``unresolved_refs`` above and skipped from ``terms``.  We do
        # NOT drop the whole constraint here: the collected references
        # trigger ONE hard-fail error after the build loop (see below).
        # Continue assembling this constraint normally so the error path
        # can still surface its surviving terms in context if needed.
        if not terms:
            # Fix 6: no-op-constraint rescue.  When every LHS term was
            # silently dropped because its physical contribution is
            # provably zero at this run date (see ``silent_zero_drops``
            # for the three drop categories: shadow Lines, fully-
            # offline Generators, Fuel-offtake with no consuming gens),
            # the constraint reduces to ``0 <op> rhs`` and is
            # mathematically a no-op.  Emit a trivial inactive stub
            # (``0 <op> 0``) so the constraint name shows up in the
            # bundle, the PLEXOS-sol → gtopt audit lines up (no
            # spurious "missing"), and a follow-up run where the
            # zeroed elements come online can simply flip
            # ``active=True`` without re-emitting.
            if silent_zero_drops > 0 and sense_val is not None:
                stub_op = _SENSE_OP.get(sense_val, "<=")
                stub_expr = f"0 {stub_op} 0"
                out.append(
                    UserConstraintSpec(
                        name=constr.name,
                        expression=stub_expr,
                        penalty=0.0,
                        active=False,
                        description=(
                            f"PLEXOS Constraint '{constr.name}': all "
                            f"{silent_zero_drops} LHS term(s) provably "
                            "contribute 0 at this run date (shadow Lines "
                            "with Lin_Units=0, fully-offline Generators, "
                            "or Fuel.offtake terms with no consuming "
                            "generators).  Emitted as an inactive stub to "
                            "preserve provenance; a follow-up run where "
                            "the zeroed elements come online can flip "
                            "active=True and restore the real LHS.  "
                            f"(File: {_uc_source_file})"
                        ),
                    )
                )
                continue
            lhs_dropped += 1
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
            lhs_dropped += 1
            continue
        # ── Daily-ENERGY budget classification (PLEXOS ``RHS Day`` /
        #    ``Hydro_MaxRampDay.csv``) ─────────────────────────────────────
        # A constraint is a per-day ENERGY budget when its RHS comes from
        # ``RHS Day`` (DB property) OR from the ramp-day overlay AND its LHS
        # references ``generator(...).generation`` (the only physical-energy
        # accessor).  These map onto gtopt's ``daily_sum`` +
        # ``constraint_type=energy`` row: ``Σ_day gen·Δt ≤ RHS`` [MWh].
        # ``RHS Day`` ships in GWh, so the effective LP RHS is ``× 1000``.
        # Scale the scalar ``rhs_val`` (inline ``<op> NUMBER`` tail) here and
        # the per-block ``rhs_profile_tuple`` further below — exactly once.
        references_generation = any(".generation" in t for t in terms)
        # Fuel-offtake daily caps (``Diesel_OffTakeDay``, ``Gas_*``) expand to
        # ``heat_rate · generator.generation`` terms, so they spuriously pass
        # ``references_generation`` here — but they take their OWN ×1000 GWh→MWh
        # scale inside the ``is_fuel_offtake`` per-block split above (see the
        # ``rhs_val *= _DAILY_ENERGY_RHS_SCALE`` block).  Excluding them from
        # ``is_daily_energy`` prevents a double-application: if both branches
        # fired, Diesel_OffTakeDay would inflate to 9.27e6 instead of the
        # correct 9277 (= PLEXOS 386.54/h × 24).  Keep the exclusion strictly
        # for the ×1000 placement, NOT to skip the scale entirely.
        is_daily_energy = (
            (rhs_from_day or ramp_day_present)
            and references_generation
            and not is_fuel_offtake
        )
        # Crew / commitment-start-count daily caps (e.g. ``Guacolda_Crew``:
        # Σ_day (startup+shutdown) ≤ 2) carry ``RHS Day`` as a COUNT, not a
        # GWh energy budget.  They DO get gtopt's ``daily_sum`` (so the cap
        # binds per DAY — matching PLEXOS spreading the daily 2 to 2/24 = 0.083
        # per hour) but with ``constraint_type=""`` (unweighted per-day count,
        # NOT Δt-weighted energy) and WITHOUT the ×1000 GWh→MWh scale.
        references_commit_count = any(
            (".startup" in t or ".shutdown" in t) for t in terms
        )
        is_daily_count = (
            (rhs_from_day or ramp_day_present)
            and references_commit_count
            and not references_generation
            and not is_fuel_offtake
        )
        # Daily-RHS constraints with neither a generation-energy nor a
        # commitment-count LHS (pure fuel / other) stay DEFERRED — different
        # units/semantics.
        if (
            (rhs_from_day or ramp_day_present)
            and not references_generation
            and not is_daily_count
        ):
            logger.debug(
                "constraint %s carries a daily RHS but no generation / "
                "commit-count LHS (fuel/other) — daily_sum mapping deferred "
                "(different units/semantics)",
                constr.name,
            )
        if is_daily_energy:
            rhs_val *= _DAILY_ENERGY_RHS_SCALE
        # Render the OPERATIVE per-day bound in the expression tail for
        # daily-energy ramp rows.  Their base ``RHS`` property is often a
        # 0/sentinel (the live cap ships in ``RHS Day`` / the ramp-day
        # overlay applied to ``rhs_profile`` below), so the inline tail would
        # otherwise read a misleading ``<op> 0`` — looking like a forced
        # shutdown when the LP actually enforces ``Σ_day gen ≤ <day cap>``.
        # Purely cosmetic: ``rhs_profile`` (set from ``ramp_day_rhs`` further
        # down) is what reaches the LP row; this only fixes the displayed
        # expression so it matches the assembled constraint.
        _expr_rhs = rhs_val
        if is_daily_energy:
            _day_rhs_tail = ramp_day_rhs.get(constr.name)
            if _day_rhs_tail:
                # ``RHS Day`` ships in GWh; the LP row is MWh — match the
                # ×1000 daily-energy scale so the tail equals the LP RHS.
                _expr_rhs = _day_rhs_tail[0] * _DAILY_ENERGY_RHS_SCALE
        expression = "".join(terms) + f" {op} {_expr_rhs:g}"
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

        # PLEXOS ``Include in ST Schedule == 0`` is honoured DIRECTLY: such
        # constraints are emitted ``active=False`` (disabled), matching
        # PLEXOS's own exclusion.  A prior ``_FORCE_ACTIVE_PATTERNS`` override
        # that kept Conf/Commit/CTFOFF/*_OFF/*_Startings/*_Comparison families
        # active *despite* flag==0 was removed — it diverged from PLEXOS by
        # enforcing 25 rows PLEXOS had disabled (gtopt potentially tighter than
        # PLEXOS).  NB: config-exclusivity rows PLEXOS keeps active carry
        # flag!=0 and so are unaffected — they stay active via this same flag.

        # Battery-disable UCs (``Almacenamiento_BAT_*: battery.energy
        # = 0``) are PLEXOS's internal way of pinning a battery's SoC
        # to zero across the horizon (= unit effectively offline).
        # gtopt already represents battery on/off via ``Battery.emax``
        # / ``eini`` / ``efin`` / ``commitment``; the EQ pin
        # collides with gtopt's battery dynamics (initial SoC > 0
        # forces a discharge path the energy-=0 row forbids), and
        # even soft-penalty slack can't resolve a multi-block
        # accumulating violation cleanly.  Mark these inactive so
        # they don't conflict.
        is_battery_disable = constr.name.startswith(
            "Almacenamiento_BAT_"
        ) or constr.name.startswith("Almacenamiento_NoBAT_")
        # GEN_BAT_/LOAD_BAT_ "battery shutoff" modeling artifact: PLEXOS
        # ships 35 GEN_BAT_<name> + 35 LOAD_BAT_<name> source UCs whose
        # LHS is a single Battery term (Generation Coefficient = 1 or
        # Load Coefficient = 1) with Sense=None (default equality) and
        # RHS=0 — i.e. ``battery.discharge = 0`` / ``battery.charge =
        # 0``.  Taken literally these would force the battery entirely
        # off, but PLEXOS itself DROPS THE WHOLE FAMILY from the ST
        # schedule (verified against RES20260422 solution: none of the
        # 70 source UCs appear in t_object, while the batteries
        # themselves dispatch — e.g. BAT_VICTOR_JARA_FV charges 8.9 GWh
        # and discharges 9.3 GWh).  Previously gtopt emitted these as
        # SOFT equalities at $10/MWh penalty, burning $182K of soft
        # slack on BAT_VICTOR_JARA_FV alone ($91,919 LOAD + $90,090
        # GEN) and corresponding amounts on the other 33 batteries.
        # Detect the exact pattern at the *LHS* level (single term,
        # ``battery(...).charge`` or ``battery(...).discharge``,
        # coef=1, RHS=0, sense=None) and emit inactive to mirror
        # PLEXOS's effective behaviour.
        # ``sense_val`` was defaulted to 0.0 (equality) earlier in this
        # function when the source XML carried no Sense — so check for
        # the equality default here, not ``None``.
        is_battery_shutoff_artifact = (
            sense_val == 0.0
            and rhs_val == 0.0
            and len(terms) == 1
            and "battery(" in terms[0]
            and (".charge" in terms[0] or ".discharge" in terms[0])
        )
        # Same modeling-artifact pattern but on the generator side —
        # ``PEHUENCHE_GENT7def`` ships TWO Generation Coefficient
        # memberships (PEHUENCHE_U1 + U2, both coef=-1) with Sense=None
        # (default equality) and RHS=0.  PLEXOS DROPS the constraint
        # from the ST schedule (verified against RES20260422 solution:
        # not in t_object), letting both PEHUENCHE units dispatch
        # freely.  Until this fix gtopt's silent-zero-drop pruned U1
        # (offline at pmax=0) and emitted ``-PEHUENCHE_U2.generation
        # = 0`` as a SOFT equality, forcing U2 to dispatch zero — that
        # constraint is what drove the −11,576 MWh / −68.8% PEHUENCHE_U2
        # under-dispatch versus PLEXOS (16,822 vs 5,246 MWh).
        # Emit inactive when the surviving LHS is a single
        # ``generator(...).generation`` term with coef=1 (i.e. the
        # post-drop residual literally says "this gen must equal 0").
        is_generator_shutoff_artifact = (
            sense_val == 0.0
            and rhs_val == 0.0
            and len(terms) == 1
            and "generator(" in terms[0]
            and ".generation" in terms[0]
        )
        is_inactive = (
            is_excluded_by_plexos
            or is_structurally_infeasible
            or is_battery_disable
            or is_battery_shutoff_artifact
            or is_generator_shutoff_artifact
        )
        # PLEXOS-HARD-list override: when the constraint name is in the
        # data-driven ``cen_pcp_hard_ucs.txt`` list (audited from the
        # solution .accdb as HARD with positive shadow), it MUST be
        # active — PLEXOS itself enforces it.  Most commonly this fires
        # for CTFOFF_* rows that the scenario-tag resolver wrongly
        # marked inactive because the dual-row Include in ST Schedule
        # picked the untagged "0" instead of the CTFOffline_ON-tagged
        # "-1" (the ON scenario IS active in PRGdia_Full_Definitivo).
        if is_inactive and constr.name in _load_plexos_hard_uc_list():
            logger.info(
                "constraint %s in PLEXOS-HARD list — overriding "
                "is_inactive=False so the LP enforces what PLEXOS enforces",
                constr.name,
            )
            is_inactive = False
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
        # Hydro daily-ramp RHS overlay: expand the per-day limit across
        # that day's 24 hours so the writer aggregates it to per-block
        # ``user_constraint.rhs``.  Overrides any scalar/shift RHS for the
        # named ramp constraints (e.g. ``RALCOramp_max_e1``).
        day_rhs = ramp_day_rhs.get(constr.name)
        if day_rhs:
            rhs_profile_tuple = tuple(v for v in day_rhs for _ in range(24))
        # PLEXOS timeslice-tagged RHS rows (``H16-20``, ``W2-6,H8-21``):
        # apply LAST since they encode the live PLEXOS evaluation
        # (verified against the RES20260422 solution-RHS pid=3073 for
        # ``Campiche_starting``, ``Commit_*``, ``IL_2024000947_*``).
        # Wins over the curtailment-shift and ramp-day overlays
        # because the tag mechanism is what PLEXOS itself applies last
        # at solver setup.  Skip when both no tags fired and no other
        # overlay is present (preserves the scalar emit path).
        if rhs_timeslice_profile:
            rhs_profile_tuple = rhs_timeslice_profile
        elif rhs_date_overlay_profile:
            # PLEXOS partial-horizon date-window overlay (e.g. the four
            # SD_2026030813 / SD_2026036857 contingency rows with
            # RHS=1600/896 active 4-6 blocks of the week — $4.15M of
            # PLEXOS solver slack proves they bind).  Applied only when
            # no timeslice overlay won (timeslice has higher precedence
            # because PLEXOS evaluates tags after date windows).
            rhs_profile_tuple = rhs_date_overlay_profile
        else:
            # PLEXOS reserve-requirement aggregation: when a UC is a
            # ``CSF_*MinProvision`` / ``CPF_*MinProvision`` family row
            # (Σ provision over Reserves = static MinProvision), PLEXOS
            # evaluates the effective per-block RHS as
            # ``max(static_RHS, Σ_reserve coef × Reserve.requirement[t])``
            # using the per-hour profiles parsed from
            # ``Res_Requirement.csv`` (verified on RES20260422: CSF_RS
            # TR_2 day-type carries hour profiles 154,154,158×5,215×3,
            # 209×6,330×3,301×3,154×2 which match PLEXOS's pid-3073
            # solver-applied RHS for CSF_UpMinProvision exactly).
            # Without this, gtopt emits a flat RHS=130 floor that the
            # LP satisfies cheaply, while PLEXOS forces 215..330 MW of
            # reserve provision during peak hours — driving ~$268K of
            # gtopt soft slack on CSF_Up/Down MinProvision.
            rsv_terms = reserve_provision_index.get(constr.object_id) or ()
            if rsv_terms and reserves and rhs_val is not None:
                reserves_by_name = {r.name: r for r in reserves}
                # Build per-block effective requirement = Σ_rsv coef × rsv.profile
                # Direction: the constraint's accessor side (the LHS
                # was emitted with ``.up`` or ``.dn``) selects ur vs dr
                # requirement.  ``reserve_direction`` map above already
                # encodes this per reserve.
                n_blocks = _horizon_days * 24
                rsv_sum = [0.0] * n_blocks
                any_profile = False
                for rsv_name, coef in rsv_terms:
                    rsv = reserves_by_name.get(rsv_name)
                    if rsv is None:
                        continue
                    direction = reserve_direction.get(rsv_name)
                    profile = (
                        rsv.ur_requirement
                        if direction == "up"
                        else rsv.dr_requirement
                        if direction == "dn"
                        else rsv.ur_requirement or rsv.dr_requirement
                    )
                    if not profile:
                        continue
                    any_profile = True
                    n = min(len(profile), n_blocks)
                    for i in range(n):
                        rsv_sum[i] += coef * profile[i]
                if any_profile:
                    # RHS per block = the time-varying reserve-requirement
                    # aggregation (Σ_rsv coef × Reserve.requirement[t]) — this
                    # mirrors PLEXOS's reported, VARIABLE constraint RHS (e.g.
                    # CTF_DownMinProvision 94..633).  The static ``rhs_val``
                    # (the constraint's nominal/Min-Provision floor) is NOT
                    # folded in here: that floor is enforced natively via
                    # ``ReserveZone.urmin/drmin`` (``Σ pf·prov ≥ max(req,
                    # min)``), so the reported RHS is not raised to a FIXED
                    # floor on low-requirement hours (which over-reported the
                    # RHS and made it look fixed where PLEXOS is variable).
                    rhs_profile_tuple = tuple(rsv_sum[i] for i in range(n_blocks))
        # GWh→MWh scale for daily-ENERGY budgets.  The scalar ``rhs_val`` was
        # already scaled above (it feeds the inline ``<op> NUMBER`` tail); the
        # per-block ``rhs_profile_tuple`` here carries the RAW ``RHS Day`` /
        # ramp-day GWh values (the ``day_rhs`` overlay re-reads the un-scaled
        # CSV), so scale it the same ×1000 — exactly once, only for the
        # daily-energy rows.  (The curtailment-shift branch above already used
        # the scaled scalar; those constraints are never daily-energy.)
        if is_daily_energy and rhs_profile_tuple:
            rhs_profile_tuple = tuple(
                v * _DAILY_ENERGY_RHS_SCALE for v in rhs_profile_tuple
            )
        # Soft-tier classification.  The legacy regex+penalty ladder used to
        # live here as a ~330-line if/elif tree mixing _HYDRO_UC_SOFT_PENALTY
        # / _RESERVE_PROVISION_SUM_PENALTY with hard-coded name patterns.  It
        # is now centralised in ``_uc_policy.classify`` so the penalty values
        # live in a single auditable place and the typed
        # :class:`ConstraintDirective` is the wire-form authority.  See
        # ``_uc_policy.py`` for the per-branch rationale that used to be
        # inline here.
        plexos_penalty = penalty_val if penalty_val and penalty_val > 0 else 0.0
        _outcome = _uc_policy.classify(
            constraint_name=constr.name,
            expression=expression,
            op=op,
            plexos_penalty=plexos_penalty,
            is_inactive=is_inactive,
            hard_set=_load_plexos_hard_uc_list(),
        )
        emitted_penalty = _outcome.penalty
        directive_to_emit: ConstraintDirective | None = _outcome.directive
        # No-limit-sentinel line-security constraints (SD_* etc.) — a
        # PURE line-flow constraint at the 100000 "contingency off" sentinel
        # is inert.  Emit as an inactive stub instead of silently dropping,
        # so the PLEXOS-sol → gtopt audit lines them up (PLEXOS exercises
        # them in its solution DB even though they're no-ops today).  See
        # ``_is_nolimit_line_sentinel`` for the detection logic.  When the
        # contingency flips active in a future run, plexos2gtopt re-reads
        # the t_data and an effective RHS below the sentinel emits the
        # real constraint.
        if (
            _is_nolimit_line_sentinel(expression, rhs_val)
            and constr.name not in _load_plexos_hard_uc_list()
        ):
            # Skip the no-op stub when this constraint is in the
            # PLEXOS-HARD audit list — even with the contingency-off
            # sentinel RHS, the PLEXOS sol shows |Shadow|>0 / HrsBind>0
            # so we must emit the real constraint (active, hard) and
            # let CPLEX bind it.  The 8 CTFOFF_* contingency-flow
            # constraints reach this branch and were being silently
            # demoted to ``0 ≤ 0 (inactive)`` before this guard fired.
            sd_sentinel_dropped += 1
            logger.debug(
                "emitting no-limit line-security constraint %s as inactive "
                "stub (RHS=%g >= %g sentinel; PLEXOS contingency inactive today)",
                constr.name,
                rhs_val,
                _SD_NOLIMIT_RHS_SENTINEL,
            )
            stub_op = _SENSE_OP.get(sense_val, "<=")
            out.append(
                UserConstraintSpec(
                    name=constr.name,
                    expression=f"0 {stub_op} 0",
                    penalty=0.0,
                    active=False,
                    description=(
                        f"PLEXOS Constraint '{constr.name}': pure-line-flow "
                        f"constraint with RHS={rhs_val:g} ≥ "
                        f"{_SD_NOLIMIT_RHS_SENTINEL:g} sentinel (contingency "
                        "inactive at this run date).  Inactive stub keeps "
                        "the LP well-conditioned while preserving the "
                        "constraint name for audit / follow-up activation.  "
                        f"(File: {_uc_source_file})"
                    ),
                )
            )
            continue
        # Tautology detection: when a single-term row reduces to
        # ``±c · binary <op> rhs`` and the LP-natural domain ``[0, 1]``
        # already satisfies it, the row is LP-redundant.  Bundle audit
        # surfaced 14 such rows (``ATA_TG1B_DIE_SSCC``,
        # ``SSCC_NVentanas_{Up,Down}``, ``COLMITO_Uniq``, …) where
        # PLEXOS itself reports ``hours_binding = 0`` and ``price = 0``
        # — they sit in the model as audit guards, never bind.  Downgrade
        # to ``active=False`` so CPLEX presolve doesn't bother allocating
        # row state.
        is_tautology = _is_tautology_single_term(terms, op, rhs_val)
        out.append(
            UserConstraintSpec(
                name=constr.name,
                expression=expression,
                penalty=emitted_penalty,
                active=False if (is_inactive or is_tautology) else None,
                rhs_profile=rhs_profile_tuple,
                daily_sum=is_daily_energy or is_daily_count,
                constraint_type="energy" if is_daily_energy else "",
                directive=directive_to_emit,
                description=_describe_user_constraint(
                    constr.name,
                    expression,
                    op,
                    rhs_val,
                    source_file=_uc_source_file,
                    fuel_offtake=is_fuel_offtake,
                    from_rhs_custom=rhs_from_custom,
                    inactive=is_inactive,
                    n_terms=len(terms),
                    n_filtered=silent_zero_drops,
                    tautology=is_tautology,
                ),
            )
        )
    if stats_out is not None:
        stats_out["raw_total"] = len(constraints)
        stats_out["lhs_dropped"] = lhs_dropped
        stats_out["sd_sentinel_dropped"] = sd_sentinel_dropped
        stats_out["unresolved_refs"] = len(unresolved_refs)
        stats_out["emitted_base"] = len(out)
    # FAIL HARD on any unresolvable UserConstraint reference.  We held
    # off until every constraint was walked so the user sees the FULL
    # list in one error (not just the first).  This mirrors gtopt's
    # strict JSON parser: an unknown element name is a fatal error, not
    # a silent drop.  Each line carries the closest emitted name as a
    # hint (e.g. ``provision_X`` → ``provision_X_gen__CSF_LW_BESS``).
    if unresolved_refs:
        lines: list[str] = []
        for constr_name, ref_expr, gtopt_key in unresolved_refs:
            hint = ""
            # Extract the bad element name from ``class("name").attr`` to
            # suggest the nearest emitted name within the same class.
            name_match = re.search(r'\("([^"]*)"\)', ref_expr)
            bad_name = name_match.group(1) if name_match else ""
            candidates = (
                sorted(emitted_names.get(gtopt_key, frozenset()))
                if emitted_names is not None and gtopt_key is not None
                else []
            )
            if bad_name and candidates:
                close = difflib.get_close_matches(bad_name, candidates, n=1, cutoff=0.4)
                if close:
                    hint = f"  (closest emitted name: {close[0]!r})"
            lines.append(f"  - constraint {constr_name!r}: {ref_expr}{hint}")
        msg = (
            f"{len(unresolved_refs)} UserConstraint term(s) reference "
            "element name(s) that gtopt never emits — refusing to write a "
            "bundle with dangling references.  Fix the source data or the "
            "name mapping; do NOT silently drop these terms:\n" + "\n".join(lines)
        )
        if lax_refs:
            # Lax mode (``--lax-uc-refs``): downgrade the fail-hard to a
            # warning + per-term silent drop.  Used for debugging /
            # iterative parser work where one wants the JSON to land
            # despite known dangling refs (the gtopt strict-load step
            # can then be run with ``--constraint-mode debug``).  In
            # this mode the offending terms have ALREADY been collected
            # but NOT injected into the output expressions, so the
            # resulting JSON is internally consistent (no dangling
            # symbols) — the bundle just LOSES coverage on those
            # constraints rather than failing the conversion.
            logger.warning(
                "%d UserConstraint term(s) reference unemitted elements; "
                "lax_refs=True → terms silently dropped, constraint may be "
                "weakened.  Re-run without --lax-uc-refs to see the full "
                "list and fix the source data.",
                len(unresolved_refs),
            )
            if stats_out is not None:
                stats_out["lax_unresolved_dropped"] = len(unresolved_refs)
        else:
            raise UnresolvedConstraintReferenceError(msg)
    # Post-process: consolidate Gas_MaxOpDay**X**_<group> per-block specs
    # into per-(fuel,owner) daily_sum + energy UCs with per-day RHS profiles.
    # See ``_consolidate_gas_maxopday_groups`` for the rationale.
    block_layout_local = getattr(_bundle, "block_layout", ()) or ()
    out = _consolidate_gas_maxopday_groups(
        out,
        block_layout=block_layout_local,
        horizon_hours=horizon_hours,
        soft_penalty=_RESERVE_PROVISION_SUM_PENALTY,
    )
    return tuple(out)


#: Soft-violation penalty ($/fuel-unit) for synthesised ``FueMaxOff_*``
#: caps.  PLEXOS treats these fuel-offtake limits as soft and exceeds
#: them substantially in the solved model; matching that (vs the old
#: near-hard 10000) keeps gtopt from throttling LNG and over-running on
#: coal.  Same magnitude as the other soft-UC penalties (see
#: ``_HYDRO_UC_SOFT_PENALTY``).
_FUEL_OFFTAKE_SOFT_PENALTY = 1000.0


def _apply_native_fuel_offtake_caps(
    bundle: PlexosBundle, fuels: tuple[FuelSpec, ...]
) -> tuple[FuelSpec, ...]:
    """Set native ``Fuel.max_offtake`` (+ soft ``max_offtake_cost``) from
    the PLEXOS ``FueMaxOffWeek_*`` weekly caps.

    Replaces the ``FueMaxOff_*`` UserConstraint approximation
    (``Σ heat_rate·gen ≤ cap`` per block) with gtopt's native per-stage
    fuel-budget row (``Σ heat_rate·gen·duration ≤ max_offtake`` over the
    week).  The model has one stage = the week, so each fuel's weekly
    cap maps directly to ``max_offtake``.  The cap is SOFT
    (``max_offtake_cost`` = the standard soft penalty) because PLEXOS
    violates these caps — a hard budget re-throttles LNG and forces coal.

    Returns the fuels tuple with caps applied; unmatched fuels pass
    through unchanged.
    """
    if bundle.accdb_cache_dir is None or not bundle.accdb_cache_dir.is_dir():
        return fuels
    from .plexos_block_layout import extract_fuel_offtake_caps

    caps = extract_fuel_offtake_caps(bundle.accdb_cache_dir)
    if not caps:
        return fuels
    # Drop never-binding sentinels (cap ≥ 1e15 = "no real limit").
    binding = {f: cap for f, (cap, _scope) in caps.items() if cap < 1e15}
    if not binding:
        return fuels
    return tuple(
        dataclasses.replace(
            fuel,
            max_offtake=binding[fuel.name],
            max_offtake_cost=_FUEL_OFFTAKE_SOFT_PENALTY,
        )
        if fuel.name in binding
        else fuel
        for fuel in fuels
    )


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
        # KEEP generators whose per-block pmax profile drops to 0 in
        # SOME blocks (PLEXOS startup-staged / alternate-fuel-mode
        # variants).  The earlier code skipped them to avoid gtopt's
        # strict-mode UC resolver throwing "missing or inactive" on
        # zero-pmax blocks — but that under-counted the FueMaxOff
        # cap LHS by entire generators, producing a +229% LNG over-
        # dispatch on CEN PCP weekly (NEHUENCO_2-TG+TV_GNL_C alone
        # dispatched 30,843 MWh vs PLEXOS 244 MWh).
        #
        # gtopt's resolver was made lenient on the specific case
        # "element registered for SOME block of the stage but not
        # this one" (element_known=true via `find_ampl_cols` scan
        # over the stage's blocks in `element_column_resolver.cpp`).
        # Genuine typos / unregistered attributes still throw — the
        # safety guard is preserved.
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

        # PLEXOS publishes a per-period RHS profile that varies
        # substantially across the horizon — e.g.
        # ``FueMaxOffWeek_Gas_Yungay_GN_A`` runs at 14.5 in the
        # first 25 blocks (where YUNGAY units are FixedLoad-pinned
        # at 52 MW × 0.284 heat ≈ 14.76 per block) and drops to
        # 0.49 in the cheap-gen surplus hours.  A uniform
        # decomposition (``cap × d / scope``) flattens this to 2.32
        # per block, which collides with the FixedLoad equality and
        # makes the LP primal-infeasible on block 1.  Use PLEXOS's
        # actual per-period RHS values where they're available;
        # fall back to the uniform decomposition otherwise.
        per_period = (
            extract_fuel_offtake_caps.rhs_per_period.get(fuel_name, {})
            if hasattr(extract_fuel_offtake_caps, "rhs_per_period")
            else {}
        )
        if per_period and block_durations:
            # ``per_period`` maps PLEXOS period_id (1-indexed) to
            # the per-period RHS.  block_durations is 0-indexed and
            # aligns 1:1 with PLEXOS blocks under
            # ``--horizon-mode plexos``.  Build the profile from
            # period_id = i + 1; fall back to the uniform formula
            # at any block where PLEXOS didn't publish a row.
            rhs_profile = tuple(
                per_period.get(i + 1, cap * (block_durations[i] / scope_hours))
                for i in range(len(block_durations))
            )
            scalar_rhs = sum(rhs_profile) / max(len(rhs_profile), 1)
        elif block_durations:
            rhs_profile = tuple(cap * (d / scope_hours) for d in block_durations)
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
        # PLEXOS treats FueMaxOff_* caps as genuinely SOFT constraints
        # and violates them substantially: the solved model dispatches
        # gas/LNG well above these per-period fuel-offtake limits (the
        # RES20260422 analysis found over-runs up to ~175 fuel-units,
        # not the tiny ~0.25 originally assumed).  Emitting them with a
        # near-hard penalty (the old 10000) made gtopt RESPECT a cap
        # PLEXOS ignores, throttling the central/north LNG combined-
        # cycles (NUEVA_RENCA, NEHUENCO, SAN_ISIDRO, QUINTERO, ATA) to
        # roughly half their PLEXOS dispatch and forcing ~100 GWh of
        # coal to fill the gap.  A controlled experiment (penalty
        # 10000 → 10) swung 100 GWh from coal back to gas, snapping the
        # fuel mix onto PLEXOS (coal 211 vs 218 GWh, gas 221 vs 227).
        # Use the standard soft-UC penalty so the cap behaves as the
        # economic "buy incremental LNG" signal PLEXOS models, not a
        # hard availability wall.
        out.append(
            UserConstraintSpec(
                name=f"FueMaxOff_{fuel_name}",
                expression=expression,
                rhs_profile=rhs_profile,
                penalty=_FUEL_OFFTAKE_SOFT_PENALTY,
                description=(
                    f"PLEXOS weekly fuel-offtake cap 'FueMaxOff_{fuel_name}': "
                    "Σ heat_rate·generation [fuel-energy/h] ≤ max_offtake — "
                    "weekly LNG/gas budget per band (per-block uniform split), "
                    "soft-priced as the 'buy incremental fuel' signal "
                    "(File: Fuel_MaxOfftakeWeek.csv)"
                ),
            )
        )
    if out:
        logger.info(
            "synthesised %d FueMaxOff* UserConstraint(s) from solution .accdb "
            "(weekly/daily fuel-offtake caps; per-block uniform decomposition)",
            len(out),
        )
    return tuple(out)


# PLEXOS encodes Combined-Cycle / multi-fuel-band plants by emitting
# one Generator object per *configuration* — e.g. SAN_ISIDRO_2-TG+TV
# ships 13 variants named with the fuel-band suffix:
#
#   _DIE                     (diesel backup)
#   _GNL_{A,B,C,D,E,F,INF}   (LNG, one per offer band)
#   _GN_{A,B,C,D,E}          (pipeline gas, one per offer band)
#
# In PLEXOS the underlying physical plant only has ONE pmax — the LP
# can run at most one configuration at a time.  PLEXOS itself enforces
# this either:
#   1. via the GUI "Composite Generator" property (not exposed as XML
#      data in CEN PCP — the ``Power Station`` class is empty here),
#   2. via fictitious-proxy commitments + ``<plant>_ConfTG`` UCs
#      (only present for ATA_CC, KELAR, NEHUENCO_1 in CEN PCP), or
#   3. implicitly via the ``FueMaxOff_Gas_<band>`` per-fuel caps
#      (which only bind on the band, not on the plant total).
#
# Without an explicit plant-cap constraint, gtopt's LP dispatches
# every active variant up to its individual pmax simultaneously,
# producing the "SAN_ISIDRO_2 ≈ 85 GWh" arbitrage (well above the
# 391-MW × 168 h ≈ 65-GWh single-config envelope) observed in the
# 2026-05-24 K=6 uniform run.
#
# Fix: detect plant families by stripping the fuel-band suffix and
# emit one ``PlantCap_<stem>`` UserConstraint that caps the SUM of
# variant generation by the family's maximum single-config pmax.
# Conservative for the rare case where two configs coexist (gas +
# diesel start-up) — for those, the ``FueMaxOff_*`` caps already
# bind on each band and provide the same envelope.  Soft-priced so
# any model-data inconsistency (e.g. simultaneous Fixed-Load on two
# variants) doesn't make the LP infeasible.
_PLANT_FAMILY_SUFFIX_PATTERNS: tuple[str, ...] = (
    # Order matters: try the most specific patterns first.
    r"_GNL_INF$",
    r"_GNL_[A-Z]$",
    r"_GN_[A-Z]$",
    r"_GLP$",
    r"_GNL_P$",
    r"_DIE$",
)


def _strip_plant_config_suffix(name: str) -> str | None:
    """Return the family stem of ``name`` if it ends in a known fuel-band
    suffix; ``None`` otherwise.

    Used by :func:`_detect_plant_families` to group multi-fuel-band
    variants of the same physical plant — the LP must cap their
    summed dispatch at the single-config pmax envelope.
    """

    for pat in _PLANT_FAMILY_SUFFIX_PATTERNS:
        stem = re.sub(pat, "", name)
        if stem != name:
            return stem
    return None


def _detect_plant_families(
    generators: tuple[GeneratorSpec, ...],
) -> dict[str, list[GeneratorSpec]]:
    """Group generators by stem (= name minus fuel-band suffix).

    Only returns stems with ≥ 2 variants and ≥ 2 ACTIVE variants
    (``pmax > 0`` or any positive entry in ``pmax_profile``) — a
    family with only one active variant has no LP arbitrage and
    doesn't need a cap UC.
    """
    families: dict[str, list[GeneratorSpec]] = {}
    for g in generators:
        stem = _strip_plant_config_suffix(g.name)
        if stem is None:
            continue
        families.setdefault(stem, []).append(g)

    def _is_active(g: GeneratorSpec) -> bool:
        if g.pmax_profile and any(v > 0.0 for v in g.pmax_profile):
            return True
        return (g.pmax or 0.0) > 0.0

    return {
        stem: variants
        for stem, variants in families.items()
        if len(variants) >= 2 and sum(1 for v in variants if _is_active(v)) >= 2
    }


def _extract_config_mutex_groups(db: PlexosDb) -> list[tuple[str, frozenset[str]]]:
    """Mutually-exclusive generation-variant groups from PLEXOS ``*_Uniq``
    Constraint objects (config exclusivity, F1).

    Each ``<plant>_Uniq`` Constraint lists EVERY (configuration ×
    fuel-band) generation variant of one physical combined-cycle plant
    (e.g. ``SANISIDRO_Uniq`` → all 39 SAN_ISIDRO variants).  They are
    mutually exclusive — sharing the same physical turbines — so at most
    one configuration's worth of capacity can run at a time.

    This is the DATA-DRIVEN source for the config-exclusivity cap: a
    name-heuristic that merges by plant stem would wrongly merge
    genuinely-parallel units (e.g. ``HUASCO`` U3/U4/U5, which carry NO
    ``_Uniq``).  Returns ``[(uniq_name, frozenset(member_gen_names)), …]``
    for groups with ≥ 2 members.
    """
    constraints = db.objects_of_class("Constraint")
    uniq = {c.object_id: c.name for c in constraints if c.name.endswith("_Uniq")}
    if not uniq:
        return []
    gen_coll = db.collection_for_named("Generator", "Constraint", "Constraints")
    if gen_coll is None:
        return []
    objs = db.object_by_id()
    constr_to_gens: dict[int, list[str]] = {}
    for gen_oid, constr_oids in db.parent_to_children(gen_coll.collection_id).items():
        gen = objs.get(gen_oid)
        if gen is None:
            continue
        for coid in constr_oids:
            if coid in uniq:
                constr_to_gens.setdefault(coid, []).append(gen.name)
    groups: list[tuple[str, frozenset[str]]] = []
    for coid, name in sorted(uniq.items(), key=lambda kv: kv[1]):
        gens = constr_to_gens.get(coid, [])
        if len(gens) >= 2:
            groups.append((name, frozenset(gens)))
    return groups


def _build_plant_cap_ucs(
    case: PlexosCase,
    mutex_groups: tuple[tuple[str, frozenset[str]], ...] = (),
) -> tuple[UserConstraintSpec, ...]:
    """Synthesise ``PlantCap_<stem>`` + ``PlantCommit_<stem>`` UCs
    capping a plant's summed variant generation AND commitment status
    at the single-config envelope.

    For each PLEXOS ``*_Uniq`` mutex group we now emit TWO constraints:

    1. ``PlantCap_<stem>``: Σ generation_v ≤ P_family_max
       (generation-level soft cap at the largest config's pmax)
    2. ``PlantCommit_<stem>``: Σ commitment(uc_v).status ≤ 1
       (commitment-level mutex — matches PLEXOS's pure-binary
       formulation, which CEN PCP confirms via CPLEX's
       "0 SOSs" header)

    Both are needed.  ``PlantCap`` alone is INSUFFICIENT — without
    a status-level mutex the LP commits every (config × fuel-band)
    variant simultaneously (status_i = 1 for all i, each running at
    a fraction of pmax that sums under the cap).  Verified on the
    CEN PCP weekly bundle: ATA-CC1 had 14 variants of the same
    physical train showing ``status = 1`` in the MIP solution of
    every block (TG1A alone burning 6 fuels at once, also part of
    TG1A+TG1B+TV1C combined cycle simultaneously) — all physically
    impossible.

    ``PlantCommit`` closes the loop by enforcing exactly the PLEXOS
    constraint: Σ status ≤ 1 over EVERY config × fuel variant of the
    same physical plant.  Once this binds, only one config has
    status = 1, so the generation-level cap is automatically
    satisfied (gen ≤ status × pmax ≤ pmax).  PlantCap is kept as
    defence-in-depth (it doesn't bind when PlantCommit does, but
    catches data anomalies where pmin sums exceed the single-config
    envelope — see CAMPICHE family).

    Also kept: the **fuel-band fallback** family cap for gens NOT
    covered by a ``_Uniq`` group (caps fuel-band arbitrage within a
    single configuration; no status-level partner emitted there since
    the fuel-band variants share the same physical hardware bound by
    pmax).

    Penalty = 10_000 (matches existing PlantCap) so a rare data
    inconsistency doesn't make the LP infeasible.  ``active`` defaults
    to ``True`` (binds in every block).
    """

    def _peak_pmax(g: GeneratorSpec) -> float:
        # Effective per-variant cap = peak over horizon (Gen_Rating
        # profile if present, else scalar pmax).
        if g.pmax_profile:
            return max(g.pmax_profile)
        return g.pmax or 0.0

    # Commitment names emitted by the writer are ``uc_<generator_name>``
    # (one CommitmentSpec per committable generator).  We can only emit
    # a mutex term for variants that actually carry a commitment; pure
    # always-on renewables and hydros without min-up-down constraints
    # do not get a ``CommitmentSpec`` and so do not have an
    # ``uc_<gen>`` column to bind on.  ``case.commitments`` is the
    # authoritative source — derived in ``extract_commitments`` from
    # the PLEXOS ``Gen_Commit.csv`` / ``Min Stable Level`` / ``Start
    # Cost`` extraction chain.
    committable: set[str] = {
        c.generator_name for c in case.commitments if c.generator_name
    }

    def _emit_cap(name: str, variants: list[GeneratorSpec], description: str) -> bool:
        active = [v for v in variants if _peak_pmax(v) > 0.0]
        if len(active) < 2:
            return False
        cap = max(_peak_pmax(v) for v in active)
        if cap <= 0.0:
            return False
        terms = [f'1 * generator("{v.name}").generation' for v in active]
        out.append(
            UserConstraintSpec(
                name=name,
                expression=" + ".join(terms) + f" <= {cap:.6f}",
                penalty=10000.0,
                description=description,
            )
        )
        return True

    def _emit_commit_mutex(
        name: str, variants: list[GeneratorSpec], description: str
    ) -> bool:
        """Emit the missing config-exclusivity status mutex.

        Mirrors a PLEXOS ``*_Uniq`` constraint (``Σ status ≤ 1``) but
        widened to ALL variants of the physical plant — covering the
        full-CC ``TG1A+TG1B+TV1C_*`` family that PLEXOS's half-config
        ``ATA_CC_1_Uniq`` (TG1A+0.5TV1C + TG1B+0.5TV1C only) leaves
        unguarded.  Without this row the LP commits every (config ×
        fuel-band) variant simultaneously and runs each at a fraction
        of its pmax, producing the +1986 % ATA-TG1A+TG1B+TV1C_GNL_E
        over-dispatch observed against PLEXOS.  Emitted with the same
        $10,000/unit penalty tier as ``PlantCap`` so it can't make the
        LP infeasible on a stray data anomaly.
        """
        # Filter to variants that (a) are NOT always-on (so they have
        # a commitment status column) and (b) have positive pmax (so
        # the LP would actually consider committing them).
        active = [v for v in variants if _peak_pmax(v) > 0.0 and v.name in committable]
        if len(active) < 2:
            return False
        terms = [f'1 * commitment("uc_{v.name}").status' for v in active]
        out.append(
            UserConstraintSpec(
                name=name,
                expression=" + ".join(terms) + " <= 1",
                penalty=10000.0,
                description=description,
            )
        )
        return True

    spec_by_name = {g.name: g for g in case.generators}
    out: list[UserConstraintSpec] = []
    covered: set[str] = set()
    n_mutex = 0

    # 1. Config-exclusivity caps from PLEXOS ``*_Uniq`` mutex groups.
    for uniq_name, members in mutex_groups:
        variants = [spec_by_name[n] for n in members if n in spec_by_name]
        stem = uniq_name[:-5] if uniq_name.endswith("_Uniq") else uniq_name
        desc = (
            f"Combined-cycle config exclusivity (synth from PLEXOS "
            f"'{uniq_name}' mutex group): Σ generation [MW] over all "
            f"{len(variants)} (config × fuel-band) variants of the physical "
            f"plant ≤ its largest single-config pmax — stops the LP "
            f"co-dispatching mutually-exclusive configurations "
            f"(File: DBSEN_PRGDIARIO.xml)"
        )
        if _emit_cap(f"PlantCap_{stem}", variants, desc):
            n_mutex += 1
            covered.update(v.name for v in variants if _peak_pmax(v) > 0.0)
        # Wire the matching commitment-level mutex (the docstring
        # promised it but the body never emitted it).  Same name stem
        # + ``PlantCommit_`` prefix; covers the same widened variant
        # set as the generation cap so the ``TG1A+TG1B+TV1C_*`` full-CC
        # family (which PLEXOS's half-config ``_Uniq`` leaves unguarded)
        # cannot co-commit with the ``TG1A+0.5TV1C_*`` half-CC family.
        cmt_desc = (
            f"Combined-cycle commitment mutex (synth from PLEXOS "
            f"'{uniq_name}' mutex group): Σ commitment status [0/1] "
            f"over all {len(variants)} (config × fuel-band) variants of "
            f"the physical plant ≤ 1 — pairs with PlantCap_{stem} to "
            f"prevent co-commitment that PlantCap alone could not stop "
            f"(LP can satisfy a generation cap by splitting Σ status "
            f"across every variant at a fraction of its pmax).  "
            f"(File: DBSEN_PRGDIARIO.xml)"
        )
        _emit_commit_mutex(f"PlantCommit_{stem}", variants, cmt_desc)

    # 2. Fuel-band fallback caps, skipping gens already covered by a
    #    ``_Uniq`` group (whose cross-config cap subsumes them).
    families = _detect_plant_families(case.generators)
    for stem, variants in sorted(families.items()):
        if any(v.name in covered for v in variants):
            continue
        desc = (
            f"Multi-fuel-band plant cap (synth, no PLEXOS '_Uniq' present): "
            f"Σ generation [MW] over the fuel-band variants of '{stem}' ≤ the "
            f"single-config pmax — caps fuel-band arbitrage within one config "
            f"(File: DBSEN_PRGDIARIO.xml)"
        )
        _emit_cap(f"PlantCap_{stem}", variants, desc)
        # And the matching commitment-level mutex on the fuel-band
        # variants of one config.  Without this the LP commits all
        # fuel bands of the same physical generator simultaneously
        # (each at a small status), satisfying PlantCap by Σ status ×
        # gen ≤ pmax but co-firing.
        cmt_desc = (
            f"Multi-fuel-band commitment mutex (synth, no PLEXOS '_Uniq'): "
            f"Σ commitment status [0/1] over the fuel-band variants of "
            f"'{stem}' ≤ 1 — pairs with PlantCap_{stem} so the LP cannot "
            f"co-commit fuel bands of the same physical generator.  "
            f"(File: DBSEN_PRGDIARIO.xml)"
        )
        _emit_commit_mutex(f"PlantCommit_{stem}", variants, cmt_desc)

    if out:
        logger.info(
            "synthesised %d PlantCap_* UserConstraint(s) "
            "(%d config-exclusivity from PLEXOS `_Uniq` mutex groups + "
            "%d fuel-band fallback families); each caps Σ variant."
            "generation at the single-config pmax envelope to stop the "
            "LP co-dispatching mutually-exclusive configurations.",
            len(out),
            n_mutex,
            len(out) - n_mutex,
        )
    return tuple(out)


def _extract_boundary_cut(bundle: PlexosBundle) -> BoundaryCutSpec | None:
    """Parse ``Hydro_StoWaterValues.csv`` into one future-cost cut.

    The file is a single boundary point (all rows at PERIOD=1): the
    ``FCF`` row is the cut intercept, every other row is a reservoir's
    water value ($/GWh).  Returns ``None`` when the file is absent or
    carries no reservoir slopes.
    """
    if not bundle.has("Hydro_StoWaterValues.csv"):
        return None
    # Long format NAME,YEAR,MONTH,DAY,PERIOD,VALUE — PERIOD=1 lands in
    # slot 0 of each per-name series.
    data = read_long(bundle.csv("Hydro_StoWaterValues.csv"), n_days=1)
    values = {name: series[0] for name, series in data.items() if series}
    fcf = values.pop("FCF", 0.0)
    slopes = {name: v for name, v in values.items() if v != 0.0}
    if not slopes:
        logger.info("Hydro_StoWaterValues.csv carries no reservoir slopes")
        return None
    logger.info(
        "boundary cut: FCF intercept %.3e + %d reservoir water values",
        fcf,
        len(slopes),
    )
    return BoundaryCutSpec(fcf=fcf, slopes=slopes)


def extract_plants(case: PlexosCase) -> tuple[PlantSpec, ...]:
    """Build a :class:`PlantSpec` per multi-config Generator family.

    Successor to :func:`_build_plant_cap_ucs`: emits one native
    :class:`Plant` entry per detected family instead of a soft
    ``PlantCap_<stem>`` UserConstraint.  The LP-side ``PlantLP`` then
    enforces ``Σ generation ≤ pmax`` as a hard row per (stage, block),
    eliminating the per-block UC and its 10 000 $/MWh slack column.

    Detection mirrors :func:`_detect_plant_families` — group by name
    stem (= name minus fuel-band suffix), keep only stems with ≥ 2
    active variants.  Each PlantSpec carries:

      * ``pmax = max(peak_pmax over active variants)`` — the single-
        config envelope of the physical plant.
      * ``n_units = None`` — disabled by default (the Σ-cap is the
        primary mechanism; per-plant commit budgets are wired only
        when PLEXOS data justifies it).
      * ``uniq_mutex = False`` — disabled by default (Σ-cap subsumes
        the mutex when every variant runs near pmax; the mutex is a
        future refinement for CC config-binary cases).
    """
    families = _detect_plant_families(case.generators)
    out: list[PlantSpec] = []
    for stem, variants in sorted(families.items()):

        def _peak_pmax(g: GeneratorSpec) -> float:
            if g.pmax_profile:
                return max(g.pmax_profile)
            return g.pmax or 0.0

        active_variants = [v for v in variants if _peak_pmax(v) > 0.0]
        if len(active_variants) < 2:
            continue
        cap = max(_peak_pmax(v) for v in active_variants)
        if cap <= 0.0:
            continue
        out.append(
            PlantSpec(
                name=stem,
                generator_names=tuple(v.name for v in active_variants),
                pmax=cap,
                uniq_mutex=False,
            )
        )
    if out:
        logger.info(
            "extract_plants: emitted %d native Plant primitive(s) "
            "(one per multi-fuel-band Generator family; replaces the "
            "synthesised PlantCap_* UserConstraints with a hard "
            "plant_cap_<name> LP row per stage × block)",
            len(out),
        )
    return tuple(out)


#: Regex that splits a hydro min/max/maxramp UC name into ``(stem, kind)``.
#: Matches names like ``ANTUCOmin`` / ``ELTOROmax`` / ``ANGOSTURAmaxramp``.
#: Kind ``maxramp`` is recognised but treated like ``max`` for the
#: per-unit Commitment cap.
_HYDRO_MIN_MAX_RE = re.compile(
    r"^(?P<stem>[A-Za-z][A-Za-z0-9_]*?)(?P<kind>min|max|maxramp)$"
)

#: Regex that extracts a single ``generator("<NAME>").generation`` term
#: with a positive coefficient.  ``coeff`` is optional (defaults to 1).
_SINGLE_GEN_TERM_RE = re.compile(
    r"^\s*(?:(?P<coeff>[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*\*\s*)?"
    r'generator\("(?P<gen>[^"]+)"\)\.generation\s*'
    r"(?P<op><=|>=|=)\s*(?P<rhs>[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$"
)


def _parse_single_gen_uc(expression: str) -> tuple[str, float, str, float] | None:
    """Parse an UC LHS of form ``c * generator("G").generation OP RHS``.

    Returns ``(gen_name, coeff, op, rhs)`` or ``None`` when the
    expression doesn't match (multi-term LHS, different variable kind,
    non-trivial RHS, etc).
    """
    m = _SINGLE_GEN_TERM_RE.match(expression)
    if m is None:
        return None
    coeff = float(m["coeff"]) if m["coeff"] is not None else 1.0
    try:
        rhs = float(m["rhs"])
    except (TypeError, ValueError):
        return None
    return m["gen"], coeff, m["op"], rhs


def _varying_profile_or_empty(
    profile: tuple[float, ...] | None,
) -> tuple[float, ...]:
    """Return ``profile`` only when it actually varies, else ``()``.

    Mirrors the writer's ``max(profile) != min(profile)`` guard: an
    all-equal profile carries no information beyond the scalar bound,
    so it is collapsed to the empty tuple (the writer then keeps the
    inline scalar).
    """
    if not profile:
        return ()
    return profile if max(profile) != min(profile) else ()


def _combine_bound_profiles(
    acc: tuple[float, ...] | None,
    rhs_profile: tuple[float, ...],
    rhs: float,
    *,
    reducer: Callable[[float, float], float],
) -> tuple[float, ...]:
    """Element-wise combine a per-hour bound profile into ``acc``.

    The common case is exactly one UC per ``(gen, kind)`` so this just
    returns ``rhs_profile`` (or broadcasts the scalar ``rhs`` when the
    UC has no per-hour profile).  When MULTIPLE UCs target the same
    ``(gen, kind)`` the profiles are combined element-wise with
    ``reducer`` (``max`` for pmin, ``min`` for pmax) — consistent with
    the scalar aggregation rule.  An empty incoming profile is
    broadcast from its scalar across the accumulator's horizon so the
    element-wise combine stays well-defined.
    """
    incoming = rhs_profile
    if acc is None:
        # First UC for this (gen, kind): carry its profile verbatim
        # (empty when the bound is constant — handled by the caller's
        # ``_varying_profile_or_empty`` guard at construction time).
        return incoming
    if not acc and not incoming:
        return ()
    horizon = max(len(acc), len(incoming))
    acc_full = acc if acc else (rhs,) * horizon
    inc_full = incoming if incoming else (rhs,) * horizon
    if len(acc_full) != len(inc_full):
        # Mismatched horizons (should not happen for a single case) —
        # fall back to the longer one untouched rather than guess.
        return acc_full if len(acc_full) >= len(inc_full) else inc_full
    return tuple(reducer(a, b) for a, b in zip(acc_full, inc_full, strict=True))


def _auto_promote_hydro_min_max_to_commitments(
    base_ucs: tuple[UserConstraintSpec, ...],
    hydro_ucs: tuple[UserConstraintSpec, ...],
    generators: tuple[GeneratorSpec, ...],
    existing_commitments: tuple[CommitmentSpec, ...],
) -> tuple[tuple[CommitmentSpec, ...], frozenset[str]]:
    """Promote hydro ``<NAME>min/max`` UCs to per-generator Commitments.

    PLEXOS encodes per-plant turbine generation floors / caps as
    ``ANTUCOmin``/``ANTUCOmax``/``ELTOROmax`` UserConstraints whose
    LHS reduces to a single ``generator(<HYDRO_Ui>).generation`` term
    each (one row per unit when the plant has multiple units).  These
    are operational on/off bounds that gtopt represents natively via
    ``Commitment.pmin`` / ``Commitment.pmax`` — the LP enforces
    ``pmin·u ≤ gen ≤ pmax·u`` once a Commitment exists for the
    generator, so the per-block UC becomes redundant.

    For every matched generator we:

      * Synthesise a :class:`CommitmentSpec` with ``initial_status=1``
        and ``pmin`` / ``pmax`` from the bounds (``pmax=0`` when only
        a ``min`` UC matches).
      * Mark the matching base UC name AND the corresponding
        ``discharge_<NAME>min/max`` mirror for drop.

    Returns ``(new_commitments, drop_uc_names)``.  Returns empty
    tuples when no match is found (existing UCs are kept verbatim).
    """
    # Hydro = generator with NO fuel membership.
    hydro_names: set[str] = {g.name for g in generators if not g.fuel_names}
    if not hydro_names:
        return ((), frozenset())
    committed: set[str] = {c.generator_name for c in existing_commitments}

    # Index base UCs by (stem, kind) ->
    #   [(gen_name, op, rhs, uc_name, rhs_profile)].
    # ``rhs_profile`` carries the per-hour varying bound (length
    # 24 × n_days, empty ``()`` when the bound is constant) so the
    # native Commitment can reproduce the per-day discharge floor /
    # cap instead of flattening the horizon to the day-1 scalar.
    by_stem: dict[
        tuple[str, str], list[tuple[str, str, float, str, tuple[float, ...]]]
    ] = {}
    for uc in base_ucs:
        m = _HYDRO_MIN_MAX_RE.match(uc.name)
        if m is None:
            continue
        parsed = _parse_single_gen_uc(uc.expression)
        if parsed is None:
            continue
        gen_name, coeff, op, rhs = parsed
        if abs(coeff - 1.0) > 1e-9:
            # Reject any non-unit coefficient — pmin/pmax map 1:1
            # to ``generator.generation`` and a scaled LHS would
            # shift the bound.
            continue
        if gen_name not in hydro_names:
            continue
        if gen_name in committed:
            continue
        stem, kind = m["stem"], m["kind"]
        by_stem.setdefault((stem, kind), []).append(
            (gen_name, op, rhs, uc.name, uc.rhs_profile)
        )

    if not by_stem:
        return ((), frozenset())

    # Group across kinds: per generator collect (pmin, pmax, source-stem).
    pmin_by_gen: dict[str, float] = {}
    pmax_by_gen: dict[str, float] = {}
    # Per-gen per-hour FLOOR profile (length 24 × n_days).  The native
    # Commitment carries the ``min`` profile as ``pmin_profile`` (the
    # writer emits it as a per-block TB schedule).  The ``max`` profile
    # is deliberately NOT collected: it has no Commitment home —
    # ``Generator.pmax`` already caps the upper bound and the max UC is
    # promoted only to drop the now-redundant constraint.
    pmin_profile_by_gen: dict[str, tuple[float, ...]] = {}
    stem_by_gen: dict[str, str] = {}
    drop_base_uc_names: set[str] = set()
    drop_stems: set[str] = set()
    for (stem, kind), rows in by_stem.items():
        for gen_name, op, rhs, uc_name, rhs_profile in rows:
            stem_by_gen.setdefault(gen_name, stem)
            if kind == "min" and op == ">=":
                pmin_by_gen[gen_name] = max(pmin_by_gen.get(gen_name, 0.0), rhs)
                pmin_profile_by_gen[gen_name] = _combine_bound_profiles(
                    pmin_profile_by_gen.get(gen_name),
                    rhs_profile,
                    rhs,
                    reducer=max,
                )
                drop_base_uc_names.add(uc_name)
                drop_stems.add(stem)
            elif kind in ("max", "maxramp") and op == "<=":
                # Pick the tightest (smallest positive) max bound when
                # multiple ``max`` UCs reference the same generator.
                cur = pmax_by_gen.get(gen_name, 0.0)
                pmax_by_gen[gen_name] = rhs if cur == 0.0 else min(cur, rhs)
                drop_base_uc_names.add(uc_name)
                drop_stems.add(stem)

    if not stem_by_gen:
        return ((), frozenset())

    # Also drop the ``discharge_<stem>min/max`` soft mirrors emitted by
    # ``extract_hydro_discharge_user_constraints`` — they cap the same
    # turbine discharge sum the native Commitment now enforces via pmin/pmax.
    drop_discharge_names: set[str] = set()
    for uc in hydro_ucs:
        if not uc.name.startswith("discharge_"):
            continue
        bare = uc.name[len("discharge_") :]
        m = _HYDRO_MIN_MAX_RE.match(bare)
        if m is None:
            continue
        if m["stem"] in drop_stems:
            drop_discharge_names.add(uc.name)

    new_commitments: list[CommitmentSpec] = []
    for gen_name in sorted(stem_by_gen):
        pmin = pmin_by_gen.get(gen_name, 0.0)
        pmax = pmax_by_gen.get(gen_name, 0.0)
        # Carry the per-day varying floor as ``pmin_profile`` so the
        # writer emits a per-block TB schedule (the discharge bounds in
        # e.g. ``Hydro_AntucoBounds.csv`` step down across the week).
        # Mirror the writer's ``max != min`` guard: an all-equal profile
        # adds nothing over the scalar ``pmin``, so leave it empty.
        pmin_profile = _varying_profile_or_empty(pmin_profile_by_gen.get(gen_name))
        new_commitments.append(
            CommitmentSpec(
                generator_name=gen_name,
                pmin=pmin,
                pmin_profile=pmin_profile,
                initial_status=1.0,
            )
        )
        logger.info(
            "auto-promoted hydro min/max UC for stem %s to Commitment(%s) "
            "pmin=%g pmax=%g%s",
            stem_by_gen[gen_name],
            gen_name,
            pmin,
            pmax,
            " (per-block pmin schedule)" if pmin_profile else "",
        )

    drop_names = frozenset(drop_base_uc_names | drop_discharge_names)
    return tuple(new_commitments), drop_names


def extract_case(
    bundle: PlexosBundle,
    *,
    lax_uc_refs: bool = False,
    plexos_legacy: bool = False,
) -> PlexosCase:
    """Run every extractor and return the assembled :class:`PlexosCase`.

    This is the single entry-point the writer should consume; the
    individual ``extract_*`` functions are exported for unit-test
    targeting only.

    :param lax_uc_refs: when True, downgrade the strict UserConstraint
        reference check (``UnresolvedConstraintReferenceError``) to a
        warning + silent per-term drop.  See ``--lax-uc-refs`` on the
        CLI for the use case (debugging / iterative parser work).
    :param plexos_legacy: when True, emit PLEXOS-faithful formulations
        even when they're not the physically / economically right
        choice.  See ``--plexos-legacy`` on the CLI for the running
        list of toggles.  Mirrors plp2gtopt's ``--plp-legacy``.
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
    forced_waterway_targets: list[tuple[str, str, float, tuple[float, ...]]] = []
    # Drain configs harvested from ``Vert_*`` spillway arcs that the
    # extractor collapses onto the source storage's junction instead of
    # emitting a ``<src>_ocean`` Junction + connecting Waterway.  Maps
    # ``source_storage_name → {'drain_capacity', 'drain_cost'}``;
    # consumed by ``extract_junctions`` (extra_drain_configs kwarg)
    # below to set the new ``Junction.drain_capacity`` / ``drain_cost``
    # fields on the corresponding junction.
    junction_drain_configs: dict[str, dict[str, float | None]] = {}
    # Real reservoirs = those that will SURVIVE the pondage/RoR demotion
    # below.  A ``Vert_<src>`` spill whose source is a real reservoir must
    # NOT be collapsed onto that reservoir's junction (a drain belongs on
    # ending/ocean junctions only — the reservoir already drains via its
    # own ``spillway_cost``).  Computed here so ``extract_waterways`` can
    # route those spills to the ``<source>_ocean`` ending junction instead.
    real_reservoir_names = frozenset(
        r.name for r in reservoirs if not _reservoir_is_pure_pondage(r)
    )
    waterways = extract_waterways(
        db,
        bundle,
        forced_targets_out=forced_waterway_targets,
        junction_drain_configs_out=junction_drain_configs,
        real_reservoir_names=real_reservoir_names,
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
    # ``generator_nameplates``: PHYSICAL ``Max Capacity`` [MW] per gen,
    # captured here so the reservoir extraction-flow estimator can size
    # ``fmax`` from full turbine capacity even for units PLEXOS holds
    # offline (dispatch ``pmax = 0``).  Never written into the JSON.
    generator_nameplates: dict[str, float] = {}
    generators = extract_generators(db, bundle, nameplates_out=generator_nameplates)
    fuels = extract_fuels(db, bundle)
    turbines = extract_turbines(db, bundle)
    # Pre-filter snapshot — discharge UCs (``extract_hydro_discharge_
    # user_constraints``) need the production_factor for EVERY PLEXOS
    # membership generator, including units whose ``Gen_UnitsOut.csv``
    # entry makes them inactive day-of (e.g. ``ANTUCO_U2`` /
    # ``EL_TORO_U1`` on the 2026-04-07 bundle).  Without this snapshot
    # the UC LHS falls back to ``coeff = 1.0`` for the dropped units,
    # corrupting the m³/s discharge sum.
    all_turbines_for_pf = turbines

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
    # Turbines dropped because their generator is offline for the whole horizon
    # (``pmax = 0``).  Kept here so the reservoir extraction-flow estimator can
    # still size the ``fmax`` bound from the offline unit's nameplate (via
    # ``case.extra_turbines``) without re-admitting them to ``turbine_array``.
    dropped_turbs: tuple[TurbineSpec, ...] = ()
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

        # KEEP the inactive generators in the JSON as bounded-zero
        # placeholders (pmax = 0 ⇒ the LP column is constrained to
        # exactly 0) so any User Constraint / FueMaxOff_* that
        # references them by name still resolves through the gtopt
        # ``element_known`` path and contributes a zero coefficient
        # — silently — instead of throwing under strict resolver
        # mode.  ``extract_reserve_provisions`` filters its own
        # eligibility list against the emitted gen set so dangling
        # ``ReserveProvision`` rows do NOT make it into the JSON;
        # this is the actual fix for the PLEXOS-mirroring infeasibility
        # (~15 dropped reserves on the 31 always-offline diesels,
        # ~395 on the larger GNL-config fleet).

    # ── Pseudo-hydro generators ──
    # PLEXOS-classified hydro generators with NO Storage attachment
    # (no Head Storage / Tail Storage membership) — small ROR plants
    # like LA_HIGUERA, LA_CONFLUENCIA, ALFALFAL, PEUCHEN, SAUZAL, ...
    # (~99 on CEN PCP) — are LEFT AS-IS.  PLEXOS dispatches them
    # purely from ``Gen_Rating.csv`` per-block availability, which we
    # already read into ``GeneratorSpec.pmax_profile``.  The
    # per-block ``pmax`` bound IS the water-driven dispatch envelope,
    # so the existing ``gen[t] <= pmax_profile[t]`` constraint is
    # equivalent to a synthetic pond + inflow + penstock + turbine
    # topology where the penstock would carry exactly ``pmax_profile``
    # water and the turbine would convert with ``pf = 1``.  No
    # benefit to adding the redundant water topology — same LP
    # behaviour, +400 entities of JSON bloat, and the hard
    # ``Flow.discharge`` equality on the synthetic inflow would
    # over-constrain the LP when combined with ``--use-plexos-gen-cap``
    # (the cap drops pmax below Gen_Rating but the discharge stays at
    # the higher value, producing infeasibility — verified on CEN PCP
    # 2026-04-22).
    # ─────────────────────────────────────────────────────────────

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
    # Demotion predicate lives at module scope (``_reservoir_is_pure_
    # pondage``) so the same definition feeds both this demotion and the
    # early ``real_reservoir_names`` set passed to ``extract_waterways``.
    # We DO allow demotion when the reservoir is still a turbine
    # ``main_reservoir`` reference, because the writer synthesises a
    # Junction with the same name (extract_junctions adds
    # ``extra_junction_names`` below) and the turbine's head_storage
    # lookup resolves through that Junction.  This cleans up degenerate
    # run-of-river plants (ISLA, LA_MINA, LAJA_I, LOMAALTA, RUCUE,
    # QUILLECO, CURILLINQUE, SANIGNACIO on CEN PCP).
    pondage_names = tuple(
        sorted(r.name for r in reservoirs if _reservoir_is_pure_pondage(r))
    )
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
        # Apply GTOPT_RESERVOIR_SPILL=strict: discard Junction.drain
        # ONLY for the FINAL real reservoir set (post-demotion) — the
        # demoted pondage/tailrace junctions keep their drain because
        # they're cascade pass-through nodes that need it to dispose
        # of water arriving when downstream turbines are capped.
        # In ``basic`` mode we keep ALL drain mechanisms — both the
        # Reservoir.spillway (cost=0) and the Junction.drain — and let
        # the LP pick the cheapest.  The reservoir spillway wins on
        # cost (free vs $3.6) so this is mostly cosmetic, but the
        # caller can opt out of the duplicate-removal step.
        _rs_mode = _os_rs.environ.get("GTOPT_RESERVOIR_SPILL", "").lower()
        if _rs_mode == "strict":
            real_reservoir_names = frozenset(r.name for r in reservoirs)
            removed_real = [
                n
                for n in list(junction_drain_configs.keys())
                if n in real_reservoir_names
            ]
            for n in removed_real:
                junction_drain_configs.pop(n, None)
            if removed_real:
                logger.info(
                    "extract_case: GTOPT_RESERVOIR_SPILL=strict — removed "
                    "Junction.drain on %d real reservoirs (Reservoir.spillway "
                    "cost=0 is the sole exit): %s",
                    len(removed_real),
                    ", ".join(sorted(removed_real)),
                )
        junctions = extract_junctions(
            reservoirs,
            extra_junction_names=extra_junction_names,
            drain_configs=junction_drain_configs or None,
        )
        known_junction_names = frozenset(j.name for j in junctions)

        # Strict-mode duplicate-spillway cleanup on the WATERWAY tuple.
        # In ``--reservoir-spillway=strict`` we also strip any waterway
        # acting as a parallel spillway path to the Reservoir's own
        # ``spillway_cost=0`` drain:
        #   * surviving ``Vert_<X>`` arcs (defensive — the upstream
        #     collapse should have handled them, but bundles without
        #     the collapse path or with non-Storage Vert source could
        #     leave them in place)
        #   * pure-spillway arcs (``fmax = 0.0`` → unbounded, ``fcost > 0``)
        #     originating at a real reservoir
        # Both classes are dropped from the waterway tuple entirely so
        # the LP has no competing spillage path.  ``basic`` mode keeps
        # them — the LP will still prefer the cheaper reservoir
        # spillway, but the alternates remain available for diagnostic
        # purposes.
        if _rs_mode == "strict":
            real_res_names = {r.name for r in reservoirs}
            ww_to_drop: list[str] = []
            keep_vert = _vert_waterways_referenced_by_constraints(db)
            for w in waterways:
                # 1. Surviving Vert_<X> (defensive).  Exception: arcs
                #    referenced by some UserConstraint Flow Coefficient
                #    membership (``keep_vert``, auto-derived from the
                #    PLEXOS input) are kept on purpose so the UC terms
                #    resolve — dropping them here would re-introduce the
                #    gen-only-LHS divergence we just fixed upstream.
                if w.name.startswith("Vert_") and w.name not in keep_vert:
                    ww_to_drop.append(w.name)
                    continue
                # 2. Pure-spillway: unbounded fmax + positive fcost from
                #    a real reservoir.  ``fmax = 0.0`` means unbounded
                #    (no JSON ``fmax`` key emitted).
                if w.fmax == 0.0 and w.fcost > 0.0 and w.storage_from in real_res_names:
                    ww_to_drop.append(w.name)
            if ww_to_drop:
                logger.info(
                    "extract_case: GTOPT_RESERVOIR_SPILL=strict — dropped %d "
                    "duplicate spillway waterway(s) (Vert_* survivors + "
                    "fmax=∞/fcost>0 arcs from real reservoirs): %s",
                    len(ww_to_drop),
                    ", ".join(sorted(ww_to_drop)),
                )
                drop_set = set(ww_to_drop)
                waterways = tuple(w for w in waterways if w.name not in drop_set)

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
    # PLEXOS contingency-state shadow Line names with Units=0 across the
    # entire horizon — captured here so the UC extractor can recognise
    # ``waterway("Vert_X").flow``-style terms whose parent Line is
    # mothballed / contingency-inactive (per Lin_Units.csv) and drop them
    # silently (matches PLEXOS's own zero-flow contribution) instead of
    # raising the fail-hard unresolved-ref error.
    shadow_lines_all_off: set[str] = set()

    # Reservoir extraction-flow bound contribution from offline units.
    # ``dropped_turbs`` (turbines of generators PLEXOS holds offline for the
    # week, ``pmax = 0``) are absent from the emitted ``turbine_array`` to avoid
    # a free-drain LP artefact, but their nameplate must still size the upstream
    # reservoir ``fmax``.  Build the SAME JSON turbine dicts the writer emits —
    # via ``build_turbine_array`` in built-in-waterway mode (selected by passing
    # ``extra_waterways``) so ``junction_a`` / ``junction_b`` / ``generator`` /
    # ``production_factor`` are populated identically to the online turbines —
    # and hand them to the estimator as ``extra_turbines`` (BOUND-only; never
    # written into the JSON).  Deferred import: ``gtopt_writer`` imports this
    # module, so a top-level import would be circular.
    extra_turbines: list[dict[str, Any]] = []
    if dropped_turbs:
        from .gtopt_writer import (  # noqa: PLC0415
            build_turbine_array,
            build_waterway_array,
        )

        waterway_array = build_waterway_array(
            waterways, block_layout=bundle_spec.block_layout
        )
        extra_turbines = build_turbine_array(
            dropped_turbs,
            waterways,
            extra_waterways=waterway_array,
        )

    case = PlexosCase(
        bundle=bundle_spec,
        nodes=extract_nodes(db),
        fuels=fuels,
        generators=generators,
        lines=extract_lines(db, bundle, shadow_lines_all_off_out=shadow_lines_all_off),
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
            extra_provision_gens=_uc_reserve_provision_gens(db),
            bundle=bundle,
        )
        + extract_sscc_bess_provisions(db, bundle, extract_batteries(db, bundle)),
        commitments=extract_commitments(db, bundle, generators, fuels),
        flow_rights=_synthesise_pinned_flow_rights(
            forced_waterway_targets, known_junction_names
        )
        + extract_flow_rights(bundle, turbines, known_junction_names),
        decision_variables=extract_decision_variables(db),
        boundary_cut=_extract_boundary_cut(bundle),
    )

    # Valid-reference set for UserConstraint VALIDATION.  This is the
    # set of EVERY element gtopt will actually emit — NOT a feasibility /
    # always-active filter.  Critically it includes generators with
    # ``pmax == 0`` or a per-block ``pmax_profile`` that is zero in some
    # (or all) blocks: gtopt MODELS such a generator (it gets a
    # dispatch column) and its UserConstraint resolver is LENIENT —
    # references at zero-pmax blocks silently contribute 0 to the LHS
    # (element_known via the per-stage block scan in
    # ``element_column_resolver.cpp``).  An offline-but-emitted gen is
    # therefore a VALID reference, and excluding it here (the old
    # ``_gen_always_active`` filter) would wrongly classify a real
    # reference as unresolvable and trip the hard-fail below.  The only
    # thing that must NOT be in this set is a name gtopt never emits at
    # all — that is the genuine dangling-reference case the hard-fail
    # exists to catch.
    #
    # Synthetic ``<bat>_gen`` discharge generators (created
    # UNCONDITIONALLY by ``system.cpp::expand_batteries`` for every
    # bus-coupled battery) are valid Generator references too; their
    # companion ``uc_<bat>_gen`` Commitment (also unconditional) is a
    # valid Commitment reference (used by the Battery ``Reserve Units``
    # → ``forward_to_battery_gen_commit`` rewrite).
    battery_gen_names = frozenset(f"{b.name}_gen" for b in case.batteries)
    emitted_names: dict[str, frozenset[str]] = {
        "Generator": frozenset(g.name for g in case.generators).union(
            battery_gen_names
        ),
        "Line": frozenset(line.name for line in case.lines),
        "Battery": frozenset(b.name for b in case.batteries),
        # gtopt element names for synthesised commitment / reserve
        # provision rows. The constraint writer routes coefficients via
        # the templated names (``uc_<gen>`` / ``provision_<gen>``).
        # Commitment binaries come from ``CommitmentSpec`` rows
        # (emitted as ``uc_<gen_name>``) PLUS the per-battery synthetic
        # ``uc_<bat>_gen`` commitments that ``expand_batteries`` creates
        # unconditionally (relaxed continuous u when the battery carries
        # no commitment economics — see ``source/system.cpp``).  The
        # Battery ``Reserve Units`` coefficient forwards to
        # ``commitment("uc_<bat>_gen").status``, so these names MUST be
        # recognised as valid references.
        "Commitment": frozenset(
            f"uc_{c.generator_name}" for c in case.commitments
        ).union(f"uc_{b.name}_gen" for b in case.batteries),
        # ReserveProvision allow-list carries the ACTUAL emitted
        # provision name (``p.name``) so that zone-suffixed SSCC BESS
        # provisions (``provision_<bat>_gen__<ZONE>``) resolve — plus
        # the legacy ``provision_<gen>`` form for the per-generator
        # reserve path (whose ``p.name`` is itself ``provision_<gen>``,
        # but kept explicit for clarity / robustness).
        "ReserveProvision": frozenset(p.name for p in case.reserve_provisions).union(
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
        # Fuel allow-list for the new ``fuel("X").offtake`` UC accessor
        # (gtopt FuelLP exposes the per-block offtake decision variable
        # ``Y_f[b]`` bound by ``Y_f − Σ hr·dur·gen = 0``).  PLEXOS
        # ``Offtake Coefficient`` UCs (``Gas_MaxOpDay*``) now translate
        # verbatim as a single ``α × fuel("<name>").offtake`` term
        # instead of being expanded into per-generator terms.
        "Fuel": frozenset(f.name for f in case.fuels),
    }
    heat_rate_by_gen = {g.name: g.heat_rate for g in case.generators if g.heat_rate}
    pmax_by_gen_for_uc = {g.name: g.pmax for g in case.generators if g.pmax > 0}
    pmax_profiles_by_gen = {
        g.name: g.pmax_profile for g in case.generators if g.pmax_profile
    }
    uc_stats_raw: dict[str, int] = {}
    # ``always_on_gens``: generators emitted with NO ``Commitment`` row
    # (typically wind/solar — PLEXOS treats their commitment.status as a
    # constant 1).  UC ``commitment("uc_<gen>").status`` terms on these
    # gens are absorbed into the RHS (``rhs_val -= coeff``) instead of
    # raising the fail-hard unresolved-ref contract.  Battery synthetic
    # ``<bat>_gen`` companions ALWAYS have a matching ``uc_<bat>_gen``
    # commitment (created unconditionally by ``expand_batteries``), so
    # they're excluded from the always-on set by construction.
    committable_gens = {c.generator_name for c in case.commitments}
    always_on_gens = frozenset(
        g.name for g in case.generators if g.name not in committable_gens
    )
    # ``unusable_provisions``: ReserveProvisions emitted WITHOUT a
    # ``reserve_zones`` membership.  These slip through
    # ``extra_provision_gens`` (UC-referenced gens that aren't Reserve
    # members) and the zero-cap config-variant path.  Gtopt's
    # ``reserve_provision_lp.cpp`` doesn't register the ``.up`` / ``.dn``
    # AMPL accessors without zone participation, so any UC term
    # referencing the provision fails to resolve.  Drop those terms
    # silently in the UC builder (PLEXOS resolves them to 0 as well —
    # no zone, no headroom contribution).
    # ReserveProvisionSpec.name is "" for the default-named per-gen
    # provisions and explicitly set only by the SSCC BESS path
    # (``provision_<bat>_gen__<ZONE>``).  Reconstruct the effective name
    # the writer / UC extractor sees: ``p.name`` when explicit, else
    # ``provision_<generator_name>``.
    unusable_provisions = frozenset(
        (p.name or f"provision_{p.generator_name}")
        for p in case.reserve_provisions
        if not p.reserve_zones
    )
    base_ucs = extract_user_constraints(
        db,
        bundle,
        emitted_names=emitted_names,
        heat_rate_by_gen=heat_rate_by_gen,
        pmax_by_gen=pmax_by_gen_for_uc,
        pmax_profiles_by_gen=pmax_profiles_by_gen,
        shadow_lines_all_off=frozenset(shadow_lines_all_off),
        always_on_gens=always_on_gens,
        unusable_provisions=unusable_provisions,
        stats_out=uc_stats_raw,
        lax_refs=lax_uc_refs,
        reserves=reserves,
        plexos_legacy=plexos_legacy,
    )
    # Tighten oversized decision-variable big-M availability gates
    # (``output - 100000*dv <= 0``) down to the gated entity's real max
    # power, so the 1e5 PLEXOS Value Coefficient stops inflating kappa.
    base_ucs = _tighten_decision_variable_bigm(
        base_ucs,
        {g.name: (g.pmax or 0.0) for g in case.generators},
        {b.name: (b.pmax_discharge or 0.0) for b in case.batteries},
        {b.name: (b.pmax_charge or 0.0) for b in case.batteries},
    )
    hydro_ucs = extract_hydro_discharge_user_constraints(
        db, bundle, all_turbines_for_pf, case.generators
    )
    # Auto-promote hydro ``<NAME>min/max`` UCs (per-unit single-term
    # ``generator(...).generation`` bounds) to native ``Commitment``
    # objects.  Once a Commitment exists, gtopt enforces
    # ``pmin·u ≤ gen ≤ pmax·u`` automatically — the per-block UC plus
    # the discharge_<NAME>min/max soft mirror become redundant and we
    # drop both.  Lets the LP take ``u = 0`` (forced-off) rather than
    # paying soft slack to violate a hard floor.
    auto_commitments, auto_dropped_ucs = _auto_promote_hydro_min_max_to_commitments(
        base_ucs, hydro_ucs, case.generators, case.commitments
    )
    if auto_dropped_ucs:
        base_ucs = tuple(uc for uc in base_ucs if uc.name not in auto_dropped_ucs)
        hydro_ucs = tuple(uc for uc in hydro_ucs if uc.name not in auto_dropped_ucs)
    if auto_commitments:
        case = dataclasses.replace(
            case, commitments=case.commitments + auto_commitments
        )
    # Solution-side FueMaxOff* weekly caps → native ``Fuel.max_offtake``
    # (soft, per-stage budget) instead of the old ``FueMaxOff_*``
    # UserConstraint approximation.  gtopt's FuelLP enforces
    # ``Σ heat_rate·gen·duration ≤ max_offtake`` over the week with a
    # priced slack — the correct weekly-fuel-budget semantics, and it
    # avoids the per-block decomposition artifacts of the UC form.
    case = dataclasses.replace(
        case, fuels=_apply_native_fuel_offtake_caps(bundle, case.fuels)
    )
    # Plant-family configuration caps — one per multi-fuel-band
    # Generator family (SAN_ISIDRO_2-TG+TV, QUINTERO_1A, NEHUENCO_2-TG,
    # NUEVA_RENCA-TG+TV, etc.).  Closes the PLEXOS configuration-
    # exclusivity gap when no ``ConfTG`` UC is present.  Emitted as
    # native ``Plant`` primitives that the LP enforces directly via
    # ``PlantLP`` (one hard ``plant_cap_<name>`` row per stage × block),
    # superseding the legacy soft ``PlantCap_*`` UserConstraint
    # approximation.
    plants = extract_plants(case)
    uc_stats = UserConstraintStats(
        raw_plexos_constraints=uc_stats_raw.get("raw_total", 0),
        empty_lhs_dropped=uc_stats_raw.get("lhs_dropped", 0),
        base_emitted=uc_stats_raw.get("emitted_base", len(base_ucs)),
        hydro_synthesized=len(hydro_ucs),
        plant_cap_synthesized=0,
    )
    # Raw PLEXOS object counts (pre-drop), for the conversion drop funnel and
    # the emissions guard (``Emission`` objects carry carbon caps the
    # converter only partially handles — see ``build_emission_array``).
    raw_class_counts = {
        cls: len(db.objects_of_class(cls))
        for cls in ("Line", "Battery", "Storage", "Waterway", "Generator", "Emission")
    }
    n_emission = raw_class_counts.get("Emission", 0)
    if n_emission > 0 and not any(
        f.co2_rate != 0.0 or f.co2_upstream_rate != 0.0 for f in case.fuels
    ):
        logger.warning(
            "extract_case: %d PLEXOS Emission object(s) present but no fuel "
            "carries a CO2 rate — carbon caps/prices are NOT converted "
            "(gtopt EmissionZone cap/price unsupported by the converter yet). "
            "Dispatch will ignore the emission limit.",
            n_emission,
        )
    case = dataclasses.replace(
        case,
        plants=plants,
        user_constraints=(tuple(base_ucs) + tuple(hydro_ucs)),
        uc_stats=uc_stats,
        raw_class_counts=raw_class_counts,
        generator_nameplates=generator_nameplates,
        extra_turbines=extra_turbines,
    )
    logger.info(
        "parsed bundle %s: nodes=%d fuels=%d gens=%d lines=%d demands=%d "
        "batteries=%d reservoirs=%d waterways=%d turbines=%d flows=%d "
        "reserves=%d provisions=%d commitments=%d flow_rights=%d "
        "plants=%d user_constraints=%d",
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
        len(case.plants),
        len(case.user_constraints),
    )
    return case


__all__ = [
    "UnresolvedConstraintReferenceError",
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
    "extract_plants",
    "extract_reserve_provisions",
    "extract_reserves",
    "extract_reservoirs",
    "extract_turbines",
    "extract_user_constraints",
    "extract_waterways",
]
