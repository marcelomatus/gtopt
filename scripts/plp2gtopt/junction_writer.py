# -*- coding: utf-8 -*-

"""Writer for converting central data to hydro system JSON format.

Converts central plant data into:
- Junctions (nodes in the hydro system)
- Waterways (connections between nodes)
- Flows (water discharges)
- Reservoirs (storage nodes)
- Turbines (energy conversion points)
- ReservoirSeepages (waterway → reservoir seepage links)
- ReservoirEfficiencies (volume-dependent turbine efficiency curves)
"""

import logging
import math
from pathlib import Path
from typing import Any, Dict, List, Optional, cast, TypedDict

import pandas as pd

from gtopt_shared.water_values import WaterValueResolver

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cenfi_parser import CenfiParser
from .cenpmax_parser import CenpmaxParser
from .cenre_parser import CenreParser
from .extrac_parser import ExtracParser
from .aflce_parser import AflceParser
from .filemb_parser import FilembParser
from .manem_parser import ManemParser
from .ralco_parser import RalcoParser
from .manem_writer import ManemWriter
from .minembh_parser import MinembhParser
from .vrebemb_parser import VrebembParser
from .ror_equivalence_parser import RorSpec
from .stage_parser import StageParser
from .mance_parser import ManceParser
from .block_parser import BlockParser
from .mance_writer import ManceWriter
from .pmin_flowright_writer import (
    _FLOW_RIGHT_DIRECTION,
    resolve_flow_right_fail_cost,
)

_logger = logging.getLogger(__name__)

# UIDs for synthetic "ocean" drain junctions start above this offset so they
# cannot collide with central UIDs (which are typically in the range 1–999).
_OCEAN_UID_OFFSET = 10000

# UID offset for consumptive irrigation-diversion FlowRights (see
# ``_process_central`` / ``is_irrigation_diversion``).  Each right's UID is
# ``_IRRIGATION_DIVERSION_UID_OFFSET + central_id``, keeping them unique,
# stable, and well clear of both central UIDs (1–999) and the waterway-fmin
# FlowRight range emitted by ``pmin_flowright_writer`` (5500+).
_IRRIGATION_DIVERSION_UID_OFFSET = 6000

# ``purpose`` metadata stamped on the irrigation-diversion FlowRight.
_IRRIGATION_DIVERSION_PURPOSE = "irrigation"

# PLP convention: ``PotMax`` (max generation power) and ``VertMax`` (max
# spillway flow) in plpcnfce.dat are sometimes set to a numeric sentinel
# meaning "essentially unbounded" — observed values are in the 9000-10042
# band (looks like 9000 + central_index) and 99999.  A real Chilean
# generator/spillway flow cap is at most a few thousand m³/s, so any value
# at or above this threshold is treated as "unspecified" and dropped from
# the emitted waterway, leaving the flow unbounded.  Keeping the sentinel
# as a literal LP upper bound inflates matrix kappa (max coef ÷ min coef)
# and yields false binding-bound duals during SDDP cuts.
#
# This is the same threshold introduced as `_is_vert_sentinel` in 8d1fff9b
# (subsequently removed by 86616b80, then re-added on both branches).
# The merged form here renames to `_is_plp_no_limit` and extends coverage
# to the ``gen`` waterway side (``fmax = PotMax / Rendi``) which never
# had the check in the first place.
_PLP_NO_LIMIT_SENTINEL = 9000.0


def _is_plp_no_limit(value: float) -> bool:
    """Return True if ``value`` looks like a PLP "no bound" sentinel."""
    return value >= _PLP_NO_LIMIT_SENTINEL


def _eval_pf_segments(segments: List[Dict[str, float]], volume: float) -> float:
    """Evaluate a piecewise-linear PF curve at ``volume``.

    Picks the segment whose breakpoint is the largest ≤ ``volume`` and
    returns ``slope*volume + constant``.  Matches PLP's convention for
    volume-indexed piecewise tables.
    """
    if not segments:
        return 0.0
    active = segments[0]
    for seg in segments:
        if float(seg.get("volume", 0.0)) <= volume:
            active = seg
        else:
            break
    slope = float(active.get("slope", 0.0))
    constant = float(active.get("constant", 0.0))
    return (slope * volume) + constant


def _merge_pf_curves_min(
    primary: List[Dict[str, float]],
    other: List[Dict[str, float]],
) -> List[Dict[str, float]]:
    """Combine two production-factor curves by taking the min at each breakpoint.

    ``primary`` carries the volume breakpoint structure (typically the
    plpcenpmax-derived curve, which has multiple segments). ``other``
    usually has a single linear segment (plpcenre's Rendi curve). For
    each breakpoint in ``primary``, evaluate both curves; if ``other``
    is lower, adopt ``other``'s slope/constant on that segment.
    Otherwise keep ``primary``'s.  Matches the user-requested
    "MIN envelope" combination of the two physical constraints.
    """
    if not primary:
        return list(other)
    if not other:
        return list(primary)
    merged: List[Dict[str, float]] = []
    for seg in primary:
        vol = float(seg.get("volume", 0.0))
        pf_prim = _eval_pf_segments(primary, vol)
        pf_other = _eval_pf_segments(other, vol)
        if pf_other < pf_prim:
            # Find the active `other` segment at this volume and adopt
            # its slope/constant on the merged segment so the line at
            # `vol` equals `pf_other` and stays monotone with `other`.
            active = other[0]
            for oseg in other:
                if float(oseg.get("volume", 0.0)) <= vol:
                    active = oseg
                else:
                    break
            merged.append(
                {
                    "volume": vol,
                    "slope": float(active.get("slope", 0.0)),
                    "constant": float(active.get("constant", 0.0)),
                }
            )
        else:
            merged.append(dict(seg))
    return merged


class _SpillwayFields(TypedDict, total=False):
    """Subset of ``Reservoir`` fields produced by ``_spillway_fields``.

    Used as the return type for the helper so its result can be
    ``**``-expanded into a ``Reservoir`` literal without mypy losing the
    statically-known key names.  Both keys are optional because the
    helper returns ``{}`` to suppress the drain column entirely for a
    never-drain sentinel (see the ``_DRAIN_KILLED_RESERVOIRS`` branch in
    ``_spillway_fields``).
    """

    spillway_cost: float
    spillway_capacity: float


class Waterway(TypedDict, total=False):
    """Represents a waterway connection between junctions in the hydro system.

    ``fmin`` / ``fmax`` accept either a numeric default (constant bound) or
    a string parquet column reference (``"fmin"`` / ``"fmax"``) when the
    bound is wired through a per-stage parquet schedule — see
    ``write_transit_pmin_parquet`` for the upgrade path.
    """

    uid: int
    name: str
    junction_a: str
    junction_b: str
    fmin: float | str
    fmax: float | str
    capacity: float
    fcost: float


class _JunctionRequired(TypedDict):
    """Required fields for Junction (always present)."""

    uid: int
    name: str
    drain: bool


class Junction(_JunctionRequired, total=False):
    """Represents a node in the hydro system.

    ``drain_capacity`` and ``drain_cost`` are optional (default +∞ / 0
    on the C++ side) and carry PLP's ``VertMax`` / ``CVert`` onto the
    junction-level drain column when the central's spillway target is
    the ocean — see ``JunctionLP::add_to_lp``.
    """

    drain_capacity: float
    drain_cost: float


class _FlowRequired(TypedDict):
    """Required fields for Flow (always present)."""

    uid: int
    name: str
    junction: str
    discharge: float | str


class Flow(_FlowRequired, total=False):
    """Represents a water discharge in the hydro system."""


class _ReservoirRequired(TypedDict):
    """Required fields for Reservoir (always present)."""

    uid: int
    name: str
    junction: str
    eini: float
    efin: float
    emin: float | str
    emax: float | str
    capacity: float
    flow_conversion_rate: float


class Reservoir(_ReservoirRequired, total=False):
    """Represents a storage node in the hydro system.

    ``use_state_variable`` is optional: when set to ``False`` the reservoir
    state (energy level) is not linked across blocks, which models small /
    independent hydro reservoirs (PLP ``Hid_Indep='T'``).

    Energy scaling is now handled exclusively via the ``variable_scales``
    option in the planning options section (not per-element fields).

    ``fmin`` / ``fmax`` (extraction / storage-flow bounds) are *optional*:
    the reservoir construction site intentionally omits them so the C++
    ``ReservoirLP`` applies its own finite defaults (see the ``qe`` bounds
    note at the reservoir literal).  They are only ever emitted by
    downstream estimators that fill concrete topology-derived bounds.
    """

    use_state_variable: bool
    fmin: float
    fmax: float
    spill_junction: str
    spillway_cost: float
    spillway_capacity: float
    annual_loss: float
    soft_emin: list[float]
    soft_emin_cost: list[float]
    seepage: List[Dict[str, Any]]
    discharge_limit: List[Dict[str, Any]]
    production_factor: List[Dict[str, Any]]
    daily_cycle: bool
    efin_cost: float


class Turbine(TypedDict, total=False):
    """Represents an energy conversion point in the hydro system.

    Connection mode is one of (mutually exclusive):

    * ``waterway`` — legacy: a pre-existing Waterway carries the flow.
    * ``junction_a`` (+ optional ``junction_b``) — built-in waterway
      mode: the Turbine owns its own flow arc, no Waterway needed.
      ``junction_b`` unset = terminal drain (no synthetic ocean Junction
      required).  When this mode is used, ``capacity`` (m³/s) caps the
      per-block water throughput on the built-in arc.
    """

    uid: int
    name: str
    generator: str
    waterway: str
    junction_a: str
    junction_b: str
    production_factor: float
    capacity: float


class ProductionFactorSegment(TypedDict):
    """One segment of a piecewise-linear efficiency curve."""

    volume: float
    slope: float
    constant: float


class ReservoirProductionFactor(TypedDict):
    """Volume-dependent turbine efficiency (PLP rendimiento).

    Maps reservoir volume to turbine conversion rate [MW·s/m³] via a
    piecewise-linear concave envelope.
    """

    uid: int
    name: str
    turbine: str
    reservoir: str
    mean_production_factor: float
    segments: List[ProductionFactorSegment]


class ReservoirSeepageSegment(TypedDict):
    """One segment of a piecewise-linear seepage curve."""

    volume: float
    slope: float
    constant: float


class _ReservoirSeepageRequired(TypedDict):
    """Required fields for ReservoirSeepage (always present)."""

    uid: int
    name: str
    waterway: str
    reservoir: str
    slope: float
    constant: float


class ReservoirSeepage(_ReservoirSeepageRequired, total=False):
    """Represents water seepage from a waterway into a reservoir.

    When ``segments`` is non-empty the piecewise-linear concave envelope
    is used: ``seepage(V) = slope_i × V + constant_i`` where the active
    segment is selected based on the current reservoir volume.  The LP
    constraint coefficients (slope on eini/efin columns and the constant RHS)
    are updated dynamically by ReservoirSeepageLP.

    ``slope`` and ``constant`` may be a scalar, an inline array (per-stage
    schedule), or a filename string referencing a Parquet schedule file.
    When ``segments`` is present these fields hold the mean/fallback values
    used before the first volume-dependent update.
    """

    segments: List[ReservoirSeepageSegment]


class ReservoirDischargeLimitSegment(TypedDict):
    """One segment of a piecewise-linear drawdown limit curve."""

    volume: float
    slope: float
    intercept: float


class _ReservoirDischargeLimitRequired(TypedDict):
    """Required fields for ReservoirDischargeLimit."""

    uid: int
    name: str
    waterway: str
    reservoir: str


class ReservoirDischargeLimit(_ReservoirDischargeLimitRequired, total=False):
    """Volume-dependent discharge limit for a reservoir.

    The LP constraint per stage is:
      qeh ≤ slope × V_avg + intercept
    where the active segment is selected by reservoir volume.
    """

    segments: List[ReservoirDischargeLimitSegment]


class HydroSystemOutput(TypedDict):
    """Output structure for hydro system JSON format."""

    junction_array: List[Junction]
    waterway_array: List[Waterway]
    flow_array: List[Flow]
    reservoir_array: List[Reservoir]
    turbine_array: List[Turbine]
    reservoir_seepage_array: List[Dict[str, Any]]
    reservoir_discharge_limit_array: List[Dict[str, Any]]
    reservoir_production_factor_array: List[Dict[str, Any]]
    flow_right_array: List[Dict[str, Any]]


