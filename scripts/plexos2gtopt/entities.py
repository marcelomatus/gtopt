"""Typed dataclasses for parsed PLEXOS entities.

Each extractor in :mod:`plexos2gtopt.parsers` returns one of these.
They deliberately stay close to PLEXOS semantics (units, English
field names matching ``share/gtopt/naming_dialects.json`` ``dialect:
plexos`` aliases) so the writer can perform unit conversion in one
place rather than scattering it across parsers.

All entities are frozen dataclasses so they can be hashed and used as
dict keys / set members when building topology indexes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True)
class BundleSpec:
    """Top-level bundle parameters extracted from ``PLEXOS_Param.xml``.

    Attributes:
        bundle_date: Calendar date of the daily PCP run (``YYYY-MM-DD``).
        step_count: Number of periods (24 for hourly daily PCP).
        step_type: Period type — ``"Hour"`` is the only one supported
            in v0; ``"Minute"`` / ``"Day"`` reject loudly.
        day_beginning: Hour-of-day for stage 1 block 1 (usually 0).
        currency: Currency tag (``"USD"`` / ``"CLP"``); pass-through.
        bundle_name: Original archive basename for diagnostics.
    """

    bundle_date: str = ""
    step_count: int = 24
    step_type: str = "Hour"
    day_beginning: int = 0
    currency: str = "USD"
    bundle_name: str = ""
    # PLEXOS Region.VoLL ($/MWh) — Value of Lost Load — maps to gtopt's
    # ``model_options.demand_fail_cost``.  When every Region carries a
    # consistent non-zero VoLL the parser surfaces it here; CEN PCP
    # ships 467.19 $/MWh on both Central-Southern and Northern.
    # Defaults to 1000 (gtopt's traditional default) when PLEXOS leaves
    # VoLL unset / inconsistent across regions.
    demand_fail_cost: float = 1000.0
    # Horizon length in days.  ``1`` (default) keeps the legacy day-1
    # PCP behaviour; ``7`` reconstructs the full CEN PCP forward-look
    # week.  All time-varying CSV readers
    # (:func:`plexos_csv.read_wide` / :func:`read_long`) are widened
    # via their ``n_days`` parameter; the simulation emits
    # ``n_days × step_count`` hourly blocks in the ``hourly`` mode.
    n_days: int = 1
    # Block layout for the PLEXOS-native mode.  Empty tuple ⇒ emit
    # ``n_days × step_count`` uniform hourly blocks (``hourly`` mode).
    # Non-empty ⇒ each inner tuple is the 1-indexed hourly intervals
    # that constitute one block, in calendar order.  CEN PCP daily
    # produces 111 blocks across 7 days; ``len(block_layout) == 111``,
    # ``sum(len(b) for b in block_layout) == 168``.  Quantities are
    # aggregated per block in the writer: ``mean`` for time-average
    # values (demand, hydro inflow), ``min`` for capacity (line units,
    # availability), and the block duration = ``len(intervals)``
    # hours.
    block_layout: tuple[tuple[int, ...], ...] = ()


@dataclass(frozen=True)
class NodeSpec:
    """A bus / node (``t_class[Node]``, class_id=22)."""

    object_id: int
    name: str
    region: str | None = None
    zone: str | None = None


@dataclass(frozen=True)
class FuelSpec:
    """A fuel record (``t_class[Fuel]``, class_id=4).

    ``price`` is the day-of-bundle scalar from ``Fuel_Price.csv``
    (monthly series collapsed to one value); ``heat_content`` ships
    from the PLEXOS attribute ``Heat Content`` when present.

    ``co2_rate`` / ``co2_upstream_rate`` carry the per-fuel-unit CO₂
    combustion / upstream emission factor (``tCO₂ / GJ`` when the
    matching ``Generator.heat_rate`` is in ``GJ / MWh``).  PLEXOS
    exposes these either as a direct property on the Fuel object
    (``CO2 Production Rate`` / ``Emissions Rate``) or as a property
    on the ``Emission → Fuel`` membership (``Production Rate``,
    matched by the Emission object's name carrying ``CO2`` /
    ``CO₂``).  The writer emits a :class:`FuelEmissionFactor` row
    with ``emission = "co2"`` when either rate is non-zero.
    """

    object_id: int
    name: str
    price: float = 0.0
    heat_content: float = 0.0
    co2_rate: float = 0.0
    co2_upstream_rate: float = 0.0
    #: Canonical fuel-family tag derived from ``name`` by
    #: :func:`plexos2gtopt.parsers.fuel_family_of_fuel`.  Values are
    #: stable lowercase strings — ``"diesel"`` / ``"fuel_oil"`` /
    #: ``"gas"`` / ``"glp"`` / ``"biomasa"`` / ``"biogas"`` /
    #: ``"carbon"`` / ``"otros"`` (CEN's residual category) — and
    #: ``"other"`` when the prefix doesn't match any known family.
    #: The writer surfaces this as the gtopt ``Fuel.type`` field
    #: (``include/gtopt/fuel.hpp:126`` — "Optional element type/category
    #: tag"), mirroring the ``Generator.type = "thermal"|"renewable"``
    #: convention.  Shared with
    #: :func:`plexos2gtopt.parsers.fuel_family_of_generator` so the
    #: orphan-recovery sibling search and the published fuel definition
    #: agree by construction.
    type_tag: str = "other"
    #: Project-specific fuel SUB-TYPE that disambiguates within a
    #: family.  Used to select the right IPCC sub-grade when the
    #: family alone is ambiguous:
    #:
    #:   * coal sub-bituminous vs bituminous vs anthracite vs lignite
    #:   * pipeline natural gas vs LNG (same combustion factor, but
    #:     LNG carries a real upstream chain that IPCC default omits)
    #:
    #: Populated by :func:`extract_fuels` from name-pattern heuristics
    #: where the upstream PLEXOS XML doesn't ship a sub-type property
    #: (PLEXOS Fuel objects in CEN PCP carry only family-level
    #: Heat Value / Price / Offtake — no sub-grade tag).  Empty string
    #: means "no project-specific subtype detected; fall back to the
    #: family alias in the IPCC defaults".
    subtype: str = ""
    #: Weekly fuel offtake cap from ``Fuel_MaxOfftakeWeek.csv`` (one
    #: scalar per fuel, for the week containing the bundle's
    #: reference date).  Mirrors the PLEXOS ``FueMaxOffWeek_<fuel>``
    #: Constraint object.  ``None`` when no row applies (e.g. the
    #: fuel ships in the CSV with VALUE = 0 on the binding week — a
    #: legitimate "shut" signal — distinct from "not in the CSV at
    #: all", which means the cap is irrelevant for the bundle).
    max_offtake: float | None = None
    #: Per-unit soft penalty ``[$/<fuel_unit>]`` for exceeding
    #: ``max_offtake`` (gtopt ``Fuel.max_offtake_cost``).  PLEXOS treats
    #: the ``FueMaxOffWeek_*`` caps as SOFT and routinely violates them
    #: (the solved model burns gas above the contract band), so we emit
    #: a finite cost rather than a hard wall — a hard cap throttles the
    #: central/north LNG combined-cycles by ~100 GWh and forces coal.
    #: ``None`` ⇒ hard cap.
    max_offtake_cost: float | None = None

    #: Minimum offtake floor for the bundle horizon ``[fuel-unit]``
    #: from PLEXOS ``Fuel.Min Offtake`` family (pids 595-600 — bare,
    #: Hour, Day, Week, Month, Year — combined into a single
    #: horizon-wide budget the same way ``max_offtake`` aggregates
    #: ``Fuel_MaxOfftakeWeek.csv`` rows).  Models take-or-pay (ToP)
    #: contracts.  ``None`` ⇒ no floor; 0 ⇒ explicit zero floor (rare
    #: but legal — PLEXOS XML may ship it).  Across the 14 cached CEN
    #: PCP bundles 2025-10 → 2026-05, **zero fuels populate any Min
    #: Offtake property**, so this field is dormant on current CEN
    #: cases; the plumbing exists to honour future bundles that do
    #: ship the property and to mirror the symmetric ``max_offtake``
    #: shape end-to-end.
    min_offtake: float | None = None

    #: Per-unit shortfall penalty ``[$/<fuel_unit>]`` (gtopt
    #: ``Fuel.min_offtake_cost``).  PLEXOS asymmetry: PLEXOS's
    #: ``Min Offtake Penalty`` (pid 602) defaults to **$1000/fuel-unit**
    #: (soft-by-default) — opposite of the ``Max Offtake Penalty``
    #: convention.  The converter translates this idiom: when a PLEXOS
    #: bundle ships ``min_offtake`` without an explicit penalty, the
    #: parser injects ``min_offtake_cost = 1000`` here so the emitted
    #: JSON preserves PLEXOS-faithful soft-by-default semantics, while
    #: the gtopt LP model keeps the gtopt-native "unset ⇒ hard"
    #: convention symmetric with ``max_offtake_cost``.
    #: ``None`` ⇒ hard floor.
    min_offtake_cost: float | None = None


@dataclass(frozen=True)
class GeneratorSpec:
    """A generating unit (``t_class[Generator]``, class_id=2).

    ``pmax_profile`` is the per-block (24 floats) availability series
    from ``Gen_Rating.csv``; renewable units carry the time-varying
    profile here and the writer emits a matching ``GeneratorProfile``.
    Thermal units typically expose a constant series at nameplate.
    """

    object_id: int
    name: str
    bus_name: str
    #: Canonical primary-energy tag derived by
    #: :func:`plexos2gtopt.parsers.primary_energy_of_generator` from the
    #: PLEXOS name suffix (``_DIE``/``_GNL``/``_FV``/``_EO`` etc.) +
    #: category (``Solar Farms``/``Hydro Gen Group *``) + fuel
    #: attachment.  Surfaces on the gtopt ``Generator.type`` JSON field
    #: as a hierarchical ``<top>:<sub>`` string (``"thermal:diesel"``,
    #: ``"renewable:solar"``, ``"renewable:hydro"``); falls back to
    #: bare ``"thermal"`` / ``"renewable"`` when no specific family
    #: was detected — preserving the legacy binary classification for
    #: downstream consumers that rely on ``startswith()``.
    type_tag: str = "renewable"
    pmin: float = 0.0
    pmax: float = 0.0
    # PLEXOS ``Auxiliary Use`` (``Gen_AuxUse.csv``): fraction of gross
    # generation consumed by station service (e.g. 0.068 = 6.8%).  The
    # writer derates net capacity by ``(1 − aux_use)`` so gtopt's
    # injected MW matches PLEXOS's net-of-auxiliary output.  0 ⇒ none.
    aux_use: float = 0.0
    heat_rate: float = 0.0
    vom_charge: float = 0.0
    fuel_transport: float = 0.0
    start_cost: float = 0.0
    shutdown_cost: float = 0.0
    fuel_names: tuple[str, ...] = field(default_factory=tuple)
    pmax_profile: tuple[float, ...] = field(default_factory=tuple)
    # 118-Bus-style schemas ship fuel price on the Generator object
    # (``Fuel Price`` property on the System→Generators collection)
    # instead of on the Fuel object. When set, the writer uses this
    # value directly and skips the fuel-name → fuel-price lookup.
    fuel_price_override: float = 0.0
    # Piecewise heat-rate (cumulative MW breakpoints + per-segment
    # slopes in fuel-unit / MWh). Populated only when PLEXOS ships the
    # quadratic form (``Heat Rate Base`` + ``Heat Rate Incr`` +
    # ``Heat Rate Incr2``); mutually exclusive with scalar ``heat_rate``.
    pmax_segments: tuple[float, ...] = field(default_factory=tuple)
    heat_rate_segments: tuple[float, ...] = field(default_factory=tuple)
    # PLEXOS ``Generator.Fixed Load`` per-period profile
    # (``Gen_FixedLoad.csv``).  When non-empty, the writer emits both
    # ``pmin`` and ``pmax`` matching the per-period fixed-load value:
    # the LP is forced to dispatch EXACTLY this amount
    # (``Generation[t] = Fixed Load[t]``).  Used by must-take
    # renewables (solar / wind CF profile) and forced run-of-river
    # hydro.  Empty for free-dispatch generators (the common case for
    # thermal / battery-driven).
    fixed_load_profile: tuple[float, ...] = field(default_factory=tuple)
    # PLEXOS ``Generator.Initial Generation`` (``Gen_IniGeneration.csv``):
    # the dispatch level at t=0 (start of horizon), used for ramp /
    # commitment continuity across SDDP / cascade rolling windows.
    # Scalar (single value per generator) — PLEXOS only ships the
    # period-1 value.  Defaults to 0 when the CSV doesn't list the
    # generator.
    initial_generation: float = 0.0
    # PLEXOS ``Generator.Units`` (``Gen_IniUnits.csv``): initial number
    # of units online at t=0.  In CEN PCP this is a {0, 1} flag per
    # ``<plant>_U<k>`` Generator (one PLEXOS Generator per physical
    # unit).  Round-tripped onto gtopt's ``Generator.uini`` for
    # downstream tooling; the unit-commitment continuity state is
    # carried separately via ``Commitment.initial_status`` /
    # ``initial_hours``.  ``None`` ⇒ CSV not present / no entry for
    # this generator.
    initial_units: float | None = None
    # Mode-variant inheritance signal (Phase 2 — issue #1 followup,
    # 2026-06).  When a CSV-only thermal orphan inherits its bus + fuel
    # from a longest-common-prefix XML sibling via
    # :func:`plexos2gtopt.parsers._recover_csv_only_thermals`, this
    # field carries that parent's name so the writer can stamp
    # ``[gtopt-meta mode_variant=secondary inherits_emission_from=<parent>]``
    # on the orphan's ``description``.  Downstream
    # ``gtopt_marginal_units`` consults the meta to inherit
    # ``emission_rate`` from the parent when the orphan ends up as the
    # LP marginal at a cell (rare — orphans have ``pmax = 0`` so they
    # almost never dispatch, but the LP basis can elect them at
    # tie-break corners).  Empty string ⇒ not a recovered orphan.
    inherits_emission_from: str = ""


def generator_has_fuel_cost(gen: "GeneratorSpec") -> bool:
    """True when *gen* has a real marginal energy cost (fuel + heat rate).

    Single source of truth shared by the writer
    (:func:`plexos2gtopt.gtopt_writer.build_generator_array`) and the
    post-conversion comparison (:mod:`plexos2gtopt._comparison`).  A
    fuel-cost generator whose ``Fixed Load`` is set is a forced-dispatch /
    commitment trajectory PLEXOS pins exactly (emitted as ``pmin = pmax``);
    a zero-cost renewable / RoR with a Fixed Load is emitted as a
    curtailable cap (``pmin = 0``).  Mirrors the ``has_fuel_cost`` predicate
    that gates that branch — keep the two definitions identical so the
    comparison's PLEXOS-side ``pmin`` mirror cannot drift from the writer.
    """
    has_fuel = bool(gen.fuel_names) or gen.fuel_price_override > 0.0
    return bool(gen.heat_rate_segments and gen.pmax_segments) or (
        gen.heat_rate > 0.0 and has_fuel
    )


@dataclass(frozen=True)
class LineSpec:
    """A transmission line (``t_class[Line]``)."""

    object_id: int
    name: str
    bus_from: str
    bus_to: str
    tmax_ab: float = 0.0
    tmin_ab: float = 0.0
    # Series resistance in per-unit (same unit basis as ``reactance``).
    # Used by the writer to emit a ``resistance`` field on the Line
    # entry so gtopt's piecewise loss model can compute the
    # K-segment loss curve `P_loss = R · f² / V²`.  Zero or unset →
    # no losses on this line (matches gtopt's default).
    resistance: float = 0.0
    # Optional per-hour profile (length = bundle.n_days × 24) for
    # DLR (Dynamic Line Rating) corridors whose Lin_MaxRating.csv ships
    # different ratings per period (e.g. LoAguirre500->Polpaico500: 900
    # MW overnight, 2078 MW for periods 7-23).  When non-empty, the
    # writer aggregates this to per-block tmax_ab and falls back to the
    # scalar above only if the profile is invariant.
    tmax_ab_profile: tuple[float, ...] = ()
    tmin_ab_profile: tuple[float, ...] = ()
    # PLEXOS exposes TWO rating tiers per line:
    #   * Max Flow (pid 1880) — steady-state / continuous rating;
    #     this is what ``Lin_MaxRating.csv`` actually ships
    #     (despite the misleading filename) and what fills
    #     ``tmax_ab`` above.
    #   * Max Rating (pid 1882) — short-term / emergency rating
    #     used by PLEXOS for scheduling-time exceedances.  Lives
    #     only in the input XML t_data (no CSV mirror).  Typically
    #     1.5-2× Max Flow on the CEN PCP bundle (e.g. Capricornio110
    #     ->LaNegra110: Max Flow=76 MW, Max Rating=137 MW).
    # The writer feeds these two into gtopt's soft/hard limit pair:
    #   * tmax_ab          ← Max Rating   (emergency hard cap)
    #   * tmax_normal_ab   ← Max Flow     (steady-state soft cap)
    #   * overload_penalty ← default ($/MWh, applied between the two)
    max_rating: float = 0.0
    min_rating: float = 0.0
    # PLEXOS Enforce Limits (0=Never, 1=Voltage, 2=Always) controls
    # whether the LP applies thermal limits.  Per the Energy Exemplar
    # docs (Line.EnforceLimits): "0=Never → line thermal limits are
    # never enforced; the LP allows unbounded flow with no violation
    # cost".  The converter maps this onto gtopt's soft/hard limit
    # pair:
    #   * EL=0 → drop the hard cap (tmax = +inf via unset)
    #   * EL=1/2 → keep the hard cap (current default behaviour)
    # CEN PCP carries 185 lines at EL=0, 96 at EL=1, 63 at EL=2.
    enforce_limits: int = 2  # default per PLEXOS docs ("Always enforce")
    # Set by ``extract_lines`` for ex-PLEXOS-EL=0 lines (promoted to
    # ``enforce_limits = 1``): the writer turns these into a SOFT cap
    # (free up to the rating, penalised between the rating and
    # ``headroom × rating``, hard at ``headroom × rating``) instead of
    # a plain hard cap.  Orig EL=1/EL=2 lines keep ``soft_cap = False``
    # (plain hard cap); ``--lift-line-caps`` lines are also soft-capped but
    # with the wider band (see ``soft_cap_lifted``).
    soft_cap: bool = False
    # True for ``--lift-line-caps`` corridors: still soft-capped (not
    # uncapped) but with a WIDER free band (4× rating) and a 10× hard cap
    # instead of the regular 2×/5×, since these radial paths are the ones
    # PLEXOS itself runs hardest.  Carries PLEXOS-like over-flow for free
    # yet can't teleport.
    soft_cap_lifted: bool = False
    # Per-period in-service flag from PLEXOS ``Lin_Units.csv`` (1 = in
    # service, 0 = out for maintenance / forced outage).  Length =
    # ``bundle.n_days × 24``; empty ⇒ always in service.  The writer
    # aggregates this to per-block ``Line.in_service`` so an intra-stage
    # outage window opens the line for those blocks only.
    in_service_profile: tuple[int, ...] = field(default_factory=tuple)
    units: int = 1
    reactance: float = 0.0
    wheeling_charge: float = 0.0
    # Per-line override for the piecewise-linear loss segment count.  Set
    # by ``extract_lines`` when the adaptive cube-root rule is enabled
    # (``--loss-error-pct > 0``) — see _adaptive_loss_segments().  Zero
    # (the default) means "no per-line override — let the writer pull
    # the uniform ``GTOPT_NSEG_LOSSES`` env var" (legacy behaviour).
    # When non-zero, the writer emits exactly this many segments on the
    # line, bounded by ``[floor=2, ceiling=--nseg-losses (default 6)]``.
    loss_segments: int = 0
    # Per-line override for the piecewise-linear loss SEGMENT LAYOUT.
    # Set by ``_apply_dynamic_loss_layout`` when ``--loss-pwl-layout dynamic``
    # is active: each line is assigned ``"midpoint"`` or ``"uniform"``
    # by the greedy mean-error allocator so the system-wide signed
    # mean error cancels (heavy lines contribute negative midpoint
    # bias, light lines contribute positive uniform bias).  Empty
    # string (the default) means "no per-line override — the writer
    # pulls ``GTOPT_LOSS_PWL_LAYOUT`` and applies it uniformly across
    # every line".  Accepted values: ``"uniform"``, ``"midpoint"``,
    # ``"equal_error"``, ``"tangent"`` (mirrors gtopt's
    # ``LinePwlLayout`` enum at ``include/gtopt/line_enums.hpp:213``).
    loss_pwl_layout: str = ""
    # L-secant chord segments for ``tangent_signed_flow`` line-loss mode
    # (issue #504).  When > 0, the gtopt LP replaces the single ``|f|``
    # auxiliary with ``L`` segment columns and a piecewise chord upper
    # bound; combined with ``loss_use_sos2 = True`` this enforces SOS2
    # fill-order so the segments saturate in geometric breakpoint order
    # and the chord stays tight.  Stamped by ``_apply_loss_sos2_policy``
    # (``--loss-sos2-lines`` explicit list and/or ``--loss-sos2-auto``
    # heuristic).  Zero (default) ⇒ no L-secant — the LP keeps the
    # single secant for this line.  Only meaningful when the line
    # ultimately routes to ``line_losses_mode = "tangent_signed_flow"``.
    loss_secant_segments: int = 0
    # Companion to ``loss_secant_segments``: when ``True``, the gtopt LP
    # additionally emits an SOS2 declaration over the L segment columns.
    # Without SOS2 the chord remains valid but the LP can pick any
    # convex combination of the L segments, drifting away from the
    # geometric fill order — the SOS2 lock pins the at-most-two-adjacent
    # invariant (Beale-Tomlin 1970) so the chord is tight everywhere.
    # Requires a MIP backend with native SOS2 support (CPLEX today);
    # falls through to the default-throw on CBC / CLP.
    loss_use_sos2: bool = False


@dataclass(frozen=True)
class DemandSpec:
    """A consumer attached to a bus (one per non-zero column in ``Nod_Load.csv``).

    ``fcost`` is the demand-curtailment penalty (``$/MWh`` of unserved
    energy) emitted on the gtopt ``Demand.fcost`` field.  When set it
    overrides the global ``model_options.demand_fail_cost`` for this
    Demand, letting per-Region PLEXOS ``Region.VoLL`` values surface
    natively without the lossy ``max(VoLLs)`` collapse the converter
    used to apply.
    """

    name: str
    bus_name: str
    lmax_profile: tuple[float, ...] = field(default_factory=tuple)
    fcost: float = 0.0


@dataclass(frozen=True)
class BatterySpec:
    """A battery energy-storage system (``t_class[Battery]``, class_id=7)."""

    object_id: int
    name: str
    bus_name: str
    emin: float = 0.0
    emax: float = 0.0
    eini: float = 0.0
    efin: float = 0.0
    pmax_charge: float = 0.0
    pmax_discharge: float = 0.0
    # Per-hour power rating (length = bundle.n_days × 24) from the matching
    # ``BAT_<name>`` Generator's ``Gen_Rating`` series.  When it VARIES across
    # the horizon (battery DLR — e.g. BAT_TOCOPILLA 72→110 MW), the writer
    # emits ``pmax_charge``/``pmax_discharge`` as a per-block matrix matching
    # this series instead of the scalar peak above (which would over-state the
    # battery's power in the de-rated blocks).  Empty ⇒ constant rating ⇒
    # scalar.  CEN ships symmetric Max Power, so one series feeds both legs.
    power_profile: tuple[float, ...] = field(default_factory=tuple)
    input_efficiency: float = 1.0
    output_efficiency: float = 1.0
    # PLEXOS Min Charge / Min Discharge Level (MW): minimum power when
    # actively charging / discharging.  Maps to gtopt's
    # ``Battery.pmin_charge`` / ``pmin_discharge``.  CEN PCP carries
    # these only on 2 batteries (BAT_DEL_DESIERTO, BAT_TOCOPILLA).
    pmin_charge: float = 0.0
    pmin_discharge: float = 0.0
    # PLEXOS ``Max Cycles Day``: daily energy-throughput limit N (cycles
    # per day).  Maps to gtopt's ``Battery.max_cycles_day`` — a HARD
    # constraint ``Σ discharge·Δt ≤ N · capacity`` per day (NOT a cost).
    # 1.0 for all 41 CEN PCP batteries; 0.0 ⇒ no limit emitted.
    max_cycles_day: float = 0.0


@dataclass(frozen=True)
class ReservoirSpec:
    """A hydro reservoir (``t_class[Storage]``, class_id=8).

    Volumes in hm³, water value in $/hm³, spill penalty in $/MWh
    (PLEXOS native — writer converts to $/(m³/s)/h via ``fp_med``).
    """

    object_id: int
    name: str
    emin: float = 0.0
    emax: float = 0.0
    eini: float = 0.0
    efin: float = 0.0
    water_value: float = 0.0
    # True when PLEXOS shipped the 1e+30 "never drain" sentinel on
    # `Water Value`.  The writer must then emit `efin = eini` as a HARD
    # `vol_end >= eini` constraint and skip `efin_cost` entirely (the
    # soft-slack path would let the LP buy out of the sentinel at the
    # clamped price, which is exactly what the sentinel forbids).
    never_drain: bool = False
    spill_penalty_per_mwh: float = 0.0
    # Optional per-block emin/emax profiles (length = n_blocks).  When
    # set, the writer emits the inline ``[[per-block]]`` matrix instead
    # of the scalar ``emin`` / ``emax`` fields.  Used by PLEXOS-style
    # "tight at boundaries, loose interior" reservoir bounds: block 0
    # and block N-1 carry the operational ``Hydro_*Volume.csv`` floor/
    # cap for that day; interior blocks carry the static (physical)
    # ``Min Volume`` / ``Max Volume`` so the LP has free dispatch
    # space mid-week.  Empty tuple ⇒ scalar fallback.
    emin_profile: tuple[float, ...] = field(default_factory=tuple)
    emax_profile: tuple[float, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class WaterwaySpec:
    """A waterway / spillway (``t_class[Waterway]``, class_id=9)."""

    object_id: int
    name: str
    storage_from: str | None = None
    storage_to: str | None = None
    fmin: float = 0.0
    fmax: float = 0.0
    # Per-hour forced-flow profile (length = bundle.n_days × 24) for
    # waterways whose ``Hydro_WaterFlows.csv`` column carries a
    # TIME-VARYING value.  When non-empty, the writer emits
    # ``fmin``/``fmax`` as a per-block matrix matching this series,
    # overriding the scalar ``fmin``/``fmax`` above.  Without this,
    # pinning ``fmin = fmax = max(profile)`` over-constrains the LP
    # on every hour where the natural inflow is below the peak —
    # forcing the upstream cascade (e.g. ``penstock_LOMA_ALTA`` into
    # ``B_Maule``) to dispatch turbines that wouldn't run physically,
    # producing phantom hydro generation and accumulating water at
    # synthetic sinks.
    forced_flow_profile: tuple[float, ...] = ()
    # Explicit "no flow" signal: pin ``fmin = fmax = 0`` on the emitted
    # JSON, deactivating the waterway in the LP.  Used for
    # ``Caudal_Eco_*`` / ``Riego_*`` / ``Ext_*`` diversion waterways
    # that ship with zero CSV AND no static Min/Max Flow — without it
    # the writer's ``if fmax > 0.0`` check leaves the arc uncapped and
    # the LP can discover it as a free water path (e.g. ``Ext_Maule``:
    # L_Maule -> LA_MINA, an uncapped diversion arc that the boundary-cut
    # water value currently keeps idle but does not structurally close).
    inactive: bool = False
    # PLEXOS ``Max Flow Penalty`` ($/(m³/s)/h or $/MWh-equivalent) —
    # per-flow cost on the waterway column.  Maps to gtopt's
    # ``Waterway.fcost``.  ``-1.0`` in PLEXOS means "feature disabled"
    # (sentinel), translated to 0 by the parser.  CEN PCP uses 3.6
    # on most Vert_* spillway arcs and 7200/360 on a handful of
    # high-cost gas-storage spillways.
    fcost: float = 0.0
    # When True (default), the ``forced_flow_profile`` is emitted as
    # BOTH ``fmin`` AND ``fmax`` — the LP physically pins the waterway
    # to the PLEXOS-mandated per-hour value (correct for irrigation
    # diversions, ecological flows, and filtration sinks where the
    # water IS removed at exactly that rate).  When False, the profile
    # is emitted as ``fmin`` only and ``fmax`` is left unbounded — the
    # LP can route ADDITIONAL flow above the minimum (correct for
    # bypass / overflow waterways like ``B_Maule`` where PLEXOS lets
    # surplus water flow naturally above the published Min Flow).
    pin_fmax_from_profile: bool = True


@dataclass(frozen=True)
class JunctionSpec:
    """A hydraulic node synthesised from a PLEXOS Storage object.

    gtopt models hydro topology as Junctions (nodes) joined by
    Waterways (edges), with Reservoirs attached at junctions. PLEXOS
    keeps Storage and topology fused — we explode them so each Storage
    becomes one Junction plus one Reservoir co-located there.

    ``drain_capacity`` and ``drain_cost`` are optional and carry the
    ``Waterway.Max Flow`` / ``Waterway.Max Flow Penalty`` from PLEXOS's
    ``Vert_<src>`` spillway arc when we collapse the legacy
    ``Vert_<src> → <src>_ocean`` (Waterway + synthetic ocean Junction)
    encoding into a single ``Junction{drain: true, drain_capacity,
    drain_cost}`` row on the source junction.  Both are optional —
    when None the LP-side default (``DblMax`` / ``0.0``) applies.
    """

    name: str
    drain: bool = False  # True for terminal cascades (water leaves the system)
    drain_capacity: float | None = None
    drain_cost: float | None = None


@dataclass(frozen=True)
class TurbineSpec:
    """A hydro turbine — Generator with a Head Storage membership.

    PLEXOS encodes turbines as Generator objects with a Storage link;
    gtopt has an explicit Turbine entity that references both the
    Generator (for power output on the electrical bus) and the
    upstream Reservoir (``main_reservoir``) for water balance.

    ``tail_reservoir_name`` captures PLEXOS's *Tail Storage* link when
    present — the downstream reservoir / junction the turbine
    discharges into.  The writer emits the turbine in built-in waterway
    mode (``junction_a`` = reservoir, ``junction_b`` = this tail) so the
    turbine carries its own flow arc; terminal plants with no tail get
    ``junction_a`` only and drain.
    """

    generator_name: str
    reservoir_name: str
    production_factor: float = 0.0  # MW per m³/s (Hydro_EfficiencyIncr fp_med)
    tail_reservoir_name: str | None = None


@dataclass(frozen=True)
class FlowSpec:
    """A natural inflow into a Junction (``Hydro_WaterFlows.csv`` row).

    gtopt's ``Flow`` class is a per-(scene, stage, block) discharge
    pushed into a Junction; we map per-PLEXOS-Storage inflow profiles
    to one Flow per Junction. Inflow units are m³/s.

    When ``fcost > 0`` the writer emits gtopt ``Flow.fcost`` so the
    LP column relaxes from the default hard ``flow = discharge``
    equality to a soft, costed slack column (unbounded above,
    ``[0, +inf)`` with penalty ``fcost``).  Used by the PLEXOS
    "Non-physical Inflow Penalty" → gtopt slack mapping: the
    reservoir gets a per-junction inflow slack the LP only activates
    when balance can't otherwise close.  ``discharge_profile`` is
    typically left empty for slack flows since the upper bound is
    intentionally uncapped.
    """

    name: str
    junction_name: str
    discharge_profile: tuple[float, ...] = field(default_factory=tuple)
    fcost: float = 0.0


@dataclass(frozen=True)
class ReserveSpec:
    """A reserve zone (``t_class[Reserve]``).

    PLEXOS distinguishes up- and down-reserve via a name-suffix
    convention in CEN PCP: ``_LW`` (lower / down) vs ``_RS`` (raise /
    spinning / up). ``Res_Requirement.csv`` uses a custom
    ``NAME, PATTERN, VALUE`` layout where PATTERN encodes the
    day-of-week + hour (e.g. ``"DO_1,H17"`` = day 1, hour 17). The
    parser collapses to the daily PCP horizon (DO_1 only) and builds
    a 24-element requirement profile.
    """

    object_id: int
    name: str
    ur_requirement: tuple[float, ...] = field(default_factory=tuple)
    dr_requirement: tuple[float, ...] = field(default_factory=tuple)
    #: Static PLEXOS ``Min Provision`` floor [MW] on total provided reserve,
    #: kept SEPARATE from the time-varying requirement so the LP enforces
    #: ``Σ pf·prov ≥ max(requirement, min)`` → ReserveZone.urmin / .drmin.
    ur_min_provision: float = 0.0
    dr_min_provision: float = 0.0
    eligible_generators: tuple[str, ...] = field(default_factory=tuple)
    # PLEXOS Reserve.Type encodes the ancillary-service class:
    # 1=Regulation, 2=Spinning, 3=Regulation Raise, 4=Regulation Lower,
    # 5=Replacement, 6=Tertiary.  Used by extract_reserve_provisions to
    # split per-generator provisions by type so the type-specific
    # Min Provision floors (Min Spinning Provision, Min Regulation
    # Provision, Min Replacement Provision) map to the correct urmin.
    plexos_type: int = 0  # 0 = unknown / no type set
    # Type tag derived from plexos_type for downstream grouping.
    # Values: "regulation", "spinning", "replacement", "tertiary", "other".
    type_tag: str = "other"
    # Shortage-penalty costs ($/MWh) on the reserve-balance row.  Mapped
    # to ``ReserveZone.urcost`` (raise/up direction: PLEXOS Type 1/2/3/
    # 5/6) and ``ReserveZone.drcost`` (lower/down: PLEXOS Type 4).  The
    # PLEXOS source property is one of ``Violation Cost`` / ``Shortage
    # Penalty`` / ``Penalty Cost`` / ``VoRS``  (CEN PCP exposes ``VoRS``
    # ``= Value of Reserve Shortage``).  Negative PLEXOS values (e.g.
    # the ``-1`` sentinel meaning "use default / disabled") are clamped
    # to ``0.0``.
    urcost: float = 0.0
    drcost: float = 0.0


@dataclass(frozen=True)
class DecisionVariableSpec:
    """A PLEXOS ``Decision Variable`` translated to a gtopt
    :class:`DecisionVariable`.

    PLEXOS auto-creates a free continuous LP variable for each
    Decision Variable object (class_id 72); constraint coefficients
    reference its value via ``Value Coefficient``. gtopt maps the same
    semantics through the small :file:`decision_variable.hpp` LP class,
    which materialises one column per (scenario, stage, block) with
    optional bounds and cost — and registers it for AMPL/UserConstraint
    resolution as ``decision_variable("X").value``.
    """

    name: str
    lower_bound: float | None = None
    upper_bound: float | None = None
    cost: float = 0.0


@dataclass(frozen=True)
class PlantSpec:
    """Declarative grouping of generator configuration variants of one
    physical plant.

    Mirrors the gtopt-side :file:`include/gtopt/plant.hpp` C++ struct.
    Emits up to three LP rows per stage natively, replacing the
    synthesised ``PlantCap_<stem>`` UserConstraints and the
    ``<plant>_Uniq`` mutex UCs that the converter used to ship:

      * ``plant_cap_<name>``    — Σ generation ≤ pmax  (per stage × block)
      * ``plant_commit_<name>`` — Σ commit_coeff·status ≤ n_units (per stage)
      * ``plant_uniq_<name>``   — Σ status ≤ 1         (per stage)

    A PlantSpec carries the variant names (strings — resolved to
    Generator names at LP build time), the aggregate ``pmax``, the
    per-plant unit count, and the ``uniq_mutex`` flag.
    """

    name: str
    generator_names: tuple[str, ...] = ()
    pmax: float | None = None
    n_units: int | None = None
    commit_coeffs: tuple[float, ...] = ()
    uniq_mutex: bool = False


@dataclass(frozen=True)
class UserConstraintSpec:
    """A PLEXOS ``Constraint`` object translated to a gtopt UserConstraint.

    The expression carries the LHS + operator + RHS as a single string
    in the AMPL-inspired grammar that
    :file:`include/gtopt/constraint_parser.hpp` consumes. ``penalty``
    is set when the PLEXOS constraint ships a ``Penalty Price``
    (turning the row into a soft constraint with a visible slack).

    ``active`` (default ``None`` ⇒ unset ⇒ gtopt's default = active)
    controls whether the constraint is enforced in the LP.  PLEXOS
    contingency constraints (post-trip reserve allocations, N-1
    security rows, etc.) are translated with ``active = False`` so
    they ship in the JSON for inspection / future contingency
    simulation but the monolithic deterministic LP skips them — they
    would otherwise be infeasible-as-written because PLEXOS models
    them with implicit slacks that only fire under simulated trips.
    """

    name: str
    expression: str
    penalty: float = 0.0
    description: str = ""
    active: bool | None = None
    # Per-block RHS override.  When supplied, gets serialised to the
    # gtopt-side ``user_constraint.rhs`` TB-schedule field and overrides
    # the scalar parsed from the inline ``<op> NUMBER`` tail of
    # ``expression``.  Empty tuple ⇒ no override (legacy scalar-RHS
    # behaviour).  Multi-day cases must already match the writer's
    # ``block_count`` / ``block_layout`` (24-element daily patterns
    # are tiled by ``n_days`` upstream).
    rhs_profile: tuple[float, ...] = ()
    # When ``True`` the constraint is a per-DAY budget: gtopt's
    # ``UserConstraint.daily_sum`` switches the LP from the default
    # one-row-per-block expansion to ONE row per 24 h day (the per-block
    # terms accumulate into a running row flushed at each day boundary).
    # Used for the PLEXOS ``RHS Day`` daily-energy budgets
    # (``RALCOramp_max_e1/e2``, ``CANUTILLARreserve``).
    daily_sum: bool = False
    # gtopt ``UserConstraint.constraint_type``.  ``"energy"`` makes each
    # block's contribution Δt-weighted (``coeff · Δt_b · col_b``) so a
    # ``daily_sum`` LHS becomes a daily ENERGY sum ``Σ_day gen·Δt`` [MWh]
    # matched against an energy budget [MWh].  Empty ⇒ gtopt default
    # (unweighted per-block / per-day count).
    constraint_type: str = ""
    # Typed constraint-family directive — replaces the legacy
    # name-regex / expression-substring classification (``_RegRange_``,
    # ``Gas_MaxOpDay``, MinProvision) and hardcoded soft-penalty ladder
    # that today live inside this converter (``_RESERVE_PROVISION_SUM_
    # PENALTY``, ``_HYDRO_UC_SOFT_PENALTY``, …).  When set, the
    # writer emits a ``directive: {kind, …}`` JSON sibling field that
    # ``UserConstraint::directive`` (gtopt-side) picks up — the
    # directive's payload (e.g. its own ``penalty``) wins over the
    # scalar ``UserConstraint.penalty`` at LP-build time so policy
    # lives in ONE auditable place.  See
    # ``include/gtopt/constraint_directive.hpp`` for the C++-side
    # schema and the AMPL/PAMPL modernization plan (2026-05-30) for
    # the migration rationale.  Default ``None`` = legacy emission
    # (scalar ``penalty`` only, behaviour unchanged).
    directive: ConstraintDirective | None = None


@dataclass(frozen=True)
class ConstraintDirective:
    """Typed constraint-family directive attached to a UserConstraintSpec.

    Mirrors the C++-side ``gtopt::ConstraintDirective`` struct
    (``include/gtopt/constraint_directive.hpp``).  Only the fields valid
    for ``kind`` are expected to be populated; everything else stays
    ``None``.

    Attributes:
        kind: One of ``regrange | reserve_prov_sum | daily_budget |
            hydro_floor | max_starts_window``.  Lowercase canonical name
            (matches the NamedEnum table on the C++ side).
        penalty: Soft-penalty override [$ / unit-of-violation].  Set for
            ``regrange`` / ``reserve_prov_sum`` (and any directive whose
            policy carries a per-kind cost).  Forbidden for
            ``hydro_floor`` / ``max_starts_window``.
        scope: Free-form classification tag (e.g. ``"fuel:GAS"``,
            ``"owner:CSF"``).  Used by ``daily_budget`` / ``hydro_floor``.
        window_hours: Rolling window length in HOURS.  Required and
            ``> 0`` for ``max_starts_window``; forbidden elsewhere.
    """

    kind: str
    penalty: float | None = None
    scope: str | None = None
    window_hours: int | None = None


@dataclass(frozen=True)
class FlowRightSpec:
    """An irrigation / environmental flow envelope on a Junction.

    PLEXOS encodes these as auxiliary rows in ``Hydro_AntucoBounds.csv``
    (Laja basin) or similar overlays. gtopt's :class:`FlowRight` LP
    class applies an fmin/fmax bound at a Junction over the run
    horizon, optionally annotated with a ``purpose`` string for
    diagnostics.
    """

    name: str
    junction_name: str | None
    purpose: str = "irrigation"
    fmin: float = 0.0  # >0 = enforce lower bound
    fmax: float = 0.0  # >0 = enforce upper bound
    # Soft kink point [m³/s].  When >0, the LP layer
    # (``flow_right_lp.cpp``) penalises any flow BELOW ``target`` at
    # ``fcost`` and rewards any flow ABOVE ``target`` at ``uvalue``.
    # When zero/unset, the column degenerates to a plain hard band
    # ``[fmin, fmax]`` with no fcost activation — so ``target`` must
    # be set for any soft-pin behaviour.
    target: float = 0.0
    # Per-block soft kink profile [m³/s].  When non-empty it OVERRIDES the
    # scalar ``target`` so a time-varying obligation (e.g. ``Riego_LAJA_I``
    # irrigates only the first day, then 0) is delivered block-by-block — a
    # scalar target would soft-force the peak every hour and over-remove
    # water.  Emitted by the writer as ``FlowRight.target = [[profile]]``.
    target_profile: tuple[float, ...] = ()
    # Per-(stage, block) slack-violation penalty in $/(m³/s)/h. The
    # writer broadcasts this scalar across the 24-block horizon when
    # emitting the FlowRight; matches gtopt's
    # ``OptTBRealFieldSched`` shape (same as ``Demand.fcost``).
    fcost: float = 0.0
    # Optional pass-through downstream junction.  When set, the
    # writer emits ``FlowRight.bypass_junction`` / ``bypass_cost`` on
    # the JSON entry — the LP layer (``flow_right_lp.cpp``) then adds
    # a per-block bypass column priced at ``bypass_cost·cf`` that
    # contributes -1 to ``junction_name``'s balance and +1 to
    # ``bypass_junction``'s, replacing the legacy synthetic
    # ``bypass_<name>`` Waterway pressure-release path.  Pure consumer
    # behaviour is preserved when this is left at None.
    bypass_junction: str | None = None
    bypass_cost: float = 0.0


@dataclass(frozen=True)
class CommitmentSpec:
    """Unit-commitment parameters for a thermal generator.

    Sourced from the six ``Gen_*.csv`` UC files in CEN PCP plus
    ``t_data`` fallbacks on the Generator System collection for
    Min Up/Down time and ramp limits. Mapped 1:1 onto gtopt's
    ``Commitment`` LP class (see ``json_commitment.hpp``).
    """

    generator_name: str
    startup_cost: float = 0.0
    shutdown_cost: float = 0.0
    min_up_time: float = 0.0
    min_down_time: float = 0.0
    initial_status: float = 0.0  # 1 = online at t=0, 0 = offline
    initial_hours: float = 0.0  # signed: + hours up, - hours down
    # Raw PLEXOS ``Gen_IniHoursUp.csv`` / ``Gen_IniHoursDown.csv``
    # values [h] — non-negative.  ``initial_status`` / ``initial_hours``
    # above is the collapsed view (signed, picking whichever of the
    # two pairs is the "active" side at t=0); these raw fields keep
    # the original pair so the converter round-trips PLEXOS input
    # faithfully.  ``None`` ⇒ CSV not present / no entry for this
    # generator (distinct from 0.0 which means "explicit zero hours").
    ini_hours_up: float | None = None
    ini_hours_down: float | None = None
    ramp_up: float = 0.0  # MW/h
    ramp_down: float = 0.0  # MW/h
    # Per-block ramp-up / ramp-down profiles (MW/h, length = bundle.n_days
    # × 24) from the ``CFdata/CPF`` curves, carried only when the curve
    # varies intra-horizon.  Emitted as gtopt ``Commitment.ramp_up`` /
    # ``ramp_down`` TB schedules; empty → the scalar above is used.
    ramp_up_profile: tuple[float, ...] = field(default_factory=tuple)
    ramp_down_profile: tuple[float, ...] = field(default_factory=tuple)
    # PLEXOS ``Min Stable Level`` (MW per-unit, when committed) maps
    # to gtopt's ``Commitment.pmin`` — distinct from
    # ``Generator.pmin`` (always-on hard floor).  Per PLEXOS docs
    # (ReserveGenerators.MinStableFactor): Min Stable Level applies
    # only when the unit is committed.
    pmin: float = 0.0  # MW (when-committed floor)
    # Per-period ``Min Stable Level`` profile (``Gen_MinStableLevel.csv``,
    # length = bundle.n_days × 24).  PLEXOS ships Min Stable Level as a
    # time series — CEN PCP coal units carry e.g. 98.53 MW for most of
    # the week and 170.53 MW for a few peak hours.  When this varies the
    # writer aggregates it to per-block values and emits ``pmin`` as a
    # vector schedule; otherwise the scalar ``pmin`` above is used.
    pmin_profile: tuple[float, ...] = field(default_factory=tuple)
    # PLEXOS ``Generator.Commit`` per-period forcing (Gen_Commit.csv),
    # one value per horizon hour, VALUE in {-1, 0, +1}:
    #   +1 = forced ON, 0 = forced OFF, -1 = MIP-endogenous (free).
    # The writer turns this into gtopt's ``Commitment.must_run`` (when
    # every value is +1) or per-block ``fixed_status`` (pins the ``u``
    # status variable to 1/0 and leaves -1 blocks free).  Empty = no
    # forcing.  Forcing is applied via the commitment variable only —
    # ``pmax`` is left untouched.
    commit_status_profile: tuple[int, ...] = field(default_factory=tuple)
    # PLEXOS ``Run Up Rate`` (MW/min) → gtopt's ``Commitment.startup_ramp``
    # (MW max output in the startup block).  Converted to MW by
    # multiplying by 60 (per-hour startup ramp).  11 generators in CEN
    # PCP carry this, mostly KELAR fuel variants.
    startup_ramp: float = 0.0  # MW (per-block startup envelope)
    # No-load cost ($/hr): fixed cost when the unit is committed,
    # independent of power output. For PLEXOS quadratic heat-rate
    # formulations, ``noload_cost = Heat Rate Base × Fuel Price``.
    noload_cost: float = 0.0
    # PLEXOS ``Generator.Initial Generation`` (``Gen_IniGeneration.csv``)
    # in MW.  Maps to gtopt's ``Commitment.initial_power`` — the
    # dispatch level at ``t = -1`` used in the first-block ramp /
    # commitment continuity rows:
    #   p[0] − initial_power ≤ RU·u_init + SU·(1 − u_init)
    #   initial_power − p[0] ≤ RD·u[0] + SD·w[0]
    # When 0 (the default), gtopt's legacy "cold-start" behaviour
    # (``p_prev = 0``) is preserved, which is correct for genuinely
    # offline units and for any case where PLEXOS didn't ship an
    # Initial Generation entry.  73 generators on CEN PCP weekly
    # 2026-04-22 carry a non-zero value (mostly hydro turbines at
    # the start of their dispatch window).
    initial_power: float = 0.0
    # PLEXOS ``Max Starts {Hour|Day|Week|Horizon}`` (prop_id 203..205,
    # plus the per-horizon prop 202).  Encoded onto gtopt's
    # ``Commitment.max_starts`` integer cap with an explicit scope
    # selector — see ``include/gtopt/commitment.hpp`` ``MaxStartsScope``
    # for the four resolved values (PLEXOS per-month / per-year collapse
    # to ``"horizon"`` on the gtopt side because typical gtopt stages
    # are shorter than a month).  ``max_starts = 0`` (the default)
    # means "no cap emitted".  When a generator carries non-zero values
    # at MULTIPLE scopes simultaneously, the converter picks the
    # tightest one applicable to the run horizon (see ``extract_
    # commitments`` for the priority rule).
    max_starts: int = 0
    # Symmetric ``min_starts`` floor (forces commitment, complement to
    # max_starts as a cap).  PLEXOS doesn't ship a Min Starts property
    # directly, but the same per-window LP-side primitive in gtopt
    # (commitment_lp.cpp C9) handles both bounds.  Populated only when a
    # converter / writer needs to inject a forced-commitment floor.
    # Default 0 → no lower row emitted (commitment LP treats unset as
    # +∞ on the cap side, 0 on the floor side).
    min_starts: int = 0
    starts_scope: str = ""  # "hour" | "day" | "week" | "horizon" (shared)


@dataclass(frozen=True)
class ReserveProvisionSpec:
    """A Generator's eligibility to provide reserve to one or more zones.

    PLEXOS exposes a Reserve→Generator membership table; gtopt's
    ``ReserveProvision`` is the inverse view (one row per Generator,
    listing the reserve zones it can serve).
    """

    generator_name: str
    reserve_zones: tuple[str, ...] = field(default_factory=tuple)
    # urmax / drmax cap the per-block up / down reserve column. PLEXOS
    # doesn't surface this directly; the converter defaults them to
    # the parent generator's pmax (the maximum possible provision MW)
    # so gtopt unconditionally materialises the LP column for any
    # user constraint that references it.
    urmax: float = 0.0
    drmax: float = 0.0
    # urmin / drmin floor the per-block up / down reserve column.
    # Sourced from PLEXOS Reserve→Generator Min Provision / Min
    # Spinning Provision / Min Replacement Provision properties (the
    # maximum of these across an up-direction reserve gives urmin;
    # likewise for down).  When > 0, gtopt's reserve_provision_lp
    # enforces ``provision_col >= urmin × ur_provision_factor``.
    urmin: float = 0.0
    drmin: float = 0.0
    # PLEXOS reserve type tag — one of "regulation" / "spinning" /
    # "replacement" / "tertiary" / "other".  When the converter emits
    # per-type provisions (Option B), this tag distinguishes the
    # provision rows for the same gen across types; the JSON name
    # becomes ``provision_<gen>_<type_tag>``.
    type_tag: str = "other"
    # Per-block up / down provision-factor schedules (length =
    # bundle.n_days × 24, p.u.).  Populated from ``SSCC_Activation_BESS.csv``
    # for the synthetic ``<battery>_gen`` providers: the per-time-pattern
    # ancillary-services activation fraction the BESS offers to each
    # ``*_BESS`` reserve zone.  Empty → the writer emits the scalar 1.0
    # default.  RS (raise) zones drive ``ur``; LW (lower) zones drive ``dr``.
    ur_provision_factor_profile: tuple[float, ...] = field(default_factory=tuple)
    dr_provision_factor_profile: tuple[float, ...] = field(default_factory=tuple)
    # Per-block up / down MAX RESERVE CAPABILITY (MW), from CEN's
    # ``CFdata/{CPF,CSF,CTF}/{CPF,CSF,CTFON}_<gen>_{MRU,MRD}.csv``
    # files.  These are the authoritative per-(gen, reserve-type, hour)
    # caps PLEXOS uses to bound its Reserve.Provision LP variables
    # (verified 2026-05-31: PLEXOS sol max == CFdata cap EXACTLY for
    # every binding (gen, reserve) pair on v0407).  When populated,
    # ``urmax_profile`` is the SUM of MRU across all up-reserve types
    # (CPF_RS + CSF_RS + CTF_RS) the gen participates in; likewise
    # ``drmax_profile`` for MRD across down-reserve types.  Empty →
    # the writer falls back to the scalar ``urmax`` / ``drmax``
    # (currently ``pmax``, an over-loose bound 2-16000× larger than
    # the CFdata caps).  Without the per-block profile, the LP
    # leaves ``reserve_provision.up`` / ``.dn`` unbounded above ⇒
    # PLEXOS-binding reserve UCs (``CPF_Up5Calculation`` -$84/MW,
    # ``CPF_UpMinProvision`` -$74, ``CSF_UpMinProvision`` -$43) never
    # bind in gtopt; also triggers the ``UPStorageBound_BAT_*`` -inf
    # dual cascade on BESS plants.
    urmax_profile: tuple[float, ...] = field(default_factory=tuple)
    drmax_profile: tuple[float, ...] = field(default_factory=tuple)
    # Optional explicit provision name (overrides the default
    # ``provision_<generator_name>``).  Used by the SSCC BESS mapping to
    # emit one provision per (battery, ``*_BESS`` zone) without colliding
    # on the shared ``<battery>_gen`` generator name.
    name: str = ""


@dataclass(frozen=True)
class BoundaryCutSpec:
    """One end-of-horizon future-cost (boundary) cut.

    Mirrors a single row of PLEXOS ``Hydro_StoWaterValues.csv``: the
    ``FCF`` value is the cut intercept (``rhs``); ``slopes`` maps each
    reservoir name to its water value ($/GWh).  Emitted to gtopt as a
    ``boundary_cuts.csv`` row ``scene,rhs,<res...>`` with coefficients
    ``-water_value`` (more stored water ⇒ lower future cost) on each
    reservoir's terminal-volume state variable.
    """

    fcf: float
    slopes: dict[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class UserConstraintStats:
    """Provenance of the user-constraint conversion, for the comparison report.

    Captures the funnel from raw PLEXOS ``Constraint`` objects to the
    ``UserConstraintSpec`` set carried on the case, so the PLEXOS↔gtopt
    comparison can attribute every lost constraint to a reason:

    ``raw_plexos_constraints``
        Number of PLEXOS ``Constraint`` objects in the bundle.
    ``empty_lhs_dropped``
        Constraints dropped during extraction because their LHS could not be
        faithfully represented (no supported terms, or a partial form after
        dropping unsupported coefficients).
    ``hydro_synthesized`` / ``plant_cap_synthesized``
        UserConstraints synthesised by the converter (hydro daily-ramp /
        discharge rows; combined-cycle ``*_Uniq`` config-exclusivity caps)
        that do not originate from a single PLEXOS ``Constraint`` object.
    """

    raw_plexos_constraints: int = 0
    empty_lhs_dropped: int = 0
    base_emitted: int = 0
    hydro_synthesized: int = 0
    plant_cap_synthesized: int = 0


@dataclass(frozen=True)
class PlexosCase:
    """The full set of parsed entities for one bundle.

    The writer takes this as a single argument and emits one gtopt
    planning JSON.
    """

    bundle: BundleSpec
    nodes: tuple[NodeSpec, ...] = field(default_factory=tuple)
    fuels: tuple[FuelSpec, ...] = field(default_factory=tuple)
    generators: tuple[GeneratorSpec, ...] = field(default_factory=tuple)
    lines: tuple[LineSpec, ...] = field(default_factory=tuple)
    demands: tuple[DemandSpec, ...] = field(default_factory=tuple)
    batteries: tuple[BatterySpec, ...] = field(default_factory=tuple)
    reservoirs: tuple[ReservoirSpec, ...] = field(default_factory=tuple)
    waterways: tuple[WaterwaySpec, ...] = field(default_factory=tuple)
    junctions: tuple[JunctionSpec, ...] = field(default_factory=tuple)
    turbines: tuple[TurbineSpec, ...] = field(default_factory=tuple)
    flows: tuple[FlowSpec, ...] = field(default_factory=tuple)
    reserves: tuple[ReserveSpec, ...] = field(default_factory=tuple)
    reserve_provisions: tuple[ReserveProvisionSpec, ...] = field(default_factory=tuple)
    commitments: tuple[CommitmentSpec, ...] = field(default_factory=tuple)
    flow_rights: tuple[FlowRightSpec, ...] = field(default_factory=tuple)
    decision_variables: tuple[DecisionVariableSpec, ...] = field(default_factory=tuple)
    plants: tuple[PlantSpec, ...] = field(default_factory=tuple)
    user_constraints: tuple[UserConstraintSpec, ...] = field(default_factory=tuple)
    # Single end-of-horizon boundary (future-cost) cut from PLEXOS
    # ``Hydro_StoWaterValues.csv``: FCF intercept + per-reservoir water
    # values (the SDDP terminal value function).  ``None`` when the
    # bundle ships no water-value file.
    boundary_cut: BoundaryCutSpec | None = None
    # User-constraint conversion funnel (raw → dropped → synthesised), used
    # by the PLEXOS↔gtopt comparison report.  ``None`` until populated by
    # ``extract_case``.
    uc_stats: UserConstraintStats | None = None
    # Raw PLEXOS object counts per class (``len(db.objects_of_class(...))``),
    # captured before extraction-time drops/demotions so the comparison can
    # show a conversion drop funnel (e.g. Line 344 → 317 emitted).  Empty
    # until populated by ``extract_case``.
    raw_class_counts: dict[str, int] = field(default_factory=dict)
    # PHYSICAL nameplate (PLEXOS ``Max Capacity`` [MW], all units) keyed by
    # generator name — captured UNCONDITIONALLY, even for units PLEXOS holds
    # offline for the week (whose dispatch ``pmax`` is 0).  Feeds the
    # reservoir extraction-flow estimator so the ``fmax`` bound reflects full
    # turbine capacity; NOT written into the output JSON.  Empty until
    # populated by ``extract_case``.
    generator_nameplates: dict[str, float] = field(default_factory=dict)
    # Turbine JSON dicts (same shape as ``system["turbine_array"]`` entries:
    # ``name`` / ``generator`` / ``junction_a`` / ``junction_b`` /
    # ``production_factor`` / ``uid`` / optional ``waterway``) for units PLEXOS
    # holds offline for the week (dispatch ``pmax = 0``) whose turbines were
    # dropped from ``turbines`` to avoid a free-drain LP artefact.  Fed to the
    # reservoir extraction-flow estimator (counted for the ``fmax`` BOUND only)
    # so a maintenance-offline second unit's nameplate still sizes the reservoir
    # bound; NEVER written into the output JSON ``turbine_array``.  Empty until
    # populated by ``extract_case``.
    extra_turbines: list[dict[str, Any]] = field(default_factory=list)