class JunctionWriter(BaseWriter):
    """Converts central plant data to hydro system JSON format for GTOPT."""

    # Reservoirs whose storage-drain (spillway) column must NEVER be
    # activated.  ELTORO is our correctness sentinel: it has no spillway
    # drain so its water can only leave through the modelled topology
    # (turbine / ``_ver`` arc), which lets us catch double-escape /
    # mass-leak regressions.  Every OTHER reservoir gets the drain ENABLED
    # (``spillway_cost = 0`` emitted, ``spillway_capacity`` omitted → C++
    # +6000 m³/s default).  Mirrors plexos2gtopt's ``build_reservoir_array``.
    _DRAIN_KILLED_RESERVOIRS = frozenset({"ELTORO"})

    def __init__(  # pylint: disable=too-many-arguments
        self,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        extrac_parser: Optional[ExtracParser] = None,
        manem_parser: Optional[ManemParser] = None,
        cenre_parser: Optional[CenreParser] = None,
        cenfi_parser: Optional[CenfiParser] = None,
        filemb_parser: Optional[FilembParser] = None,
        ralco_parser: Optional[RalcoParser] = None,
        minembh_parser: Optional[MinembhParser] = None,
        vrebemb_parser: Optional[VrebembParser] = None,
        plpmat_parser: Optional[Any] = None,
        cenpmax_parser: Optional[CenpmaxParser] = None,
        mance_parser: Optional[ManceParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
        water_value_resolver: Optional[WaterValueResolver] = None,
    ) -> None:
        """Initialize hydro system writer.

        Args:
            central_parser: Parser for central plant data
            stage_parser: Parser for stage data
            aflce_parser: Parser for inflow data
            extrac_parser: Parser for extraction data
            manem_parser: Parser for reservoir maintenance schedules
            cenre_parser: Parser for reservoir efficiency (plpcenre.dat)
            cenfi_parser: Parser for seepage data (plpcenfi.dat)
            filemb_parser: Parser for primary PLP seepage model
                (plpfilemb.dat); takes precedence over cenfi_parser when
                both are present.
            ralco_parser: Parser for drawdown limit data (plpralco.dat).
            minembh_parser: Parser for soft minimum volume constraints
                (plpminembh.dat).
            cenpmax_parser: Parser for volume-dependent turbine Pmax curves
                (plpcenpmax.dat). When provided, each entry emits a
                ReservoirProductionFactor (scaled by the physical flow cap
                ``PotMax / Rendi``) and fixes the turbine's generation
                waterway ``fmax`` to that same flow cap.
            options: Configuration options for the writer
        """
        super().__init__(central_parser, options)
        self.stage_parser = stage_parser
        self.aflce_parser = aflce_parser
        self.extrac_parser = extrac_parser
        self.manem_parser = manem_parser
        self.cenre_parser = cenre_parser
        self.cenfi_parser = cenfi_parser
        self.filemb_parser = filemb_parser
        self.ralco_parser = ralco_parser
        self.minembh_parser = minembh_parser
        self.vrebemb_parser = vrebemb_parser
        self.plpmat_parser = plpmat_parser
        self.cenpmax_parser = cenpmax_parser
        self.mance_parser = mance_parser
        self.block_parser = block_parser
        # Optional auto water-shortfall pricing helper.  When the
        # resolver's ``is_active`` is True, ``_apply_soft_storage_bounds``
        # consults it instead of ``_resolve_storage_bound_cost``.  The
        # canonical caller (``_writer_hydro``) always passes an
        # instance, but for direct callers we self-construct one when
        # the unified-pricing pipeline is gated on (auto flag set or an
        # explicit ``--water-fail-cost`` override is present).
        opts = self.options or {}
        _wvr_gate = bool(opts.get("auto_water_fail_cost")) or (
            opts.get("water_fail_cost") is not None
        )
        self._water_value_resolver: Optional[WaterValueResolver] = (
            water_value_resolver
            if water_value_resolver is not None or not _wvr_gate
            else WaterValueResolver(
                central_parser=central_parser,
                cenre_parser=cenre_parser,
                options=self.options,
            )
        )
        self._embed_reservoir_constraints = bool(
            self.options.get("embed_reservoir_constraints", False)
        )
        # ``--drop-spillway-waterway`` (default False, opt-in): when on,
        # suppress every ``_ver`` (spillway / vert) waterway emission and
        # mark the central's own junction as ``drain = True`` so excess
        # water leaves the system as a junction-level loss to the ocean.
        # The tradeoff is physical accuracy: PLP routes spill to a
        # downstream central when ``ser_ver > 0`` (the water can be
        # re-used), and charges per-flow ``fcost`` (CVert / Costo de
        # Rebalse) on the spill.  Dropping the arc loses the routing AND
        # the cost — all spillover becomes a free leak — but in exchange
        # every ``_ver`` arc and its associated ``fcost`` disappears from
        # the LP, which improves scaling and removes a class of spurious
        # binding-bound duals.
        #
        # Default flipped to False after the gtopt_iplp investigation
        # (2026-04-28) showed the suppress-mode topology was implicated
        # in the SDDP elastic-cut degeneracy chain at LMAULE / ELTORO.
        # PLP-faithful spillway topology (``_ver`` waterway + per-flow
        # cost) is the safer default; opt into suppress mode only when
        # LP scaling outweighs routing fidelity for the case at hand.
        self._drop_spillway_waterway = bool(
            self.options.get("drop_spillway_waterway", False)
        )
        # ``--vrebemb-as-sink`` (default False, opt-in): for centrals in
        # plpvrebemb.dat, route ``_ver`` to a synthetic ``<name>_ocean``
        # drain and drop ``fmax``/``fcost``.  Restores PLP's qrb
        # (sink-bound, costed) rebalse semantics — PLP subtracts qrb from
        # end-of-stage storage WITHOUT adding it back at any downstream
        # junction, so the parallel-pipe ``_ver → ser_ver`` model gtopt
        # used previously can generate "fictitious water" feeding
        # downstream demand via cap arbitrage.  Non-vrebemb centrals are
        # untouched.  ``--drop-spillway-waterway`` still wins (early
        # return), see ``_process_central``.
        self._vrebemb_as_sink = bool(self.options.get("vrebemb_as_sink", False))
        # Centrals whose gen Waterway is needed downstream by
        # ``pmin_flowright_writer`` (FlowRight attached to waterway uid)
        # or by ``_ror_reservoir_spec`` (reservoir at gen_waterway's
        # junction_a).  These cannot be migrated to the built-in
        # Turbine waterway because the downstream consumers grep the
        # waterway by name.  Resolved lazily — empty when neither
        # ``--pmin-as-flowright`` nor a whitelist is set.
        self._pmin_flowright_centrals: frozenset[str] = (
            self._resolve_pmin_flowright_centrals()
        )
        # Centrals that are the SOURCE of a plpcenfi seepage reference
        # their own ``_gen`` Waterway by name (see ``_process_seepages``),
        # and ``ReservoirSeepage`` has no built-in-turbine variant (only a
        # ``waterway`` field — see ``reservoir_seepage.hpp``).  Such
        # centrals must keep their legacy ``_gen`` Waterway, so they are
        # excluded from the built-in Turbine migration.  Only relevant
        # when plpcenfi is the ACTIVE seepage source (plpfilemb, when
        # present, takes precedence and synthesises independent ``filt_``
        # waterways that never touch the gen arc).
        self._cenfi_seepage_source_centrals: frozenset[str] = frozenset()
        if self.filemb_parser is None and self.cenfi_parser is not None:
            self._cenfi_seepage_source_centrals = frozenset(
                str(entry["name"]) for entry in self.cenfi_parser.seepages
            )
        self._waterway_counter = 0
        self._ocean_junction_counter = 0
        # Counter for terminal turbine centrals (bus > 0, ser_hid = 0,
        # not an RoR-promotion candidate) that use the gtopt
        # ``Turbine.junction_a`` built-in waterway, eliding one
        # synthetic ``<central>_ocean`` Junction + one ``<central>_gen``
        # Waterway each.  Reported via ``_logger.info`` at the end of
        # ``to_json_array``.
        self._builtin_turbine_waterway_count: int = 0
        self._junction_names: dict[int, str] = {}
        self._skipped_isolated: list[str] = []
        self._referenced_junctions: set[int] = set()
        # Counter for PLP "no limit" sentinels normalised on gen and ver
        # waterways (see ``_is_plp_no_limit``).  Logged once at end of
        # ``to_json_array`` so the user can see how many spurious bounds
        # were dropped — improves LP scaling.
        self._plp_no_limit_count: int = 0
        # Counter for waterway emissions skipped because PLP shipped
        # ``VertMax = 0`` (or otherwise ended up with fmin = fmax = 0).
        # The corresponding LP column would be hard-pinned to zero and
        # collapsed by solver presolve; dropping it at emission time
        # keeps the Waterway parquet honest and removes false-positive
        # validator noise.  Logged once at end of ``to_json_array``.
        self._dead_zero_waterway_count: int = 0
        # Counter + name list for ``_ver`` spillway arcs dropped in the
        # post-pass that collapses the parallel-arc duplicate when a
        # ``filt_*`` seepage waterway shares the same (junction_a,
        # junction_b) pair (e.g. ELTORO_ver_37_38 ⇄ filt_ELTORO_37_38
        # both routing to ABANICO).  Reported via ``_logger.info`` at
        # the end of ``to_json_array``.
        self._seepage_redundant_spillway_count: int = 0
        self._seepage_redundant_spillway_names: list[str] = []
        # Gen waterways of transit centrals (``bus = 0``) that have
        # plpmance.dat per-stage flow envelopes.  These centrals have
        # no generator entry to consume Generator/pmin.parquet, so we
        # mirror the per-stage bound onto Waterway/fmin.parquet +
        # Waterway/fmax.parquet keyed by gen-waterway uid.  See
        # ``_write_transit_waterway_bounds``.  Each entry is
        # ``(central_id, gen_waterway_uid, central_name, gen_waterway_dict)``;
        # the dict reference lets us upgrade ``fmin``/``fmax`` to
        # column refs only AFTER the parquet write decides which
        # columns survive (ManceWriter drops cols matching the static
        # fill).
        self._transit_gen_waterways: list[tuple[int, int, str, "Waterway"]] = []
        # Resolved at the start of to_json_array() from --ror-as-reservoirs*
        # options.  Maps promoted central name -> RorSpec (vmax_hm3 +
        # production_factor override).
        self._ror_reservoir_spec: dict[str, RorSpec] = {}
        # Populated at the start of to_json_array() from filemb + central
        # parsers; used to suppress the parallel spillway arc when a
        # seepage already wires the same (source, target) reservoir pair.
        self._seepage_target_pairs: set[tuple[int, int]] = set()

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return cast(CentralParser, self.parser)

    def _resolve_pmin_flowright_centrals(self) -> frozenset[str]:
        """Return the set of centrals destined for FlowRight attachment.

        Mirrors :func:`pmin_flowright_writer.resolve_whitelist` but
        returns a frozenset for O(1) membership tests.  These centrals
        must keep their gen Waterway (used as the FlowRight's
        ``waterway`` reference) — the built-in Turbine waterway
        optimization skips them.
        """
        spec = self.options.get("pmin_as_flowright") if self.options else None
        if spec is None:
            return frozenset()
        # Defer to the canonical resolver in pmin_flowright_writer.
        from ._parsers import DEFAULT_PMIN_FLOWRIGHT_FILE  # noqa: PLC0415
        from .pmin_flowright_writer import resolve_whitelist  # noqa: PLC0415

        try:
            names = resolve_whitelist(
                str(spec), default_csv=DEFAULT_PMIN_FLOWRIGHT_FILE
            )
        except (FileNotFoundError, ValueError):
            # Caller will surface the error when pmin_flowright_writer
            # actually runs — here we just stay conservative and skip
            # the optimization (empty set means every terminal turbine
            # keeps its legacy gen-Waterway, same as pre-change).
            return frozenset()
        return frozenset(names)

    def _create_waterway(
        self,
        source_name: str,
        source_id: int,
        target_id: int,
        fmin: float = 0.0,
        fmax: Optional[float] = None,
        capacity: Optional[float] = None,
        fcost: Optional[float] = None,
    ) -> Optional[Waterway]:
        """Create a waterway connection between two junctions.

        Uses the junction name map (built during to_json_array) to
        resolve numeric IDs to names. Falls back to the numeric ID
        for ocean junctions that aren't in the map yet.

        ``fcost`` (optional) sets a per-flow cost on the waterway; LP
        objective gets ``fcost · waterway_flow · block_duration`` per
        block.  Used to model PLP's ``qrb`` (rebalse) penalty on `_ver`
        arcs from ``plpvrebemb.dat``.

        Dead-zero suppression: when both ``fmin`` and ``fmax`` are
        explicitly ``0.0`` the waterway column is hard-pinned to 0
        (the solver's presolve would collapse it anyway).  PLP
        encodes this for centrals with ``VertMax = 0`` ("no spill
        allowed") — e.g. ``FILT_CIPRESES_ver_7_8`` on the CEN65
        2-year case.  Skip the emission entirely so the JSON doesn't
        ship LP no-ops that confuse the validator and bloat the
        Waterway parquet count without contributing a single row to
        the LP.
        """
        if target_id == 0:
            return None

        if fmax == 0.0 and (fmin or 0.0) == 0.0:
            self._dead_zero_waterway_count += 1
            _logger.debug(
                "skipping dead-zero waterway '%s_%d_%d' "
                "(fmin = fmax = 0; PLP VertMax = 0)",
                source_name,
                source_id,
                target_id,
            )
            return None

        self._waterway_counter += 1
        ja_name = self._junction_names.get(source_id, str(source_id))
        jb_name = self._junction_names.get(target_id, str(target_id))
        waterway: Waterway = {
            "uid": self._waterway_counter,
            "name": f"{source_name}_{source_id}_{target_id}",
            "junction_a": ja_name,
            "junction_b": jb_name,
            "fmin": fmin,
        }

        if fmax is not None:
            waterway["fmax"] = fmax
        if capacity is not None:
            waterway["capacity"] = capacity
        if fcost is not None:
            waterway["fcost"] = fcost

        return waterway

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central plant data to hydro system JSON format.

        Args:
            items: Optional list of central plants to process. If None,
                   uses embalse and serie type plants from central_parser.

        Returns:
            List containing single hydro system dictionary with all elements
        """
        parquet_cols = self._write_parquet_files()

        central_parser = self.central_parser

        # Get default items if none provided
        if items is None and central_parser:
            items = (
                central_parser.centrals_of_type.get("embalse", [])
                + central_parser.centrals_of_type.get("serie", [])
            ) or []
            # In full-hydro mode, pasada centrals get junctions+waterways.
            # In flow-turbine mode, they're handled separately (no junctions).
            pasada_mode = self.options.get("pasada_mode", "flow-turbine")
            if pasada_mode == "hydro":
                items = items + [
                    c
                    for c in (central_parser.centrals_of_type.get("pasada", []) or [])
                    if c.get("bus", 0) > 0
                ]

        if not items:
            return []

        # Build number→name map for junction name references.
        # This lets _create_waterway resolve numeric IDs to names.
        self._junction_names = {}
        for c in items:
            self._junction_names[c["number"]] = c["name"]

        # Resolve --ror-as-reservoirs selection against the CSV whitelist
        # and the subset of eligible centrals currently in ``items``.
        self._ror_reservoir_spec = self._load_ror_reservoir_spec(items)

        system: HydroSystemOutput = {
            "junction_array": [],
            "waterway_array": [],
            "flow_array": [],
            "reservoir_array": [],
            "turbine_array": [],
            "reservoir_seepage_array": [],
            "reservoir_discharge_limit_array": [],
            "reservoir_production_factor_array": [],
            "flow_right_array": [],
        }

        # Track isolated centrals that were skipped
        self._skipped_isolated = []
        # Reset PLP no-limit sentinel counter for this conversion run.
        self._plp_no_limit_count = 0
        # Reset built-in Turbine waterway counter for this run.
        self._builtin_turbine_waterway_count = 0
        # Reset dead-zero waterway suppression counter for this run.
        self._dead_zero_waterway_count = 0
        # Reset seepage-redundant-spillway suppression state for this run.
        self._seepage_redundant_spillway_count = 0
        self._seepage_redundant_spillway_names = []
        # Log precedence interaction once: ``--drop-spillway-waterway``
        # suppresses every ``_ver`` arc, so ``--vrebemb-as-sink`` is a
        # no-op when both flags are on.
        if self._drop_spillway_waterway and self._vrebemb_as_sink:
            _logger.info(
                "--vrebemb-as-sink is a no-op when "
                "--drop-spillway-waterway is on; the spillway-suppress "
                "mode takes precedence (no ``_ver`` arcs are emitted)."
            )

        # Build set of junction numbers referenced as downstream targets by
        # other centrals.  A central with ser_hid=0/ser_ver=0 that IS referenced
        # by others acts as a drain/sink junction and must NOT be skipped.
        self._referenced_junctions = set()
        for c in items:
            hid = c.get("ser_hid", 0)
            ver = c.get("ser_ver", 0)
            if hid > 0:
                self._referenced_junctions.add(hid)
            if ver > 0:
                self._referenced_junctions.add(ver)

        # Build the set of (source_central_id, target_central_id) pairs that
        # WILL be covered by a seepage waterway emitted later by
        # ``_process_seepages_filemb`` / ``_process_seepages``.  Used by
        # ``_process_central`` to short-circuit emission of an operationally
        # redundant ``_ver`` spillway arc whose endpoints coincide with the
        # upcoming seepage arc (e.g. ELTORO on the CEN65 2-year case: PLP's
        # ``ser_ver = ABANICO`` matches the plpfilemb seepage target
        # ``ABANICO`` exactly, so without this guard the LP gets two
        # parallel free arcs ELTORO → ABANICO).  The seepage arc is the
        # PLP-faithful one (its piecewise volume function carries the
        # forced flow); the spillway arc has nothing left to contribute
        # under ``--auto-water-fail-cost`` defaults.
        self._seepage_target_pairs = set()
        if self.filemb_parser and central_parser:
            for entry in self.filemb_parser.seepages:
                src = central_parser.get_central_by_name(entry["embalse"])
                dst = central_parser.get_central_by_name(entry["central"])
                if src is not None and dst is not None:
                    self._seepage_target_pairs.add(
                        (int(src["number"]), int(dst["number"]))
                    )
        elif self.cenfi_parser and central_parser:
            for entry in self.cenfi_parser.seepages:
                src = central_parser.get_central_by_name(entry["name"])
                dst = central_parser.get_central_by_name(entry["reservoir"])
                if src is not None and dst is not None:
                    self._seepage_target_pairs.add(
                        (int(src["number"]), int(dst["number"]))
                    )

        # Process central plants
        for central in items:
            self._process_central(central, system, central_parser)

        # Process reservoirs
        if central_parser:
            self._process_reservoirs(system, central_parser, parquet_cols)
            # `_apply_soft_emin` / `_apply_soft_storage_bounds` populate
            # `reservoir["soft_emin"]` / `reservoir["soft_emin_cost"]` as
            # per-stage Python lists.  gtopt's `OptTBRealFieldSched`
            # variant accepts only scalar / 2-D / FileSched, so a 1-D
            # inline list trips a strict-parse error
            # ("Expected array type to begin with '['").  Promote each
            # populated list into a `Reservoir/soft_emin*.parquet`
            # file-schedule reference, matching the existing
            # `Reservoir/emin.parquet` / `emax.parquet` pattern.
            self._promote_reservoir_soft_emin_to_parquet(system)

        # Process extraction plants
        if self.extrac_parser and central_parser:
            self._process_extractions(system, central_parser)

        # Process seepage data (plpfilemb.dat takes precedence over plpcenfi.dat)
        if self.filemb_parser and central_parser:
            self._process_seepages_filemb(system, central_parser)
        elif self.cenfi_parser and central_parser:
            self._process_seepages(system, central_parser)

        # Process drawdown limit data (plpralco.dat)
        if self.ralco_parser and central_parser:
            self._process_reservoir_discharge_limits(system, central_parser)

        # Process reservoir efficiency data (plpcenre.dat)
        if self.cenre_parser and central_parser:
            self._process_reservoir_efficiencies(system)

        # Process volume-dependent turbine Pmax curves (plpcenpmax.dat).
        # Emits additional ReservoirProductionFactor entries and pins each
        # turbine's generation waterway fmax to the physical flow cap
        # ``PotMax / Rendi``.  Co-exists with plpcenre.dat efficiencies.
        if self.cenpmax_parser and central_parser:
            self._process_cenpmax(system)

        # Per-stage flow envelope (fmin/fmax) on gen waterways of
        # transit-only centrals (``bus = 0`` AND has plpmance.dat).
        self._write_transit_waterway_bounds()

        if self._plp_no_limit_count > 0:
            _logger.info(
                "Normalised %d PLP 'no limit' sentinel(s) (>= %g) on gen+ver "
                "waterway bounds — improves LP scaling.",
                self._plp_no_limit_count,
                _PLP_NO_LIMIT_SENTINEL,
            )

        if self._builtin_turbine_waterway_count > 0:
            _logger.info(
                "Built-in Turbine waterway: emitted %d terminal turbine "
                "central(s) with Turbine.junction_a (no synthetic "
                "<central>_ocean Junction + <central>_gen Waterway pair)",
                self._builtin_turbine_waterway_count,
            )

        if self._dead_zero_waterway_count > 0:
            _logger.info(
                "Suppressed %d dead-zero waterway(s) (PLP VertMax = 0 — "
                "the LP column would be pinned to 0 and collapsed by "
                "solver presolve anyway).",
                self._dead_zero_waterway_count,
            )

        if self._seepage_redundant_spillway_count > 0:
            _logger.info(
                "Suppressed %d _ver spillway arc(s) whose endpoints "
                "coincide with a filt_* seepage waterway and carry no "
                "extra constraint (operationally-redundant parallel "
                "free arc): %s",
                self._seepage_redundant_spillway_count,
                ", ".join(self._seepage_redundant_spillway_names),
            )

        return [cast(Dict[str, Any], system)]

    def _process_central(
        self,
        central: Dict[str, Any],
        system: HydroSystemOutput,
        _central_parser: CentralParser,
    ) -> None:
        """Process a single central into hydro system elements.

        For ``embalse`` centrals whose PLP ``ser_hid`` field is 0 (generation
        waterway has no modelled downstream junction — the water discharges
        directly to the sea / river mouth), a synthetic drain junction is
        created regardless of ``bus``:

            <central_name>_ocean   uid = _OCEAN_UID_OFFSET + N   drain = True

        This ensures the hydro topology (junction, waterway, reservoir, flow)
        is always complete for every embalse.  An embalse with ``bus <= 0``
        operates as a hydro dam only — no turbine or generator is created.

        When ``ser_ver`` is 0 (no spillway downstream junction) the central's
        own junction is flagged ``drain = True`` so excess water can leave the
        system without an explicit spillway waterway.
        """
        central_id = central["number"]
        central_name = central["name"]
        central_type = central.get("type", "serie")

        # **Irrigation-diversion centrals** — ``serie`` transit centrals that
        # divert water OUT of the river for irrigation while ALSO returning a
        # remainder downstream via their ``ser_ver`` arc.  Signature (matches
        # RieSur123SCDZ / RIEGZACO / RieSaltos on the CEN65 2-year case):
        # ``bus == 0`` (no electrical outlet), ``ser_hid == 0`` (the gen arc
        # has no PLP downstream junction, so it would otherwise collapse into
        # a *free* source self-drain), and ``ser_ver > 0`` (a real ``_ver``
        # return arc exists).  The diverted water is a consumptive offtake,
        # not a costless spill, so for these we (1) gate the gen-path source
        # self-drain OFF and (2) emit a consumptive FlowRight on the central's
        # own junction (see the emission block below).
        is_irrigation_diversion = (
            central_type in ("serie",)
            and int(central.get("bus", 0) or 0) == 0
            and int(central.get("ser_hid", 0) or 0) == 0
            and int(central.get("ser_ver", 0) or 0) > 0
        )

        # Skip truly isolated serie/pasada centrals: bus<=0, no outgoing
        # waterways (ser_hid=0 AND ser_ver=0), AND not referenced as a
        # downstream target by any other central.  Centrals that ARE
        # referenced act as drain/sink junctions receiving water from
        # upstream and must be kept.
        if central_type in ("serie", "pasada"):
            if (
                central.get("bus", 0) <= 0
                and central.get("ser_hid", 0) == 0
                and central.get("ser_ver", 0) == 0
                and central_id not in self._referenced_junctions
            ):
                self._skipped_isolated.append(central_name)
                _logger.debug(
                    "Skipping isolated %s central '%s' (bus<=0, no waterways,"
                    " not referenced by others)",
                    central_type,
                    central_name,
                )
                return

        # Create waterways from the PLP ser_hid / ser_ver connections.
        #
        # **Generation waterway flow cap** (``fmax = PotMax / Rendi``).
        # PLP enforces the central's max water throughput indirectly
        # via the generator's ``PotMax`` (MW) and ``Rendi`` (MW per
        # m³/s).  The same physical cap must apply to the equivalent
        # gtopt waterway, otherwise the LP can drain the upstream
        # reservoir at any rate.  Symptom on juan/gtopt_iplp: LMAULE
        # (``PotMax=100``, ``Rendi=1.0``, ``Genera=2`` → no electrical
        # turbine on the gtopt side, so the cap previously fell out
        # of the cenpmax-based emission path) drained from 657 Hm³
        # to 0 in a single stage, vs PLP keeping it 115-758 Hm³ all
        # year.  Emitting ``fmax = PotMax / Rendi`` here makes the
        # cap independent of whether a turbine entry will be created
        # downstream (``bus > 0`` path), so transit-only centrals
        # (``Genera=2``, ``bus=0``) still get the right physical
        # bound.  ``cenpmax`` consumers later override this with the
        # volume-dependent flow cap when applicable.
        gen_pot_max = float(central.get("pmax", 0.0) or 0.0)
        gen_rendi = float(central.get("efficiency", 0.0) or 0.0)
        gen_fmax: Optional[float] = None
        if gen_pot_max > 0.0 and gen_rendi > 0.0:
            # PLP encodes "no PotMax cap" as a sentinel ≥ 9000 MW.  Treat
            # it as unspecified so the gen waterway stays unbounded —
            # otherwise a 9999 MW PotMax with Rendi=1.0 would emit a
            # literal fmax=9999 m³/s upper bound, inflating LP kappa.
            if _is_plp_no_limit(gen_pot_max):
                self._plp_no_limit_count += 1
            else:
                gen_fmax = gen_pot_max / gen_rendi
        # **Built-in Turbine waterway eligibility** — computed BEFORE the
        # gen Waterway so the synthetic ``<central>_gen`` arc is never
        # created for eligible centrals.  A reservoir/serie/pasada central
        # connected to a bus carries its generation flow on the Turbine
        # itself: ``junction_a`` = its own junction, ``junction_b`` = the
        # downstream ``ser_hid`` junction when one exists (terminal plants
        # leave ``junction_b`` unset).  This mirrors the plexos2gtopt
        # built-in turbines exactly — one flow column per unit, no penstock
        # Waterway clone, no ``<central>_ocean`` Junction.
        #
        # The excluded cases keep the legacy ``_gen`` Waterway because a
        # downstream consumer greps it by name:
        #   * ``--ror-as-reservoirs`` promotion (reservoir anchored on the
        #     gen Waterway's junction_a);
        #   * ``bus == 0`` transit-only centrals (no Turbine entry; their
        #     per-stage plpmance bounds bind onto the Waterway uid);
        #   * plpcenfi seepage source centrals (``ReservoirSeepage`` has a
        #     ``waterway`` ref only — no built-in-turbine variant);
        #   * TERMINAL ``--pmin-as-flowright`` centrals (``ser_hid = 0``):
        #     the FlowRight attaches to the discharge ``junction_b``, which
        #     a terminal built-in turbine does not carry — so they keep the
        #     gen→ocean Waterway whose junction_b the FlowRight needs.
        #     Non-terminal pmin-flowright centrals migrate freely: their
        #     discharge ``junction_b`` is carried directly on the Turbine.
        is_terminal = int(central.get("ser_hid", 0) or 0) == 0
        use_builtin_turbine_waterway = (
            central_type in ("embalse", "serie", "pasada")
            and central.get("bus", 0) > 0
            and central_name not in self._ror_reservoir_spec
            and central_name not in self._cenfi_seepage_source_centrals
            and not (is_terminal and central_name in self._pmin_flowright_centrals)
        )
        gen_waterway: Optional[Waterway] = None
        if not use_builtin_turbine_waterway:
            gen_waterway = self._create_waterway(
                central_name + "_gen",
                central_id,
                central["ser_hid"],
                fmax=gen_fmax,
            )
        # Spill arc (`_ver`) configuration.
        #
        # Three regimes:
        #
        # 0. ``--drop-spillway-waterway`` (default False — opt-in;
        #    flipped from True after the 2026-04-28 gtopt_iplp
        #    investigation pinned the SDDP elastic-cut degeneracy at
        #    LMAULE/ELTORO on this regime).  When enabled, the entire
        #    ``_ver`` topology is suppressed — no waterway is created
        #    in either the in-network or synthetic-ocean path, no
        #    ``rebalse_cost``/``CVert`` fcost is attached, and the
        #    central's own junction is marked ``drain = True`` further
        #    down so the LP can shed any excess water through the
        #    junction instead of an explicit arc.  All spillover
        #    becomes a free loss to the ocean.  The default (False)
        #    falls through to one of the two PLP-faithful regimes
        #    below; flip with ``--drop-spillway-waterway`` only when
        #    LP scaling outweighs routing fidelity for the case at
        #    hand and the LMAULE-class degeneracy is not in play.
        #
        # 1. Reservoir IS in plpvrebemb.dat (``Costo de Rebalse`` defined).
        #    PLP exposes a stage-level ``qrb`` rebalse var (uncapped,
        #    costed at Costo de Rebalse) — its per-block ``qv_k`` is
        #    pinned to VertMax=0.  We model this *physically* in gtopt
        #    by leaving the per-block ``_ver`` arc open (fmax = None,
        #    so the default 300_000 m³/s sentinel applies) and putting
        #    the rebalse penalty directly on the waterway as ``fcost``.
        #    The reservoir's ``spillway_capacity`` is set to 0 in
        #    ``_spillway_fields`` to disable the redundant
        #    ``reservoir_drain`` teleport — water now follows the
        #    physical chain  storage → extraction → junction → _ver.
        #
        # 2. Reservoir is NOT in plpvrebemb.dat.  No stage-rebalse
        #    mechanism exists in PLP — per-block ``qv_k`` is bounded
        #    by VertMax from plpcnfce.dat (an explicit 0 is honoured;
        #    PLP "no limit" sentinels >= 9000 are dropped to unbounded
        #    via ``_is_plp_no_limit``).  The ``_ver`` arc carries no
        #    ``fcost``; cost (if any) lives on ``reservoir_drain`` via
        #    plpmat.dat's ``CVert`` fallback.
        # ``--auto-water-fail-cost`` (default on since 2026-05-11) installs
        # the unified water-shortfall pricing on Reservoir.efin_cost /
        # soft_emin_cost / FlowRight.fail_cost via WaterValueResolver.
        # Under that pipeline the vrebemb per-central ``Costo de Rebalse``
        # is redundant for the same reason ``cvert_default`` is: it's a
        # legacy symmetry-breaker priced in $0.01–$5000 units that sit
        # next to the much larger soft-storage anchors, widening the LP
        # coefficient range without changing the optimum.  Drop both the
        # vrebemb ``Costo de Rebalse`` (no longer read here — the drain is
        # priced at 0, see ``_spillway_fields``) and ``cvert_default``
        # (global path) so the ``_ver`` arc is free under auto pricing,
        # mirroring the new plexos2gtopt convention that Vert_* spillages
        # flow to an ocean drain at zero cost.  Membership in plpvrebemb.dat
        # NO LONGER drives the drain: every reservoir gets the storage-drain
        # enabled except the never-drain sentinels.
        #
        # ``drain_enabled``: True when this central carries its spill on the
        # reservoir storage-drain column (``Reservoir.spillway_cost = 0``,
        # see ``_spillway_fields``) rather than on a draining ``_ver`` arc.
        # That column only exists on ``embalse`` reservoirs, and ELTORO (a
        # ``_DRAIN_KILLED_RESERVOIRS`` sentinel) is excluded so it keeps its
        # ``_ver`` / ocean spill path.  A drained reservoir must NOT also
        # keep a draining ``_ver`` arc — that would be a double escape path
        # (free arbitrage) — so this flag re-keys the spill-arc suppression
        # below off the drain, not off vrebemb membership.
        # ``spills_via_waterway``: a reservoir with PLP ``VertMax > 0`` routes
        # its spill (vertimiento) through a PHYSICAL ``_ver`` waterway costed
        # at CVert — the same as PLP's ``qv`` arc — and its redundant storage-
        # drain is DISABLED (see ``_spillway_fields``) so water has exactly one
        # escape path (no double-escape arbitrage).  ``VertMax == 0`` (or
        # missing) keeps the legacy storage-drain model.  ELTORO is already
        # drain-less (``_DRAIN_KILLED_RESERVOIRS``) and keeps its own ``_ver`` /
        # ocean path, so it never enters this branch.
        spills_via_waterway = self._reservoir_spills_via_waterway(central)
        drain_enabled = (
            central_type == "embalse"
            and central_name.upper() not in self._DRAIN_KILLED_RESERVOIRS
            and not spills_via_waterway
        )
        # Global vert cost from plpmat.dat (``CVert`` in PLP) — the per-flow
        # spill penalty.  Two uses:
        #
        #  * ``cvert_default``: legacy symmetry-breaker on `_ver` arcs of
        #    non-drained reservoirs.  Under ``--auto-water-fail-cost`` (default
        #    on since 2026-05-11) the legitimate water-shortfall pricing lives
        #    on Reservoir.efin_cost / soft_emin_cost / FlowRight.fail_cost
        #    (anchored on the case's own falla.gcost); the $0.01 CVert
        #    coefficient sitting next to $500–$10 000 anchors is pure LP-kappa
        #    noise, so it is dropped in that mode.
        #  * ``cvert_spill``: when ``spills_via_waterway`` the spill rides a
        #    physical ``_ver`` waterway and PLP charges CVert per m³/s.  This
        #    cost is attached unconditionally (it is the only price on that
        #    arc, the drain being disabled), so it is NOT subject to the
        #    auto-water-fail-cost kappa-reduction above.
        cvert_global = 0.0
        if self.plpmat_parser is not None:
            cvert_global = getattr(self.plpmat_parser, "vert_cost", 0.0) or 0.0
        cvert_default: Optional[float] = None
        if cvert_global > 0.0 and not bool(self.options.get("auto_water_fail_cost")):
            cvert_default = cvert_global

        vert_max_raw = central.get("vert_max")
        vert_fmax: Optional[float]
        vert_fcost: Optional[float]
        if drain_enabled:
            # Drained reservoirs carry their spill on the reservoir
            # storage-drain column (``Reservoir.spillway_cost = 0``, see
            # ``_spillway_fields``) and emit no ``_ver`` waterway at all
            # (see the spill-arc selection below).  These bound values are
            # therefore unused for drained reservoirs — they are set only
            # so the variables are defined for the shared code path; the
            # actual spill cost / capacity live on the reservoir element.
            vert_fmax = None
            vert_fcost = None
        else:
            # PLP "no limit" sentinel — VertMax values ≥ 9000 m³/s mean
            # "unbounded" (often 9000 + central index, or 99999);
            # emitting them as literal upper bounds bloats LP matrix
            # kappa and can yield false binding-bound duals.  Drop them
            # to None so the field is omitted.
            if vert_max_raw is None:
                vert_fmax = None
            else:
                v = float(vert_max_raw)
                if _is_plp_no_limit(v):
                    self._plp_no_limit_count += 1
                    vert_fmax = None
                else:
                    vert_fmax = v
            vert_fcost = cvert_default
            if spills_via_waterway and cvert_global > 0.0:
                # VertMax>0 reservoir: the spill rides this physical ``_ver``
                # waterway (drain disabled) and PLP prices it at CVert.
                vert_fcost = cvert_global

        # PLP "no limit" sentinel applies to vert_min too — sentinel-encoded
        # minimum-spillage caps are nonsense and would over-constrain the
        # ``_ver`` arc.  Drop them to 0 (no forced minimum spill).
        vert_min_raw = float(central.get("vert_min", 0.0) or 0.0)
        if _is_plp_no_limit(vert_min_raw):
            self._plp_no_limit_count += 1
            vert_fmin = 0.0
        else:
            vert_fmin = vert_min_raw

        # **Synthetic ocean drain** — shared between the spill (`_ver`)
        # and gen (`_gen`) ocean-fallback paths below.  When BOTH
        # ``ser_hid == 0`` AND ``ser_ver == 0``, the two arcs would
        # historically each get their own drain junction (`_spill`
        # + `_ocean`), wasting one synthetic junction per terminal
        # central.  We track the first-created drain uid and reuse it
        # for the second path so the topology emits exactly one
        # ``<central>_ocean`` drain.  When only one path is terminal,
        # this still creates exactly one drain — same as before.
        synthetic_drain_uid: Optional[int] = None

        # **Source self-drain** — collapse the synthetic ``<central>_ocean``
        # node + spill arc into a drain stamped on the SOURCE junction.
        # When a central's spill/consumptive remainder exits to the ocean
        # (PLP ``ser_ver = 0`` / ``ser_hid = 0`` with a non-zero
        # spill / gen capacity to dispose of), the productive water already
        # leaves via the turbine / ``_ver → real downstream`` / pmin
        # flowrights; the ``_ver``/``_gen`` → ocean arc only ever absorbed
        # the surplus.  Rather than synthesise a ``<central>_ocean``
        # Junction (drain) and a connecting spill Waterway, we mark the
        # source junction itself ``drain = True`` and port the spill arc's
        # ``fmax`` / ``fcost`` onto its ``drain_capacity`` / ``drain_cost``
        # columns (see ``JunctionLP::add_to_lp``: ``uppb = drain_capacity``,
        # ``cost = drain_cost``; both default to +∞ / 0 on the C++ side, so
        # an unbounded spill arc omits ``drain_capacity`` entirely — no 1e30
        # sentinel).  This restores the pre-``5131f15f0`` junction self-drain
        # model, but only at these genuine spill-exit points.  ``None``
        # capacity/cost leave the field unset so the C++ default applies.
        force_source_drain = False
        source_drain_capacity: Optional[float] = None
        source_drain_cost: Optional[float] = None

        # ``--drop-spillway-waterway`` (default False — opt-in):
        # when enabled, do not emit any ``_ver`` waterway — neither
        # the in-network arc to ``ser_ver`` nor the synthetic-ocean
        # fallback.  The ``drain = True`` flag set on the central's
        # own junction below absorbs surplus water in place of the
        # missing arc.
        # ``seepage_covers_spillway``: set True only when the
        # default-branch (in-network ``_ver``) emission is suppressed
        # because a parallel ``filt_*`` seepage waterway covers the
        # same endpoints AND the ``_ver`` carries no extra constraint.
        # The ocean-fallback further down consults this flag to avoid
        # synthesising a new ``<central>_ocean`` arc as a substitute.
        seepage_covers_spillway = False
        # ``reservoir_spillway_covers_spill``: set True for every drained
        # reservoir (every ``embalse`` except the ``_DRAIN_KILLED_RESERVOIRS``
        # sentinels — see ``drain_enabled`` above).  For these the spill is
        # represented solely by the reservoir's own storage-drain column
        # (``Reservoir.spillway_cost = 0``, see ``_spillway_fields``) —
        # never by a draining ``_ver`` waterway.  Keeping BOTH would be a
        # double escape path (free arbitrage), so the redundant draining
        # ``_ver``→ocean/sink arc is dropped here.  When ``ser_ver > 0`` the
        # reservoir's ``spill_junction`` wires that drain column into the
        # downstream junction balance (PLP's ``qv`` chain), so no parallel
        # arc is needed.  This mirrors plexos2gtopt, which collapses
        # ``Vert_*`` spill waterways onto the reservoir/junction rather than
        # keeping draining arcs.  ELTORO (drain disabled) is excluded, so it
        # KEEPS its ``_ver`` / ocean spill path.  The ocean-fallback below
        # also consults this flag so it does not synthesise a substitute
        # ``<central>_ocean`` ``_ver`` arc.
        reservoir_spillway_covers_spill = drain_enabled
        if self._drop_spillway_waterway and not is_irrigation_diversion:
            # Irrigation diversions are excluded: their ``_ver`` arc carries
            # the river REMAINDER downstream (not a spill-to-sea), so it must
            # be preserved even under ``--drop-spillway-waterway``.  The
            # consumptive offtake is modelled by the FlowRight emitted below.
            ver_waterway: Optional[Waterway] = None
        elif reservoir_spillway_covers_spill:
            # Reservoir spillway/drain carries the spill — no ``_ver``
            # waterway (neither in-network nor synthetic-ocean).  The
            # legacy ``--vrebemb-as-sink`` redirect (route the vrebemb
            # ``_ver`` to a synthetic ocean drain) is subsumed by this:
            # vrebemb reservoirs no longer emit any ``_ver`` arc, so the
            # flag is now a no-op for its target population.
            ver_waterway = None
        else:
            # Suppress the ``_ver`` spillway arc when:
            #  - PLP's ``ser_ver`` target coincides with the seepage
            #    target for this central (pre-computed
            #    ``self._seepage_target_pairs`` from plpfilemb / plpcenfi
            #    BEFORE central processing), AND
            #  - the spillway arc would carry no LP-side constraint of
            #    its own (no fmin floor, no finite fmax, no fcost).
            # The seepage waterway emitted later by
            # ``_process_seepages_filemb`` / ``_process_seepages`` is
            # the PLP-faithful one (its piecewise volume function drives
            # the forced flow); a parallel free spillway adds no LP
            # information and gives the LP two indistinguishable
            # columns between the same junctions.  On the CEN65
            # 2-year case this catches ELTORO_ver_37_38 ⇄
            # filt_ELTORO_37_38 (both routing to ABANICO).
            ser_ver = int(central.get("ser_ver", 0) or 0)
            seepage_dup = (central_id, ser_ver) in self._seepage_target_pairs
            ver_unconstrained = (
                (vert_fmin or 0.0) <= 0.0
                and (vert_fmax is None or math.isinf(vert_fmax))
                and (vert_fcost is None or vert_fcost <= 0.0)
            )
            if seepage_dup and ver_unconstrained:
                self._seepage_redundant_spillway_count += 1
                self._seepage_redundant_spillway_names.append(
                    f"{central_name}_ver_{central_id}_{ser_ver}"
                )
                # The seepage waterway emitted later carries the
                # PLP-faithful forced flow for this central; the
                # ``seepage_covers_spillway`` flag (consumed by the
                # ocean-fallback below) prevents us from trading a
                # ``_ver → downstream`` arc for a ``_ver → ocean`` arc
                # + a synthetic ``<central>_ocean`` junction (net
                # WORSE topology).  The reservoir's ``efin_cost`` /
                # ``soft_emin_cost`` soft penalties under
                # ``--auto-water-fail-cost`` already provide the
                # storage-level safety valve that the ocean spillway
                # used to backstop.
                ver_waterway = None
                seepage_covers_spillway = True
            else:
                ver_waterway = self._create_waterway(
                    central_name + "_ver",
                    central_id,
                    central["ser_ver"],
                    vert_fmin,
                    vert_fmax,
                    fcost=vert_fcost,
                )

        # **Spillway ocean fallback** — when ``ser_ver = 0`` AND PLP
        # would route excess water "to the ocean", encode the spill
        # capacity + cost directly on the central's own junction via
        # the new ``drain_capacity`` / ``drain_cost`` fields instead
        # of synthesising a ``<central>_ocean`` Junction and a
        # connecting ``_ver`` Waterway.  Two encodings produce the
        # same LP — the per-block drain column on ``JunctionLP`` is
        # added with ``uppb = drain_capacity`` and ``cost =
        # drain_cost``, matching the legacy ``Waterway.fmax`` /
        # ``Waterway.fcost`` on the synthetic spillway arc — but the
        # junction-level form saves one Junction + one Waterway per
        # terminal central and removes the LMAULE-class unbounded-
        # drain risk because ``drain_capacity`` ports the PLP
        # ``VertMax`` cap onto the LP-side drain column directly.
        #
        # This branch is now reached only by NON-drained centrals
        # (serie/pasada, plus ELTORO and any other never-drain reservoir
        # sentinel).  Drained reservoirs carry their spill on the
        # storage-drain column instead and are gated out below via
        # ``not reservoir_spillway_covers_spill``.  The motivating PLP path
        # is ``VertMax > 0``: use ``drain_capacity = VertMax`` and the
        # ``CVert`` default cost on ``drain_cost`` — the per-block spill cap
        # matches PLP's plpcnfce VertMax field (original LA_HIGUERA case).
        if (
            ver_waterway is None
            and not self._drop_spillway_waterway
            and not seepage_covers_spillway
            and not reservoir_spillway_covers_spill
            and central_type in ("embalse", "serie", "pasada")
        ):
            # This branch is reached only for centrals whose spill is NOT
            # already carried by a reservoir storage-drain column
            # (``reservoir_spillway_covers_spill`` is False here):
            # drained reservoirs carry their spill on that column instead,
            # never on a ``_ver`` arc.  ELTORO (drain disabled) DOES reach
            # this branch, so its spill exit is collapsed onto its own
            # source-junction drain here.
            vert_max_for_spill = central.get("vert_max", 0.0) or 0.0
            spill_fmax: Optional[float] = None
            spill_fcost: Optional[float] = None
            if vert_max_for_spill > 0.0:
                # PLP "no limit" sentinel — VertMax ≥ 9000 m³/s means
                # "unbounded"; map to None so JunctionLP uses its +∞
                # default rather than baking the sentinel into the
                # LP upper bound and inflating coefficient kappa.
                if _is_plp_no_limit(float(vert_max_for_spill)):
                    self._plp_no_limit_count += 1
                    spill_fmax = math.inf
                else:
                    spill_fmax = float(vert_max_for_spill)
                spill_fcost = cvert_default
            if spill_fmax is not None:
                # **Collapse the spill-to-ocean into a source self-drain.**
                # The productive water already exits via this central's
                # turbine / ``_ver → real downstream`` / pmin flowrights;
                # the ``_ver → ocean`` arc only ever absorbed the
                # spill / consumptive remainder.  Rather than synthesise a
                # ``<central>_ocean`` Junction + a connecting ``_ver``
                # waterway, mark the source junction ``drain = True`` and
                # port the spill cap / cost onto its ``drain_capacity`` /
                # ``drain_cost`` columns (see ``JunctionLP::add_to_lp``).
                # An unbounded spill (``spill_fmax = +∞``) omits
                # ``drain_capacity`` so the C++ +∞ default applies — no
                # 1e30 sentinel — and a zero / missing cost omits
                # ``drain_cost``.  The source's other arcs (turbine,
                # ``_ver → downstream``, flowrights) are untouched.
                force_source_drain = True
                if math.isfinite(spill_fmax):
                    source_drain_capacity = spill_fmax
                if spill_fcost is not None and spill_fcost > 0.0:
                    source_drain_cost = spill_fcost

        # For embalse/serie/pasada centrals with ser_hid=0, the gen arc
        # has no PLP downstream junction.
        #
        # **Built-in Turbine waterway** (bus > 0 terminal turbines, e.g.
        # LA_HIGUERA): gtopt's ``Turbine.junction_a`` (with
        # ``junction_b`` unset) models the per-block water debit on the
        # central's own junction PLUS the power conversion in a single
        # primitive — no synthetic ``<central>_ocean`` Junction, no
        # ``<central>_gen`` Waterway needed.  Emitted further below.
        # Saves one Junction + one Waterway per terminal turbine
        # central.  Per-block ``Turbine.capacity`` carries the same
        # ``fmax = PotMax / Rendi`` cap the legacy gen Waterway had.
        #
        # **Legacy gen Waterway path** (bus == 0 transit-only or RoR
        # promotion): the gen Waterway carries per-stage plpmance
        # bounds via ``_transit_gen_waterways`` and is the reservoir's
        # ``junction_a`` anchor for ``--ror-as-reservoirs`` promotion.
        # Both rely on a concrete Waterway object to bind parquet refs
        # to, so we keep the ocean+gen_waterway pair for these cases.
        # ``use_builtin_turbine_waterway`` was resolved above (before the
        # gen Waterway creation) so eligible centrals never get a ``_gen``
        # arc in the first place.
        # A bus == 0 transit central with plpmance.dat per-stage flow
        # envelopes needs a concrete gen Waterway to bind
        # ``Waterway/fmin.parquet`` + ``Waterway/fmax.parquet`` refs
        # onto (see ``_write_transit_waterway_bounds``).  RoR-promotion
        # and pmin-flowright consumers likewise grep the gen Waterway by
        # name.  Only when NONE of these downstream consumers need the
        # gen Waterway can we collapse the ``_gen → ocean`` arc into a
        # source self-drain.
        gen_waterway_needed_downstream = (
            central_name in self._ror_reservoir_spec
            or central_name in self._pmin_flowright_centrals
            or (
                central.get("bus", 0) == 0
                and self.mance_parser is not None
                and self.mance_parser.get_mance_by_name(central_name) is not None
            )
        )
        if (
            central_type in ("embalse", "serie", "pasada")
            and gen_waterway is None
            and not use_builtin_turbine_waterway
            and not is_irrigation_diversion
        ):
            if not gen_waterway_needed_downstream:
                # **Collapse the gen-to-ocean into a source self-drain.**
                # No downstream consumer (transit plpmance bounds, RoR
                # promotion, pmin flowright) references this gen Waterway,
                # and the productive water already exits via the central's
                # other arcs (``_ver → real downstream``, flowrights).  The
                # ``_gen → ocean`` arc only ever absorbed the spill /
                # consumptive remainder, so mark the source junction
                # ``drain = True`` and port the gen flow cap onto its
                # ``drain_capacity`` column instead of synthesising a
                # ``<central>_ocean`` Junction + ``_gen`` Waterway.  An
                # unbounded gen flow (``gen_fmax is None``) omits
                # ``drain_capacity`` so the C++ +∞ default applies — no
                # 1e30 sentinel.
                force_source_drain = True
                if gen_fmax is not None and math.isfinite(gen_fmax):
                    if (
                        source_drain_capacity is None
                        or gen_fmax > source_drain_capacity
                    ):
                        source_drain_capacity = gen_fmax
                else:
                    # An unbounded gen path makes the source drain
                    # unbounded too — drop any finite cap a prior spill
                    # path may have set so the C++ +∞ default applies.
                    source_drain_capacity = None
            else:
                if synthetic_drain_uid is None:
                    self._ocean_junction_counter += 1
                    synthetic_drain_uid = (
                        _OCEAN_UID_OFFSET + self._ocean_junction_counter
                    )
                    ocean_name = f"{central_name}_ocean"
                    ocean_junction: Junction = {
                        "uid": synthetic_drain_uid,
                        "name": ocean_name,
                        "drain": True,
                    }
                    system["junction_array"].append(ocean_junction)
                    self._junction_names[synthetic_drain_uid] = ocean_name
                    _logger.debug(
                        "Created ocean drain junction '%s' (uid=%d) for "
                        "central '%s' — gen path",
                        ocean_name,
                        synthetic_drain_uid,
                        central_name,
                    )
                # Same `fmax = PotMax / Rendi` cap as the in-network gen
                # waterway path above — the ocean-drain branch handles
                # centrals with ``ser_hid = 0`` (no downstream PLP
                # central), so the synthetic ``<central>_ocean`` junction
                # becomes the gen-waterway target.  Without this cap the
                # waterway is unbounded and the LP can drain the upstream
                # source at any rate.  Symptom on juan/gtopt_iplp:
                # LA_HIGUERA (``ser_hid = 0``, ``ser_ver = 0``,
                # ``PotMax = 155``, ``Rendi = 3.12``) had a free gen
                # waterway, so the cascade-fix that removed the
                # spurious junction-drain exposed an unbounded gen path
                # at the same central.
                gen_waterway = self._create_waterway(
                    central_name + "_gen",
                    central_id,
                    synthetic_drain_uid,
                    fmax=gen_fmax,
                )

        # When a transit-only central (``bus = 0``, e.g. LMAULE,
        # B_LaMina) has plpmance.dat per-stage flow envelopes, wire
        # the per-stage bound onto the gen waterway: PLP fixes
        # ``qg<i>_b = PotMin = PotMax`` (forced flow) on these stages,
        # but gtopt has no generator entry to consume
        # Generator/pmin.parquet, so the parquet sat unused and the
        # waterway flowed freely up to the static cap.  We reuse the
        # already-extracted plpmance values, rekey them by gen
        # waterway uid, and emit Waterway/fmin.parquet +
        # Waterway/fmax.parquet (see ``_write_transit_waterway_bounds``).
        # Placed here so both gen-waterway creation paths (in-network
        # and the ``ser_hid = 0`` ocean fallback) are covered.
        if (
            gen_waterway is not None
            and central.get("bus", 0) == 0
            and self.mance_parser is not None
            and self.mance_parser.get_mance_by_name(central_name) is not None
        ):
            # Register only.  ``fmin``/``fmax`` string refs are set
            # later by ``_write_transit_waterway_bounds`` for the
            # subset whose parquet columns actually survive the
            # static-fill drop in ``ManceWriter._create_dataframe``.
            self._transit_gen_waterways.append(
                (central_id, gen_waterway["uid"], central_name, gen_waterway)
            )

        # Add waterways if they exist
        if gen_waterway:
            system["waterway_array"].append(gen_waterway)
            if central["bus"] > 0:  # Only create turbine if connected to bus
                # When the central is listed in the RoR-as-reservoirs CSV,
                # the authoritative turbine production factor comes from
                # that file — PLP efficiency may be a 1.0 placeholder.
                ror_spec = self._ror_reservoir_spec.get(central_name)
                production_factor = (
                    ror_spec.production_factor
                    if ror_spec is not None
                    else central["efficiency"]
                )
                turbine: Turbine = {
                    "uid": central_id,
                    "name": central_name,
                    "generator": central_name,
                    "waterway": gen_waterway["name"],
                    "production_factor": production_factor,
                }
                system["turbine_array"].append(turbine)
        elif use_builtin_turbine_waterway:
            # Built-in Turbine waterway path — Turbine.junction_a on the
            # central's own junction with ``junction_b`` unset models a
            # terminal drain natively.  Per-block ``capacity`` carries
            # the legacy gen-Waterway ``fmax = PotMax / Rendi`` cap.
            turbine_builtin: Turbine = {
                "uid": central_id,
                "name": central_name,
                "generator": central_name,
                "junction_a": central_name,
                "production_factor": central["efficiency"],
            }
            # ser_hid > 0 → wire the downstream junction onto junction_b
            # (mirrors the legacy ``_gen`` Waterway).  ser_hid = 0 leaves
            # junction_b unset: a terminal run-to-sea plant.
            ser_hid = int(central.get("ser_hid", 0) or 0)
            if ser_hid > 0:
                turbine_builtin["junction_b"] = self._junction_names.get(
                    ser_hid, str(ser_hid)
                )
            # Do NOT bound the turbine flow with gen_fmax = PotMax / Rendi:
            # the generator pmax already implies it via the conversion row
            # (gen = rate * flow  =>  flow <= pmax / rate), so it is redundant.
            # Leaving the flow unbounded (like a regular waterway flow) lets
            # gtopt presolve substitute the turbine_flow column out of the
            # junction balances.  A genuine input limit (e.g. the
            # volume-dependent plpcenpmax discharge curve) is still applied
            # separately as Turbine.capacity in the cenpmax path.
            system["turbine_array"].append(turbine_builtin)
            self._builtin_turbine_waterway_count += 1

        if ver_waterway:
            system["waterway_array"].append(ver_waterway)

        # Whether this central will be promoted to a daily-cycle reservoir
        # by the --ror-as-reservoirs feature (used below to emit the
        # reservoir record).  Eligibility: generation waterway exists, bus
        # is connected, and the central name is in the whitelist CSV.
        will_promote_ror = (
            gen_waterway is not None
            and central["bus"] > 0
            and central_name in self._ror_reservoir_spec
        )

        # Drain logic:
        # ``drain = True`` makes the central junction a system sink —
        # water can leave gtopt's network with no downstream balance,
        # which is the wrong default for embalse / serie / pasada
        # centrals that have a real generation outlet (``gen_waterway``)
        # OR a real spillway (``ver_waterway``).  PLP enforces volume
        # balance through the central's outlets only — there is no
        # implicit "to-sea" sink unless the central genuinely has no
        # outlet at all.
        #
        # **Embalse without ``ver_waterway``** (the previous form, set
        # ``drain = True`` for any embalse with ``ser_ver = 0``)
        # silently created a free water-escape valve on every embalse
        # whose spillway target is the sea.  Symptom on
        # juan/gtopt_iplp: LMAULE (gen_waterway → LOS_CONDORES,
        # ``ser_ver = 0``) was drained from 657 Hm³ to 0 in p1 by
        # sending all the storage out the LMAULE-junction drain at
        # zero cost, while PLP (no such drain) had to keep LMAULE
        # 115-758 Hm³ all year.  The cascade-infeasibility chain at
        # p27/p28 collapsed once this drain was removed.
        #
        # New rule (mass conservation): a reservoir / RoR / pass-through
        # junction NEVER carries ``drain = True`` — water there is either
        # stored, turbined, or spilled downstream, but never lost.  The
        # only junctions that may drain are:
        #   * a genuine spill-exit source junction (PLP ``ser_ver = 0`` /
        #     ``ser_hid = 0`` with a real spill/gen capacity to dispose of
        #     and NO downstream consumer of the gen Waterway) — the spill /
        #     consumptive remainder is shed through the source's own drain
        #     column (``force_source_drain`` above; capacity / cost ported
        #     onto ``drain_capacity`` / ``drain_cost``), or
        #   * the central's own junction ONLY when it has no physical
        #     outlet at all (``gen_waterway is None and
        #     ver_waterway is None``) — a true topological dead-end.
        #
        # The spillway-to-sea case (PLP ``ser_ver = 0`` with a non-zero
        # ``VertMax`` / vrebemb rebalse) is handled by the ocean-fallback
        # above, which now stamps ``drain = True`` on the source junction
        # itself (carrying ``spill_fmax`` / ``spill_fcost`` on its drain
        # column).  The central's productive outlets (turbine, ``_ver →
        # real downstream``, flowrights) stay on their own arcs — the LP
        # cannot teleport stored water out, only the surplus drains.
        #
        # ``--drop-spillway-waterway`` (opt-in, default False since the
        # 2026-04-28 LMAULE / ELTORO fix): the spillway arc has been
        # suppressed above so the central's own junction must absorb
        # any surplus water itself.  Force ``drain = True`` for the
        # embalse / serie / pasada types that previously got a
        # ``_ver`` arc.
        if (
            self._drop_spillway_waterway
            and central_type
            in (
                "embalse",
                "serie",
                "pasada",
            )
            and not is_irrigation_diversion
        ):
            # Irrigation diversions keep ``drain = False``: their surplus
            # returns downstream via the preserved ``_ver`` arc (above) and
            # the consumptive offtake is the FlowRight (below).  Forcing a
            # drain here would double-count the diversion and trip the
            # assertion in the irrigation-diversion block.
            drain = True
        elif force_source_drain:
            # The spill / consumptive remainder that used to flow out a
            # synthetic ``<central>_ocean`` arc is now shed through this
            # source junction's own drain column (capacity / cost ported
            # onto ``drain_capacity`` / ``drain_cost`` below).  The
            # central's productive outlets (turbine, ``_ver → real
            # downstream``, flowrights) remain on their own arcs.
            drain = True
        elif use_builtin_turbine_waterway:
            # Built-in Turbine waterway IS the central's physical
            # outlet (debits this junction every block).  Stamping
            # ``drain = True`` here would add a free escape valve on
            # top of the turbine — water could leave at zero cost
            # without producing power.  Keep ``drain = False`` so the
            # turbine's per-block capacity remains the binding cap.
            drain = False
        else:
            drain = gen_waterway is None and ver_waterway is None
        junction: Junction = {
            "uid": central_id,
            "name": central_name,
            "drain": drain,
        }
        # Source self-drain spill cap / cost (see ``force_source_drain``):
        # port the collapsed spill arc's ``fmax`` / ``fcost`` onto the
        # junction's drain column.  Omitted fields fall back to the C++
        # defaults (+∞ capacity / 0 cost in ``JunctionLP::add_to_lp``);
        # an unbounded spill therefore carries NO ``drain_capacity`` —
        # never a 1e30 sentinel.
        if force_source_drain:
            if source_drain_capacity is not None:
                junction["drain_capacity"] = source_drain_capacity
            if source_drain_cost is not None:
                junction["drain_cost"] = source_drain_cost
        system["junction_array"].append(junction)

        # **Irrigation-diversion FlowRight** — replace the gated-off free
        # gen-path drain (see ``is_irrigation_diversion`` above) with a
        # CONSUMPTIVE FlowRight (``direction = -1``) on the central's own
        # junction: water diverted for irrigation leaves the basin here while
        # the ``_ver`` arc returns the remainder downstream.  Pure
        # representation change — no irrigation *requirement* is invented.
        # ``fmin = 0`` (never forced); ``fmax`` ports the former gen-path
        # drain capacity (``PotMax / Rendi``) when finite, else unbounded;
        # ``fcost`` reuses the soft-flow-right fail cost.
        if is_irrigation_diversion:
            # ``drain == False`` is guaranteed (the gen-path drain was gated
            # off and the ``_ver`` arc exists); assert it so a future refactor
            # re-enabling the drain cannot silently double-count the offtake.
            assert not drain, (
                f"irrigation-diversion central {central_name!r} unexpectedly "
                "kept drain=True"
            )
            fail_cost = resolve_flow_right_fail_cost(
                self.options, self.plpmat_parser, self.vrebemb_parser
            )
            flow_right: Dict[str, Any] = {
                "uid": _IRRIGATION_DIVERSION_UID_OFFSET + int(central_id),
                "name": f"{central_name}_irrigation_right",
                "purpose": _IRRIGATION_DIVERSION_PURPOSE,
                "junction_a": central_name,
                "consumptive": True,
                "direction": _FLOW_RIGHT_DIRECTION,
                "fmin": 0.0,
                "fcost": fail_cost,
            }
            if gen_fmax is not None and math.isfinite(gen_fmax):
                flow_right["fmax"] = float(gen_fmax)
            system["flow_right_array"].append(flow_right)
            _logger.info(
                "irrigation-diversion: emitted consumptive FlowRight '%s' "
                "(uid=%d, junction_a=%s, fmax=%s, fcost=%g) and disabled the "
                "free gen-path source drain for serie central '%s' "
                "(bus=0, ser_hid=0, ser_ver=%d)",
                flow_right["name"],
                flow_right["uid"],
                central_name,
                flow_right.get("fmax", "unbounded"),
                fail_cost,
                central_name,
                int(central.get("ser_ver", 0) or 0),
            )

        # Promote to a daily-cycle reservoir when the central appears in
        # the --ror-as-reservoirs whitelist.  Eligibility was already
        # enforced by _load_ror_reservoir_spec (pasada/serie only, bus>0,
        # efficiency>0, and the name must be in the CSV).  We only emit
        # the reservoir when a generation waterway exists so the turbine
        # can drain it; otherwise the promotion is a no-op for this
        # central.  The reservoir sits on ``central_name`` junction, which
        # is ``gen_waterway.junction_a`` — i.e. the **upstream** endpoint
        # of the turbine's generation waterway, representing the local
        # daily pondage at the plant intake.
        if will_promote_ror:
            vmax = self._ror_reservoir_spec[central_name].vmax_hm3
            # Daily-cycle reservoirs omit the embalse-specific fields
            # (eini/efin/fmin/fmax/flow_conversion_rate/spillway_*) — the
            # C++ schema supplies sensible defaults for a daily-cycle
            # tank.  Cast out of the strict Reservoir TypedDict to match
            # the battery_writer.get_regulation_reservoirs() pattern.
            ror_reservoir: Dict[str, Any] = {
                "uid": central_id,
                "name": central_name,
                "junction": central_name,
                "emin": 0.0,
                "emax": vmax,
                "capacity": vmax,
                "daily_cycle": True,
            }
            system["reservoir_array"].append(cast(Reservoir, ror_reservoir))
            _logger.debug(
                "Promoted %s central '%s' to daily-cycle reservoir "
                "(vmax=%g hm3, prod_factor=%g MW/(m3/s))",
                central_type,
                central_name,
                vmax,
                self._ror_reservoir_spec[central_name].production_factor,
            )

        # Add flow if exists
        afluent = self._get_central_flow(central_name, central)
        if isinstance(afluent, float) and afluent == 0.0:
            return

        flow: Flow = {
            "uid": central_id,
            "name": central_name,
            "junction": central_name,
            "discharge": afluent,
        }
        system["flow_array"].append(flow)

    def _get_central_flow(
        self, central_name: str, central: Dict[str, Any]
    ) -> float | str:
        """Get flow value for central, checking aflce parser if available.

        When the aflce parser has data for this central, returns the string
        ``"discharge"`` which tells the C++ ``FlowLP`` (with
        ``ClassName = {"Flow", "flw"}``) to read from
        ``{input_directory}/Flow/discharge.parquet``.
        """
        if self.aflce_parser:
            aflce = self.aflce_parser.get_item_by_name(central_name)
            if aflce is not None:
                # Only emit the parquet reference when the central's column
                # actually survived AflceWriter's sparsity filter (a column
                # whose flow is constant-equal-to-afluent across all active
                # scenarios is dropped to keep the parquet compact, in which
                # case the scalar `afluent` is the faithful equivalent).
                # `process_afluents` stashes the kept uid set under
                # ``options["_aflce_emitted_uids"]``; falling back to scalar
                # avoids the dangling-reference crash:
                #   ``Can't find element 'NAME:<uid>' in table 'discharge'``
                emitted = self.options.get("_aflce_emitted_uids")
                if emitted is None:
                    return "discharge"
                uid = central.get("number")
                if uid is not None and int(uid) in emitted:
                    return "discharge"
                # Column filtered out — emit the scalar afluent (which is the
                # constant the filter detected) so the LP sees the same value.
        return central.get("afluent", 0.0)

    def _load_ror_reservoir_spec(
        self, items: List[Dict[str, Any]]
    ) -> Dict[str, RorSpec]:
        """Resolve ``--ror-as-reservoirs`` selection against the CSV whitelist.

        Delegates to :func:`ror_equivalence_parser.resolve_ror_reservoir_spec`.
        When the caller has already resolved the spec (e.g. ``gtopt_writer``
        resolves it once before running ``process_afluents`` so the
        pasada-unscale map can be applied to the discharge parquet), it is
        passed in via ``options["_ror_spec_resolved"]`` and re-used here
        verbatim to avoid re-parsing the CSV.
        """
        from .ror_equivalence_parser import (  # noqa: PLC0415
            resolve_ror_reservoir_spec,
        )

        pre_resolved = self.options.get("_ror_spec_resolved")
        if pre_resolved is not None:
            return dict(pre_resolved)
        return resolve_ror_reservoir_spec(self.options, items)

    def _process_extractions(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process extraction centrals into waterways."""
        if not self.extrac_parser:
            return
        for i, extraction in enumerate(self.extrac_parser.extracs):
            upstream_name = extraction["name"]
            upstream_central = central_parser.get_central_by_name(upstream_name)
            if not upstream_central:
                _logger.warning(
                    "Upstream central '%s' not found in central parser.",
                    upstream_name,
                )
                continue

            downstream_name = extraction["downstream"]
            downstream_central = central_parser.get_central_by_name(downstream_name)
            if not downstream_central:
                _logger.warning(
                    "Downstream central '%s' not found for extraction '%s'.",
                    downstream_name,
                    upstream_name,
                )

                continue  # Skip invalid downstream

            waterway = self._create_waterway(
                upstream_name + "_extrac_" + str(i),
                upstream_central["number"],
                downstream_central["number"],
                fmin=0.0,
                fmax=extraction.get("max_extrac", 0.0),
            )
            if waterway:
                system["waterway_array"].append(waterway)

    def _process_reservoirs(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
        parquet_cols,
    ) -> None:
        """Process reservoir centrals into reservoir elements."""
        reservoirs = central_parser.centrals_of_type.get("embalse", [])
        for central in reservoirs:
            central_name = central["name"]
            central_number = central["number"]
            pcol_name = self.pcol_name(central_name, central_number)

            emin = "emin" if pcol_name in parquet_cols["emin"] else central["emin"]
            emax = "emax" if pcol_name in parquet_cols["emax"] else central["emax"]

            # PLP-style spill routing: when `SerVer > 0` the reservoir's
            # vertimiento flows to the SerVer-named central's junction.
            # Set the optional `spill_junction` so gtopt's ReservoirLP wires
            # the drain column into that downstream junction's balance row
            # (matches PLP's `qv` chain).  When SerVer == 0 (drain to sea)
            # leave it unset — drain stays a pure storage sink.
            ser_ver = central.get("ser_ver", 0)
            spill_junction_name = self._junction_names.get(ser_ver) if ser_ver else None

            # VertMax>0 reservoirs route spill on a physical ``_ver`` waterway
            # (emitted in ``_process_central``, costed at CVert) and have their
            # storage-drain DISABLED here — keeping both would be a double
            # escape path.  The ``_ver`` arc already wires the spill to the
            # ser_ver downstream junction / ocean, so ``spill_junction`` (which
            # only routes the now-absent drain column) is left unset.
            spills_via_waterway = self._reservoir_spills_via_waterway(central)

            reservoir: Reservoir = {
                "uid": central["number"],
                "name": central["name"],
                "junction": central["name"],
                "eini": central["vol_ini"],
                "efin": central["vol_fin"],
                "emin": emin,
                "emax": emax,
                "capacity": central["emax"],
                # Extraction (qe) bounds are intentionally OMITTED — same as
                # plexos2gtopt.  PLP's `qe` (storage flow-balance) is
                # *unbounded* by default (LeeQeBnd: ±DINFTY), but emitting the
                # ±1e30 effective-infinity sentinel here made the C++
                # ReservoirLP use it verbatim → a FREE / unbounded extraction
                # column that blocks GPU first-order / heuristic LP solvers
                # (cuOpt PDLP / feasibility-jump cannot project it) and trips
                # cuOpt's "Free variable found" presolve warning.  By leaving
                # `fmin`/`fmax` unset the reservoir flow estimator
                # (gtopt_shared.reservoir_flow, run as the converter's final
                # pass) fills CONCRETE topology-derived bounds; if that pass
                # is disabled, the C++ ReservoirLP default (−9000 / 6000 m³/s)
                # applies — both finite, neither restricting feasibility (the
                # binding constraint is the energy box [emin, emax]).
                # Spillway cost & capacity (see ``_spillway_fields``):
                # ─ Reservoir is NOT a never-drain sentinel → activate the
                #   reservoir spillway/drain: emit ``spillway_cost = 0.0``
                #   (free) and omit ``spillway_capacity`` (C++ default
                #   +6000 m³/s, drain enabled).  The spill is carried
                #   entirely by this reservoir-drain column — no separate
                #   ``_ver`` waterway is emitted for the reservoir.
                # ─ Reservoir IS a never-drain sentinel (``ELTORO``) → no
                #   spillway/drain at all (both fields omitted, C++ drain
                #   gate closed); its spill leaves via the modelled
                #   ``_ver`` / ocean topology instead.
                **self._spillway_fields(
                    central_name, drain_disabled=spills_via_waterway
                ),
                "flow_conversion_rate": 3.6 / 1000.0,
            }
            if spill_junction_name is not None and not spills_via_waterway:
                reservoir["spill_junction"] = spill_junction_name

            # Energy scaling mode: energy scale for LP variables is now handled
            # exclusively via the ``variable_scales`` option in the planning
            # options section (written by GTOptWriter.process_variable_scales).
            # Do NOT emit energy_scale or energy_scale_mode on the reservoir.

            # Small / independent reservoirs (PLP ``Hid_Indep='T'``)
            # do not carry state across stages — they are run-of-
            # river-style devices that PLP buffers within the day.
            #
            # Two translation regimes:
            #
            #   1. ``--plp-legacy``: literal PLP behaviour — drop only
            #      the inter-stage state link by emitting
            #      ``use_state_variable = False``.  The per-stage
            #      energy balance is kept (sini/efin free in
            #      ``[emin, emax]`` plus a ``efin = sini`` close row)
            #      and matches PLP's per-stage LP shape.  Useful for
            #      bit-for-bit PLP comparison work.
            #
            #   2. Default (no ``--plp-legacy``): emit
            #      ``daily_cycle = True``.  The C++ ``StorageLP``
            #      then applies ``dc_stage_scale = 24/stage_duration``
            #      to the energy-balance coefficients, scaling the
            #      per-block accumulation down to a 24 h equivalent.
            #      For monthly stages that means dividing the
            #      affluent contribution by ~30, which lets reservoirs
            #      whose ``Afluen × stage_duration`` would otherwise
            #      exceed the ``[emin, emax]`` box close cleanly
            #      within a single stage.  Symptom on juan/gtopt_iplp:
            #      CANUTILLAR (Afluen=126.3, PotMax/Rendi=85.1,
            #      VertMax=0, Hid_Indep=T) had no spill or
            #      accumulation path → p1 LP infeasible.
            #      ``daily_cycle = True`` makes the per-stage balance
            #      satisfiable without changing the LP topology.
            #      ``StorageOptions`` forces
            #      ``use_state_variable = False`` whenever
            #      ``daily_cycle`` is true, so callers don't have to
            #      pin both fields.
            if central.get("hid_indep", False):
                if self.options.get("plp_legacy", False):
                    reservoir["use_state_variable"] = False
                else:
                    reservoir["daily_cycle"] = True

            # Soft minimum volume (plpminembh.dat "holgura" / slack)
            self._apply_soft_emin(reservoir, central_name)

            # ``--soft-storage-bounds``: relax the per-reservoir efin row
            # and route reservoir maintenance (plpmanem.dat) emin into
            # the soft_emin slack mechanism, priced at the same per-
            # reservoir cost (plpvrebemb / CVert / fallback).
            self._apply_soft_storage_bounds(reservoir, central_name, parquet_cols)

            system["reservoir_array"].append(reservoir)

    def _reservoir_spills_via_waterway(self, central: Dict[str, Any]) -> bool:
        """True when a reservoir routes spill through a physical ``_ver`` arc.

        Per the PLP-faithful convention (user-confirmed): an ``embalse``
        reservoir with PLP ``VertMax > 0`` (spill allowed) gets a real
        spillway ``_ver`` waterway costed at CVert and has its redundant
        storage-drain DISABLED — so water has exactly one escape path
        (no double-escape arbitrage).  ``VertMax == 0`` / missing keeps the
        legacy storage-drain model.  ELTORO (``_DRAIN_KILLED_RESERVOIRS``) is
        already drain-less and keeps its own ``_ver`` / ocean path, so it is
        excluded here (its spill handling is unchanged).
        """
        if str(central.get("type", "")) != "embalse":
            return False
        if str(central.get("name", "")).upper() in self._DRAIN_KILLED_RESERVOIRS:
            return False
        vert_max = central.get("vert_max")
        return vert_max is not None and float(vert_max) > 0.0

    def _spillway_fields(
        self, central_name: str, *, drain_disabled: bool = False
    ) -> "_SpillwayFields":
        """Compute ``spillway_cost`` / ``spillway_capacity`` for one reservoir.

        EVERY reservoir gets its storage-drain (spillway) column ENABLED
        EXCEPT the never-drain sentinels in :pyattr:`_DRAIN_KILLED_RESERVOIRS`
        (currently ``ELTORO``) and reservoirs whose spill is carried by a
        physical ``_ver`` waterway (``drain_disabled=True``, set by
        ``_process_reservoirs`` for PLP ``VertMax > 0`` reservoirs — see
        :meth:`_reservoir_spills_via_waterway`).  This mirrors plexos2gtopt's
        ``build_reservoir_array`` spill block.  The translation is:

        - **Reservoir is NOT a never-drain sentinel** → *activate the
          reservoir spillway/drain* at zero cost.  We emit
          ``spillway_cost = 0.0`` (so the C++ ``StorageBase::add_to_lp``
          drain gate ``drain_cost.has_value()`` is satisfied) and **omit**
          ``spillway_capacity`` so ``ReservoirLP::spillway_capacity()``
          applies its finite ``+6000 m³/s`` default.  The gate's other
          half, ``drain_capacity.value_or(1.0) > 0``, is therefore
          satisfied and a *finite, free* per-block drain column is added
          to the energy balance.  The cost is **0**, not the vrebemb
          ``Costo de Rebalse``: the spill leaves the basin, and a non-zero
          drain cost generates spurious negative marginal prices.  No
          separate draining ``_ver`` waterway is emitted for these
          reservoirs (see ``_process_central``): the spill is represented
          solely by this reservoir-spillway column.  When ``ser_ver > 0``
          the reservoir's ``spill_junction`` (set in
          ``_process_reservoirs``) wires the drain column into the
          downstream junction balance, reproducing PLP's ``qv`` chain
          without a parallel arc.

          We do **not** re-introduce the ``1e30`` spillway_capacity
          sentinel here — it was removed as a free-column source — nor
          do we pin ``spillway_capacity = 0`` (which would *disable* the
          drain via the C++ gate).  Omitting the key is the minimal set
          of fields that activates a finite, free spill.

        - **Reservoir IS a never-drain sentinel** (``ELTORO``) → it gets
          *no* spillway/drain.  We return ``{}`` (both fields omitted), so
          ``drain_cost`` is unset and the C++ drain gate stays closed — no
          drain column.  Its spill must instead leave through the modelled
          topology (turbine / ``_ver`` arc / ocean route), which is kept
          intact by ``_process_central``.  This makes ELTORO a
          double-escape / mass-leak correctness sentinel.
        """
        if central_name.upper() in self._DRAIN_KILLED_RESERVOIRS or drain_disabled:
            # Never-drain sentinel (ELTORO), or a VertMax>0 reservoir whose
            # spill rides a physical ``_ver`` waterway → no spillway / drain
            # column at all.  The spill path is preserved in
            # ``_process_central`` so the water still has somewhere to go.
            return {}

        # Every other reservoir → activate the storage-drain at zero cost.
        # ``spillway_capacity`` is intentionally omitted so the C++
        # ReservoirLP applies its finite +6000 m³/s default (drain
        # enabled, no 1e30 sentinel).  Cost is 0 (free): a non-zero drain
        # cost produces spurious negative marginal prices.
        return {"spillway_cost": 0.0}

    def _apply_soft_emin(self, reservoir: Reservoir, central_name: str) -> None:
        """Add soft_emin and soft_emin_cost from plpminembh.dat if available.

        Builds per-stage arrays from the sparse minembh data.  Stages without
        data get 0 (no constraint).  The cost defaults to the CLI option
        ``--soft-emin-cost`` (default 0.1) when the file cost is zero.
        """
        if not self.minembh_parser or not self.stage_parser:
            return

        entry = self.minembh_parser.get_minembh_by_name(central_name)
        if entry is None:
            return

        num_stages = self.stage_parser.num_stages
        default_cost = self.options.get("soft_emin_cost", 0.1) if self.options else 0.1
        if default_cost <= 0:
            return

        # Build per-stage arrays (0-indexed); minembh stages are 1-based.
        soft_emin_arr = [0.0] * num_stages
        soft_cost_arr = [0.0] * num_stages

        stages = entry["stage"]  # numpy int32 array, 1-based
        vmins = entry["vmin"]  # numpy float64 array [dam³]
        costs = entry["cost"]  # numpy float64 array [$/dam³]

        for i, stage_num in enumerate(stages):
            idx = int(stage_num) - 1  # convert to 0-based
            if 0 <= idx < num_stages and float(vmins[i]) > 0:
                soft_emin_arr[idx] = float(vmins[i])
                file_cost = float(costs[i])
                soft_cost_arr[idx] = file_cost if file_cost > 0 else default_cost

        # Only add if there's at least one non-zero soft_emin
        if any(v > 0 for v in soft_emin_arr):
            reservoir["soft_emin"] = soft_emin_arr
            reservoir["soft_emin_cost"] = soft_cost_arr

    def _resolve_storage_bound_cost(self, central_name: str) -> float:
        """Per-reservoir penalty cost for soft efin / soft_emin slacks.

        Preference order (matches PLP's spill cost cascade):

        1. ``plpvrebemb.dat`` — explicit ``Costo de Rebalse`` for this
           reservoir.
        2. ``plpmat.dat`` — global ``CVert`` (when > 0).
        3. ``soft_emin_cost`` CLI default (``--soft-emin-cost``).
        4. Hard fallback ``1000.0`` so the slack is never priced at 0.

        After resolution the value is clamped at ``vert_cost_cap`` (CLI
        ``--vert-cost-cap``, default 500.0).  Real PLP cases sometimes
        carry vrebemb costs of 5000 \\$/hm³ which dominate the SDDP
        objective on iter-0 forward passes and produce an enormous UB
        until enough Benders cuts steer the trajectory; capping the
        per-slack price lets the gap close in fewer iterations at the
        cost of allowing slightly more spillage in the LP optimum.
        Set ``--vert-cost-cap=0`` to disable the cap.
        """
        cap = self.options.get("vert_cost_cap", 0.0) if self.options else 0.0
        cap = float(cap) if cap and cap > 0 else 0.0

        def _capped(value: float) -> float:
            return min(value, cap) if cap > 0 else value

        if self.vrebemb_parser is not None:
            cost = self.vrebemb_parser.get_cost(central_name)
            if cost is not None and cost > 0:
                return _capped(float(cost))

        if self.plpmat_parser is not None:
            cvert = getattr(self.plpmat_parser, "vert_cost", 0.0) or 0.0
            if cvert > 0:
                return _capped(float(cvert))

        cli_cost = self.options.get("soft_emin_cost", 0.0) if self.options else 0.0
        if cli_cost and cli_cost > 0:
            return _capped(float(cli_cost))

        return _capped(1000.0)

    def _apply_soft_storage_bounds(
        self,
        reservoir: Reservoir,
        central_name: str,
        parquet_cols: Dict[str, List[str]],
    ) -> None:
        """Relax per-reservoir efin and maintenance-emin to soft slacks.

        Gated by ``options["soft_storage_bounds"]`` (default true; also
        forced on by ``--plp-legacy``).  When enabled:

        - If the reservoir has a non-trivial ``efin``, set
          ``efin_cost`` so the hard ``vol_end >= efin`` row becomes
          ``vol_end + slack >= efin`` priced at ``efin_cost``.
        - If reservoir maintenance (plpmanem.dat) populated a per-stage
          ``emin`` schedule for this reservoir AND ``soft_emin`` was
          not already set by ``_apply_soft_emin`` (plpminembh.dat),
          route the maintenance schedule into ``soft_emin`` priced at
          the same per-reservoir cost, and replace the hard ``emin``
          field with the static box floor (``central["emin"]``).

        Both costs share ``_resolve_storage_bound_cost`` (vrebemb →
        CVert → CLI → fallback).
        """
        # Default ON when the key is absent — matches the CLI default
        # (see ``--soft-storage-bounds`` / ``--no-soft-storage-bounds`` in
        # ``_parsers.py``).  Programmatic callers (``convert_plp_case``)
        # opt out by setting ``soft_storage_bounds=False``.
        if self.options is not None and not self.options.get(
            "soft_storage_bounds", True
        ):
            return

        # Resolve the per-reservoir slack price.  When the auto helper
        # is enabled, derive the price from the central's cascade lost
        # production factor (energy-equivalent of stored water) so the
        # slack is strictly above any electric demand-fail price.
        # Otherwise fall back to the legacy vrebemb / CVert / CLI
        # cascade.  ``central_number`` is needed both for the auto
        # helper and the manem reroute below; resolve it once.
        central_number: Optional[int] = None
        if self.central_parser is not None:
            central_number = next(
                (
                    c["number"]
                    for c in self.central_parser.centrals_of_type.get("embalse", [])
                    if c["name"] == central_name
                ),
                None,
            )

        if (
            self._water_value_resolver is not None
            and self._water_value_resolver.is_active
            and central_number is not None
        ):
            lost_pf = self._water_value_resolver.cascade_lost_pf(central_number)
            cost = self._water_value_resolver.efin_cost_for(central_name, lost_pf)
        else:
            cost = self._resolve_storage_bound_cost(central_name)

        # ─── efin → soft via efin_cost ──────────────────────────────────
        # The terminal soft cost is the reservoir's energy-production-factor
        # estimate (``anchor × cascade_lost_pf``) UNLESS boundary cuts are
        # available for this reservoir, in which case
        # ``WaterValueResolver.efin_cost_for`` OVERWRITES it with the cut
        # lower-bound water value (see ``_build_cut_water_values``).  The
        # overwrite is now done here in Python (the legacy C++
        # ``apply_boundary_cut_soft_costs`` pass was retired): no-cut
        # reservoirs keep the EPF estimate, cut reservoirs use the
        # SDDP-revealed marginal water value.
        efin = reservoir.get("efin")
        if efin is not None and efin > 0:
            reservoir["efin_cost"] = cost

        # ─── maintenance emin → soft_emin (if not already populated) ────
        # The hard ``emin`` field currently holds the parquet-schedule
        # column name (``"emin"``) when manem data exists for this
        # reservoir.  Detect that and reroute.
        if reservoir.get("soft_emin") is not None:
            return  # already set by plpminembh — leave it alone

        # Look up the reservoir's per-stage manem emin schedule.
        if (
            self.manem_parser is None
            or self.central_parser is None
            or self.stage_parser is None
        ):
            return

        # ``central_number`` was resolved at the top of this method.
        if central_number is None:
            return
        pcol_name = self.pcol_name(central_name, central_number)
        if pcol_name not in parquet_cols.get("emin", []):
            return  # no manem data for this reservoir

        entry = self.manem_parser.get_manem_by_name(central_name)
        if entry is None:
            return

        num_stages = self.stage_parser.num_stages
        soft_emin_arr = [0.0] * num_stages
        soft_cost_arr = [0.0] * num_stages
        stages = entry["stage"]
        emins = entry["emin"]
        for i, stage_num in enumerate(stages):
            idx = int(stage_num) - 1
            if 0 <= idx < num_stages and float(emins[i]) > 0:
                soft_emin_arr[idx] = float(emins[i])
                soft_cost_arr[idx] = cost

        if any(v > 0 for v in soft_emin_arr):
            reservoir["soft_emin"] = soft_emin_arr
            reservoir["soft_emin_cost"] = soft_cost_arr
            # Static box floor only — let the schedule live in soft_emin.
            scalar_emin = self.central_parser.centrals_of_type["embalse"]
            for c in scalar_emin:
                if c["name"] == central_name:
                    reservoir["emin"] = c["emin"]
                    break

    @staticmethod
    def _find_reservoir(system: HydroSystemOutput, name: str) -> Optional[Reservoir]:
        """Find a reservoir dict by name in the system."""
        for r in system["reservoir_array"]:
            if r["name"] == name:
                return r
        return None

    def _fix_first_seepage_segment(
        self, rsv: Reservoir, segments: List[Dict[str, Any]]
    ) -> List[Dict[str, Any]]:
        """Force ``q_filt(vmin) = 0`` on the first piecewise seepage segment.

        PLP's filtration curves are physically expected to drop to zero at
        an empty reservoir, but the raw data in plpfilemb.dat occasionally
        violates this (e.g. CIPRESES first segment yields a non-zero
        ``q_filt`` at ``vmin`` due to fitting noise).  Without correction
        the LP can be forced to discharge water that isn't physically in
        storage, breaking the SDDP forward pass near the lower volume
        bound.

        The fix anchors the first segment at two points:
        - ``q_filt(vmin) = 0`` (new constraint)
        - ``q_filt(seg2.volume)`` = original value at the joint with
          segment 2 (preserves continuity with the rest of the curve)

        This produces a slightly different (usually steeper) slope for
        segment 1 and a corresponding intercept; segments 2+ are left
        unchanged.

        When ``options['plp_legacy']`` is true the segments are emitted
        verbatim and only a warning is logged — for bit-for-bit PLP
        comparison work.
        """
        if not segments:
            return segments

        first = segments[0]
        slope = float(first.get("slope", 0.0))
        constant = float(first.get("constant", 0.0))

        emin_raw = rsv.get("emin", 0.0)
        vmin = float(emin_raw) if isinstance(emin_raw, (int, float)) else 0.0

        q_at_vmin = constant + slope * vmin
        if abs(q_at_vmin) <= 1e-9:
            return segments  # already zero — no fix needed

        rsv_name = rsv.get("name", "?")
        plp_legacy = bool(self.options.get("plp_legacy", False))

        if plp_legacy or len(segments) < 2:
            # In PLP-legacy mode keep raw coefficients; if there's no
            # second segment we can't anchor the new slope cleanly.
            mode = "plp-legacy preserved" if plp_legacy else "single segment"
            _logger.warning(
                "Reservoir '%s' seepage first segment: q(vmin=%.4f)=%.4f "
                "(expected 0); %s — no fix applied.",
                rsv_name,
                vmin,
                q_at_vmin,
                mode,
            )
            return segments

        seg2_vol = float(segments[1].get("volume", 0.0))
        if seg2_vol <= vmin:
            _logger.warning(
                "Reservoir '%s' seepage: second segment starts at vol=%.4f "
                "≤ vmin=%.4f; cannot anchor first-segment fix — skipping.",
                rsv_name,
                seg2_vol,
                vmin,
            )
            return segments

        # Anchor: q(vmin)=0 and q(seg2_vol)=q_at_seg2 (continuity).
        q_at_seg2 = constant + slope * seg2_vol
        new_slope = q_at_seg2 / (seg2_vol - vmin)
        new_constant = -new_slope * vmin

        _logger.warning(
            "Reservoir '%s' seepage first segment: q(vmin=%.4f)=%.4f → 0 "
            "(rebuilt: slope %.6g→%.6g, constant %.6g→%.6g, anchored at "
            "vol=%.4f with q=%.4f).",
            rsv_name,
            vmin,
            q_at_vmin,
            slope,
            new_slope,
            constant,
            new_constant,
            seg2_vol,
            q_at_seg2,
        )

        fixed = list(segments)
        fixed[0] = {
            **first,
            "slope": new_slope,
            "constant": new_constant,
        }
        return fixed

    def _append_reservoir_constraint(
        self,
        system: HydroSystemOutput,
        rsv: Reservoir,
        element: Dict[str, Any],
        system_key: str,
        embedded_key: str,
    ) -> None:
        """Append a reservoir constraint to system-level array or embedded."""
        if self._embed_reservoir_constraints:
            cast(Dict[str, Any], rsv).setdefault(embedded_key, []).append(element)
        else:
            cast(Dict[str, Any], system)[system_key].append(element)

    def _process_seepages(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process seepage data from plpcenfi.dat.

        Each entry links a central (waterway source) to a receiving
        reservoir with a slope/constant seepage model.
        """
        if not self.cenfi_parser:
            return

        # Build name→waterway name lookup from the already-created turbines
        turbine_waterway: Dict[str, str] = {
            t["name"]: t["waterway"] for t in system["turbine_array"] if "waterway" in t
        }

        seep_array = system["reservoir_seepage_array"]

        for entry in self.cenfi_parser.seepages:
            central_name = entry["name"]
            reservoir_name = entry["reservoir"]

            # Resolve waterway uid
            ww_uid = turbine_waterway.get(central_name)
            if ww_uid is None:
                central = central_parser.get_central_by_name(central_name)
                if central is None:
                    _logger.warning(
                        "ReservoirSeepage central '%s' not found; skipping.",
                        central_name,
                    )
                    continue
                ww_uid = central["name"]

            # Find the target reservoir
            rsv = self._find_reservoir(system, reservoir_name)
            if rsv is None:
                central = central_parser.get_central_by_name(reservoir_name)
                if central is not None:
                    rsv = self._find_reservoir(system, central["name"])
                if rsv is None:
                    _logger.warning(
                        "ReservoirSeepage reservoir '%s' not found; skipping.",
                        reservoir_name,
                    )
                    continue

            seep_idx = len(seep_array) + len(rsv.get("seepage", [])) + 1
            seepage: Dict[str, Any] = {
                "uid": rsv["uid"],
                "name": f"{rsv['name']}_seepage_{seep_idx}",
                "waterway": ww_uid,
                "reservoir": rsv["name"],
                "slope": entry["slope"],
                "constant": entry["constant"],
            }

            # Include piecewise segments when present
            segments = entry.get("segments", [])
            if segments:
                seepage["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "constant": seg["constant"],
                    }
                    for seg in segments
                ]

            self._append_reservoir_constraint(
                system, rsv, seepage, "reservoir_seepage_array", "seepage"
            )

    def _process_seepages_filemb(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process seepages from plpfilemb.dat (primary PLP seepage model).

        Each entry in plpfilemb.dat provides:
        - ``embalse``: source reservoir name (filtered reservoir)
        - ``central``: receiving central name (destination of filtrated water)
        - ``mean_seepage``: mean flow [m³/s], used as initial slope fallback
        - ``segments``: piecewise-linear seepage curve (volume→slope/constant)

        A new seepage waterway is created from the source reservoir's
        junction to the receiving central's junction.  The source reservoir
        uid drives the volume-dependent LP update, exactly as described in
        the PLP Fortran subroutine ``LeeFilEmb`` / ``GenPDFilAi``.

        This method is called instead of ``_process_seepages`` when
        ``filemb_parser`` is available.
        """
        if not self.filemb_parser:
            return

        # Build a name→uid lookup from already-created reservoirs and junctions
        reservoir_name_map: Dict[str, str] = {
            r["name"]: r["name"] for r in system["reservoir_array"]
        }

        # Build central name→number lookup for receiving centrals
        central_number: Dict[str, int] = {}
        for central_entry in central_parser.centrals:
            central_number[str(central_entry["name"])] = int(central_entry["number"])

        for entry in self.filemb_parser.seepages:
            embalse_name = entry["embalse"]
            receiving_name = entry["central"]
            segments = entry.get("segments", [])

            # Resolve source reservoir name and number
            rsv_name = reservoir_name_map.get(embalse_name)
            embalse_central = central_parser.get_central_by_name(embalse_name)
            if rsv_name is None or embalse_central is None:
                if embalse_central is None:
                    _logger.warning(
                        "Filemb embalse '%s' not found; skipping.", embalse_name
                    )
                    continue
                rsv_name = embalse_central["name"]
            embalse_number = int(embalse_central["number"])

            # Resolve receiving central junction id (NomCen → gtopt junction)
            rcv_id = central_number.get(receiving_name)
            if rcv_id is None:
                _logger.warning(
                    "Filemb receiving central '%s' not found; skipping.",
                    receiving_name,
                )
                continue

            # Create a new seepage waterway from source reservoir's junction
            # to receiving central's junction (matching PLP GenPDFilAi behaviour)
            filt_waterway = self._create_waterway(
                f"filt_{embalse_name}",
                embalse_number,
                rcv_id,
            )
            if filt_waterway is None:
                _logger.warning(
                    "Filemb seepage waterway for '%s'→'%s' could not be created"
                    " (source == target?); skipping.",
                    embalse_name,
                    receiving_name,
                )
                continue
            system["waterway_array"].append(filt_waterway)

            # Find the source reservoir
            rsv = self._find_reservoir(system, rsv_name)
            if rsv is None:
                _logger.warning("Filemb reservoir '%s' not found; skipping.", rsv_name)
                continue

            # Apply the q(vmin)=0 fix to the first segment unless --plp-legacy
            # is set (in which case keep raw PLP coefficients).
            segments = self._fix_first_seepage_segment(rsv, segments)

            # Use first segment's values as the default slope/constant
            default_slope = segments[0]["slope"] if segments else 0.0
            default_constant = segments[0]["constant"] if segments else 0.0

            seep_array = system["reservoir_seepage_array"]
            seep_idx = len(seep_array) + len(rsv.get("seepage", [])) + 1
            seepage: Dict[str, Any] = {
                "uid": rsv["uid"],
                "name": f"{rsv['name']}_seepage_{seep_idx}",
                "waterway": filt_waterway["name"],
                "reservoir": rsv["name"],
                "slope": default_slope,
                "constant": default_constant,
            }

            if segments:
                seepage["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "constant": seg["constant"],
                    }
                    for seg in segments
                ]

            self._append_reservoir_constraint(
                system, rsv, seepage, "reservoir_seepage_array", "seepage"
            )

    def _process_reservoir_discharge_limits(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process drawdown limit data from plpralco.dat.

        Creates ReservoirDischargeLimit elements that constrain the stage-average
        discharge from a reservoir as a piecewise-linear function of volume.
        The waterway reference is resolved from the reservoir's turbine.

        Reservoirs listed in ``--disable-discharge-limit-for`` are skipped:
        gtopt models the discharge-limit row as a hard inequality, but PLP
        relies on its soft ``vrbp``/``vrbn`` slack pair on the same row
        (``c6531..c6540``/``c6541..c6550`` family).  Without the slack, gtopt
        can become spuriously infeasible at iter-0 of an SDDP cascade once
        the forward pass drives the reservoir down to its emin floor.
        """
        if not self.ralco_parser:
            return

        # Optional CLI-provided exclusion list (comma-separated reservoir
        # names).  Empty / unset → emit all entries (legacy behaviour).
        disabled_raw = (
            self.options.get("disable_discharge_limit_for") if self.options else None
        )
        disabled_set: set[str] = set()
        if disabled_raw:
            disabled_set = {
                name.strip() for name in str(disabled_raw).split(",") if name.strip()
            }

        # Build reservoir name → flow-source maps.  A classic coupling
        # turbine carries its flow on a ``_gen`` Waterway (``waterway``
        # ref); a built-in Turbine waterway carries it on the Turbine
        # itself (``turbine`` ref).  ``ReservoirDischargeLimit`` accepts
        # EITHER — exactly one of ``waterway`` / ``turbine`` (see
        # ``reservoir_discharge_limit.hpp``), so built-in turbines are no
        # longer skipped here.
        turbine_waterway: Dict[str, str] = {}
        builtin_turbines: set[str] = set()
        for turbine in system["turbine_array"]:
            if "waterway" in turbine:
                turbine_waterway[turbine["name"]] = turbine["waterway"]
            else:
                builtin_turbines.add(turbine["name"])

        for entry in self.ralco_parser.reservoir_discharge_limits:
            rsv_name = entry["reservoir"]
            if rsv_name in disabled_set:
                _logger.info(
                    "Skipping plpralco discharge limit for reservoir '%s' "
                    "(disabled via --disable-discharge-limit-for).",
                    rsv_name,
                )
                continue
            segments = entry.get("segments", [])

            rsv = self._find_reservoir(system, rsv_name)
            if rsv is None:
                _logger.warning(
                    "Ralco reservoir '%s' not found in reservoir_array; skipping.",
                    rsv_name,
                )
                continue

            # Resolve the flow source: legacy ``_gen`` Waterway (coupling
            # turbine) or the built-in Turbine itself.
            ww_name = turbine_waterway.get(rsv_name)
            ddl_array = system["reservoir_discharge_limit_array"]
            ddl_idx = len(ddl_array) + len(rsv.get("discharge_limit", [])) + 1
            ddl: Dict[str, Any] = {
                "uid": rsv["uid"],
                "name": f"{rsv['name']}_dlim_{ddl_idx}",
                "reservoir": rsv["name"],
            }
            # The discharge limit caps the same flow column regardless of
            # whether it is reached via a ``_gen`` Waterway (coupling
            # turbine) or the built-in Turbine — ``ReservoirDischargeLimit``
            # treats the two refs identically.
            if ww_name:
                ddl["waterway"] = ww_name
            elif rsv_name in builtin_turbines:
                ddl["turbine"] = rsv_name
            else:
                _logger.warning(
                    "Ralco reservoir '%s' has no turbine flow source; skipping.",
                    rsv_name,
                )
                continue

            if segments:
                ddl["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "intercept": seg["intercept"],
                    }
                    for seg in segments
                ]

            self._append_reservoir_constraint(
                system, rsv, ddl, "reservoir_discharge_limit_array", "discharge_limit"
            )

    def _process_reservoir_efficiencies(
        self,
        system: HydroSystemOutput,
    ) -> None:
        """Process reservoir efficiency data and embed inside reservoirs.

        Each entry in plpcenre.dat links a central (turbine) to a reservoir
        and provides a piecewise-linear efficiency curve (rendimiento) that
        maps reservoir volume to turbine conversion rate [MW·s/m³].
        """
        if not self.cenre_parser:
            return

        # Build turbine name→uid lookup from already-created turbines
        turbine_name_map: Dict[str, str] = {
            t["name"]: t["name"] for t in system["turbine_array"]
        }

        for entry in self.cenre_parser.efficiencies:
            central_name = entry["name"]
            reservoir_name = entry["reservoir"]

            turb_uid = turbine_name_map.get(central_name)
            if turb_uid is None:
                central_data = self.central_parser.get_central_by_name(central_name)
                if central_data is not None and central_data.get("bus", 0) <= 0:
                    _logger.debug(
                        "Efficiency central '%s': reservoir-only central"
                        " (bus<=0), no turbine; skipping.",
                        central_name,
                    )
                else:
                    _logger.warning(
                        "Efficiency central '%s': no matching turbine found;"
                        " skipping efficiency entry.",
                        central_name,
                    )
                continue

            rsv = self._find_reservoir(system, reservoir_name)
            if rsv is None:
                _logger.warning(
                    "Efficiency reservoir '%s' not found; skipping.",
                    reservoir_name,
                )
                continue

            segments: List[ProductionFactorSegment] = [
                {
                    "volume": seg["volume"],
                    "slope": seg["slope"],
                    "constant": seg["constant"],
                }
                for seg in entry["segments"]
            ]

            pfac_array = system["reservoir_production_factor_array"]
            pfac_idx = len(pfac_array) + len(rsv.get("production_factor", [])) + 1
            pfac: Dict[str, Any] = {
                "uid": rsv["uid"],
                "name": f"{rsv['name']}_pfac_{pfac_idx}",
                "turbine": turb_uid,
                "reservoir": rsv["name"],
                "mean_production_factor": entry["mean_production_factor"],
                "segments": segments,
            }
            self._append_reservoir_constraint(
                system,
                rsv,
                pfac,
                "reservoir_production_factor_array",
                "production_factor",
            )

    def _process_cenpmax(
        self,
        system: HydroSystemOutput,
    ) -> None:
        """Process plpcenpmax.dat curves into production-factor segments.

        For each volume-dependent Pmax curve entry:

        - Looks up the central to recover ``pmax`` and ``efficiency``
          (Rendi). Computes the physical flow cap ``flow_ref = pmax / Rendi``.
        - Fixes the turbine's generation waterway ``fmax`` to ``flow_ref``.
        - Emits a :class:`ReservoirProductionFactor` whose segments are the
          PLP ``{volume, slope, constant}`` curve scaled by ``1/flow_ref``.
          This turns the MW curve into a production-factor curve
          (``MW / (m³/s)``) because the LP flow variable is already bounded
          by ``flow_ref``, so ``PF(V) × flow_ref`` reproduces PLP's Pmax(V).

        Entries referencing unknown centrals, centrals with ``pmax <= 0`` /
        ``efficiency <= 0``, or centrals without a turbine (bus<=0) are
        skipped with a warning.  This method is additive to
        ``_process_reservoir_efficiencies`` and appends to the same
        ``reservoir_production_factor_array``.
        """
        if not self.cenpmax_parser:
            return

        # Turbine name → turbine dict (covers BOTH legacy coupling turbines
        # — which carry a ``waterway`` ref — and built-in turbines, which
        # carry the flow cap on ``capacity``).
        turbine_by_name: Dict[str, Turbine] = {
            t["name"]: t for t in system["turbine_array"]
        }

        # Waterway name → waterway dict (to set fmax in place)
        waterway_by_name: Dict[str, Waterway] = {
            w["name"]: w for w in system["waterway_array"]
        }

        for idx, entry in enumerate(self.cenpmax_parser.pmax_curves, start=1):
            central_name = entry["name"]
            reservoir_name = entry["reservoir"]

            central_data = self.central_parser.get_central_by_name(central_name)
            if central_data is None:
                _logger.warning(
                    "Cenpmax central '%s' not found in central_parser; skipping.",
                    central_name,
                )
                continue

            pot_max = float(central_data.get("pmax", 0.0) or 0.0)
            efficiency = float(central_data.get("efficiency", 0.0) or 0.0)

            if pot_max <= 0.0 or efficiency <= 0.0:
                _logger.warning(
                    "Cenpmax central '%s': pmax=%g, efficiency=%g — cannot "
                    "compute flow cap; skipping.",
                    central_name,
                    pot_max,
                    efficiency,
                )
                continue

            flow_ref = pot_max / efficiency

            turbine = turbine_by_name.get(central_name)
            if turbine is None:
                if central_data.get("bus", 0) <= 0:
                    _logger.debug(
                        "Cenpmax central '%s': reservoir-only central"
                        " (bus<=0), no turbine; skipping.",
                        central_name,
                    )
                else:
                    _logger.warning(
                        "Cenpmax central '%s': no matching turbine found; skipping.",
                        central_name,
                    )
                continue

            if "waterway" in turbine:
                # Legacy coupling turbine: pin the gen Waterway's flow cap.
                waterway = waterway_by_name.get(turbine["waterway"])
                if waterway is None:
                    _logger.warning(
                        "Cenpmax central '%s': generation waterway '%s' not"
                        " found; skipping.",
                        central_name,
                        turbine["waterway"],
                    )
                    continue
                waterway["fmax"] = flow_ref
            else:
                # Built-in Turbine waterway: the flow cap lives on the
                # Turbine itself (``capacity``), not a separate gen Waterway.
                turbine["capacity"] = flow_ref

            rsv = self._find_reservoir(system, reservoir_name)
            if rsv is None:
                _logger.warning(
                    "Cenpmax reservoir '%s' not found; skipping PF entry.",
                    reservoir_name,
                )
                continue

            # Convert the raw Pmax(V) curve to production-factor units by
            # dividing by `flow_ref` (MW → MW/(m³/s)).  Type as
            # ``List[Dict[str, float]]`` so the result composes with
            # ``_merge_pf_curves_min`` (which is intentionally curve-shape-
            # agnostic — the PLP / cenre / cenpmax curves all share the
            # same ``volume / slope / constant`` keys but appear in code
            # paths under different TypedDict aliases).
            cenpmax_pf_segments: List[Dict[str, float]] = [
                {
                    "volume": float(seg["volume"]),
                    "slope": float(seg["slope"]) / flow_ref,
                    "constant": float(seg["constant"]) / flow_ref,
                }
                for seg in entry["segments"]
            ]

            # When plpcenre ALSO provides a PF curve for this reservoir,
            # merge the two sources by taking the MIN value at each
            # plpcenpmax breakpoint — the most restrictive curve wins,
            # matching PLP's combined physics where turbine flow is
            # bounded by BOTH Rendi (efficiency) and Pmax (power cap).
            # Emit a single replacement entry; drop the cenre one.
            pfac_array = system["reservoir_production_factor_array"]
            cenre_idx = next(
                (
                    i
                    for i, e in enumerate(pfac_array)
                    if e.get("reservoir") == rsv["name"]
                ),
                None,
            )

            scaled_segments = cenpmax_pf_segments
            if cenre_idx is not None:
                cenre_entry = pfac_array[cenre_idx]
                cenre_segs = cenre_entry.get("segments", [])
                if cenre_segs:
                    scaled_segments = _merge_pf_curves_min(
                        cenpmax_pf_segments, cenre_segs
                    )
                pfac_array.pop(cenre_idx)
                embedded = rsv.get("production_factor", [])
                if isinstance(embedded, list):
                    rsv["production_factor"] = [
                        e for e in embedded if e is not cenre_entry
                    ]
                _logger.warning(
                    "Reservoir '%s' has both plpcenre and plpcenpmax PF "
                    "curves — emitting MIN-envelope combined curve "
                    "(%d segments) in place of '%s'.",
                    rsv["name"],
                    len(scaled_segments),
                    cenre_entry.get("name", "?"),
                )

            pfac: Dict[str, Any] = {
                "uid": rsv["uid"],
                "name": f"{rsv['name']}_pmax_pfac_{idx}",
                "turbine": central_name,
                "reservoir": rsv["name"],
                "mean_production_factor": efficiency,
                "segments": scaled_segments,
            }
            self._append_reservoir_constraint(
                system,
                rsv,
                pfac,
                "reservoir_production_factor_array",
                "production_factor",
            )

    def _promote_reservoir_soft_emin_to_parquet(
        self, system: HydroSystemOutput
    ) -> None:
        """Normalise per-reservoir ``soft_emin`` / ``soft_emin_cost``
        emissions so the strict gtopt JSON parser accepts them.

        * ``soft_emin`` carries the per-stage minimum-energy floors and
          is genuinely time-varying, so it goes to
          ``Reservoir/soft_emin.parquet`` (one row per stage, one
          ``uid:N`` column per reservoir).  The reservoir's inline list
          is replaced with the FileSched string sentinel
          ``"soft_emin"``.
        * ``soft_emin_cost`` is a single penalty rate (either the CLI
          default or the per-reservoir resolved cost — see
          `_resolve_storage_bound_cost`).  Its emission as a 1-D
          repeating list was incidental: emit it as a scalar instead
          (`Real` is the first alternative in the TB variant, so the
          parser accepts it natively).

        gtopt's `OptTBRealFieldSched` is a
        ``variant<Real, vector<vector<Real>>, FileSched>`` — a 1-D
        inline array is not a valid shape, so without this promotion
        the strict daw::json parser bails out with
        ``Expected array type to begin with '['`` on the first scalar
        of the inline list.  Idempotent — a no-op when no reservoir
        has an inline list.
        """
        soft_emin_rows: list[tuple[int, int, float]] = []

        for reservoir in system.get("reservoir_array", []):
            uid = reservoir["uid"]
            soft_emin = reservoir.get("soft_emin")
            if isinstance(soft_emin, list):
                for stage_idx, v in enumerate(soft_emin):
                    soft_emin_rows.append((stage_idx + 1, uid, float(v)))
                # Post-rewrite the inline `list[float]` to a FileSched
                # reference string ("soft_emin").  The TypedDict shape
                # is the pre-rewrite contract — silence mypy on this
                # intentional in-place type change.
                reservoir["soft_emin"] = "soft_emin"  # type: ignore[typeddict-item]

            soft_cost = reservoir.get("soft_emin_cost")
            if isinstance(soft_cost, list):
                # The per-stage list is the same scalar repeated (either
                # the CLI default or `_resolve_storage_bound_cost`).
                # Collapse to the first non-zero value, or 0.0 if the
                # list is all zeros (which means soft_emin itself was
                # also all zeros and the upstream paths would not have
                # set the list in the first place).
                non_zero = next((float(c) for c in soft_cost if float(c) > 0), 0.0)
                reservoir["soft_emin_cost"] = non_zero  # type: ignore[typeddict-item]

        if not soft_emin_rows:
            return

        output_dir = (
            self.options["output_dir"] / "Reservoir"
            if "output_dir" in self.options
            else Path("Reservoir")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        df = pd.DataFrame(soft_emin_rows, columns=["stage", "uid", "value"])
        wide = (
            df.pivot(index="stage", columns="uid", values="value")
            .fillna(0.0)
            .reset_index()
        )
        wide.columns = ["stage"] + [f"uid:{c}" for c in wide.columns[1:]]
        self.write_dataframe(wide, output_dir, "soft_emin")

    def _write_parquet_files(self) -> Dict[str, List[str]]:
        """Write demand data to Parquet file format."""
        #
        # write the manem data
        #
        output_dir = (
            self.options["output_dir"] / "Reservoir"
            if "output_dir" in self.options
            else Path("Reservoir")
        )
        output_dir.mkdir(parents=True, exist_ok=True)
        manem_writer = ManemWriter(
            self.manem_parser, self.central_parser, self.stage_parser, self.options
        )
        manem_cols = manem_writer.to_parquet(output_dir)

        #
        # collect the cols
        #

        return manem_cols

    def _write_transit_waterway_bounds(self) -> None:
        """Emit ``Waterway/fmin.parquet`` + ``Waterway/fmax.parquet``.

        Mirrors the per-stage plpmance flow envelope onto the gen
        waterway of every transit-only central (``bus = 0``, e.g.
        LMAULE, B_LaMina) that has plpmance entries.  These centrals
        have no generator entry, so the existing
        ``Generator/pmin.parquet`` columns sit unconsumed.  We rekey
        the columns from ``uid:<central_id>`` to ``uid:<gen_waterway_uid>``
        and write fresh parquet files in the Waterway subdirectory.
        Only sets ``fmin`` / ``fmax`` string refs on a gen waterway
        if the corresponding parquet column survives ManceWriter's
        static-fill drop (a column whose entries all match the
        central's static ``pmin`` / ``pmax`` is dropped — there is no
        per-stage override to wire).

        No-op when there are no transit centrals needing wiring or
        when the required parsers were not provided.
        """
        if not self._transit_gen_waterways:
            return
        if (
            self.mance_parser is None
            or self.block_parser is None
            or self.central_parser is None
        ):
            return

        names_set = {nm for _, _, nm, _ in self._transit_gen_waterways}
        items = [m for m in self.mance_parser.mances if m["name"] in names_set]
        if not items:
            return

        rename_map = {
            f"uid:{cid}": f"uid:{wuid}"
            for cid, wuid, _, _ in self._transit_gen_waterways
        }

        writer = ManceWriter(
            mance_parser=self.mance_parser,
            central_parser=self.central_parser,
            block_parser=self.block_parser,
            options=self.options,
        )
        df_pmin, df_pmax = writer.to_dataframe(items=items)
        df_fmin = df_pmin.rename(columns=rename_map)
        df_fmax = df_pmax.rename(columns=rename_map)

        out_dir = (
            self.options["output_dir"] / "Waterway"
            if "output_dir" in self.options
            else Path("Waterway")
        )
        out_dir.mkdir(parents=True, exist_ok=True)
        if not df_fmin.empty:
            writer.write_dataframe(df_fmin, out_dir, "fmin")
        if not df_fmax.empty:
            writer.write_dataframe(df_fmax, out_dir, "fmax")

        # Upgrade ``fmin`` / ``fmax`` to string refs on each gen
        # waterway whose column survived the parquet write.  Centrals
        # whose values matched the static fill (e.g. CLAJRUCUE/
        # RIO_TENO with all-zero pmin) keep their numeric defaults.
        fmin_uids = set(df_fmin.columns) if not df_fmin.empty else set()
        fmax_uids = set(df_fmax.columns) if not df_fmax.empty else set()
        wired_fmin = wired_fmax = 0
        for _, wuid, _, ww in self._transit_gen_waterways:
            col = f"uid:{wuid}"
            if col in fmin_uids:
                ww["fmin"] = "fmin"
                wired_fmin += 1
            if col in fmax_uids:
                ww["fmax"] = "fmax"
                wired_fmax += 1
        _logger.info(
            "Wrote per-stage Waterway fmin/fmax (%d/%d gen waterways) "
            "for transit central(s): %s",
            wired_fmin,
            wired_fmax,
            ", ".join(sorted(names_set)),
        )
