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
    pmin: float = 0.0
    pmax: float = 0.0
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


@dataclass(frozen=True)
class LineSpec:
    """A transmission line (``t_class[Line]``)."""

    object_id: int
    name: str
    bus_from: str
    bus_to: str
    tmax_ab: float = 0.0
    tmin_ab: float = 0.0
    # PLEXOS Enforce Limits (0=Never, 1=Voltage, 2=Always) controls
    # whether the LP applies thermal limits.  Per the Energy Exemplar
    # docs (Line.EnforceLimits): "0=Never → line thermal limits are
    # never enforced; the LP allows unbounded flow with no violation
    # cost".  The converter maps this onto gtopt's soft/hard limit
    # pair:
    #   * EL=0 → drop the hard cap (tmax = +inf via unset)
    #   * EL=1/2 → keep the hard cap (current default behaviour)
    # CEN PCP carries 185 lines at EL=0, 96 at EL=1, 63 at EL=2.
    enforce_limits: int = 1  # default = "enforce" (safer than allowing unbounded)
    units: int = 1
    reactance: float = 0.0
    wheeling_charge: float = 0.0


@dataclass(frozen=True)
class DemandSpec:
    """A consumer attached to a bus (one per non-zero column in ``Nod_Load.csv``)."""

    name: str
    bus_name: str
    lmax_profile: tuple[float, ...] = field(default_factory=tuple)


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
    input_efficiency: float = 1.0
    output_efficiency: float = 1.0
    # PLEXOS Min Charge / Min Discharge Level (MW): minimum power when
    # actively charging / discharging.  Maps to gtopt's
    # ``Battery.pmin_charge`` / ``pmin_discharge``.  CEN PCP carries
    # these only on 2 batteries (BAT_DEL_DESIERTO, BAT_TOCOPILLA).
    pmin_charge: float = 0.0
    pmin_discharge: float = 0.0


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
    spill_penalty_per_mwh: float = 0.0
    inflow_profile: tuple[float, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class WaterwaySpec:
    """A waterway / spillway (``t_class[Waterway]``, class_id=9)."""

    object_id: int
    name: str
    storage_from: str | None = None
    storage_to: str | None = None
    fmin: float = 0.0
    fmax: float = 0.0
    # PLEXOS ``Max Flow Penalty`` ($/(m³/s)/h or $/MWh-equivalent) —
    # per-flow cost on the waterway column.  Maps to gtopt's
    # ``Waterway.fcost``.  ``-1.0`` in PLEXOS means "feature disabled"
    # (sentinel), translated to 0 by the parser.  CEN PCP uses 3.6
    # on most Vert_* spillway arcs and 7200/360 on a handful of
    # high-cost gas-storage spillways.
    fcost: float = 0.0


@dataclass(frozen=True)
class JunctionSpec:
    """A hydraulic node synthesised from a PLEXOS Storage object.

    gtopt models hydro topology as Junctions (nodes) joined by
    Waterways (edges), with Reservoirs attached at junctions. PLEXOS
    keeps Storage and topology fused — we explode them so each Storage
    becomes one Junction plus one Reservoir co-located there.
    """

    name: str
    drain: bool = False  # True for terminal cascades (water leaves the system)


@dataclass(frozen=True)
class TurbineSpec:
    """A hydro turbine — Generator with a Head Storage membership.

    PLEXOS encodes turbines as Generator objects with a Storage link;
    gtopt has an explicit Turbine entity that references both the
    Generator (for power output on the electrical bus) and the
    upstream Reservoir (``main_reservoir``) for water balance.
    """

    generator_name: str
    reservoir_name: str
    production_factor: float = 0.0  # MW per m³/s (Hydro_EfficiencyIncr fp_med)


@dataclass(frozen=True)
class FlowSpec:
    """A natural inflow into a Junction (``Hydro_WaterFlows.csv`` row).

    gtopt's ``Flow`` class is a per-(scene, stage, block) discharge
    pushed into a Junction; we map per-PLEXOS-Storage inflow profiles
    to one Flow per Junction. Inflow units are m³/s.
    """

    name: str
    junction_name: str
    discharge_profile: tuple[float, ...] = field(default_factory=tuple)


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
class UserConstraintSpec:
    """A PLEXOS ``Constraint`` object translated to a gtopt UserConstraint.

    The expression carries the LHS + operator + RHS as a single string
    in the AMPL-inspired grammar that
    :file:`include/gtopt/constraint_parser.hpp` consumes. ``penalty``
    is set when the PLEXOS constraint ships a ``Penalty Price``
    (turning the row into a soft constraint with a visible slack).
    """

    name: str
    expression: str
    penalty: float = 0.0
    description: str = ""


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
    # Per-(stage, block) slack-violation penalty in $/(m³/s)/h. The
    # writer broadcasts this scalar across the 24-block horizon when
    # emitting the FlowRight; matches gtopt's
    # ``OptTBRealFieldSched`` shape (same as ``Demand.fcost``).
    fcost: float = 0.0


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
    ramp_up: float = 0.0  # MW/h
    ramp_down: float = 0.0  # MW/h
    # PLEXOS ``Min Stable Level`` (MW per-unit, when committed) maps
    # to gtopt's ``Commitment.pmin`` — distinct from
    # ``Generator.pmin`` (always-on hard floor).  Per PLEXOS docs
    # (ReserveGenerators.MinStableFactor): Min Stable Level applies
    # only when the unit is committed.
    pmin: float = 0.0  # MW (when-committed floor)
    # PLEXOS ``Run Up Rate`` (MW/min) → gtopt's ``Commitment.startup_ramp``
    # (MW max output in the startup block).  Converted to MW by
    # multiplying by 60 (per-hour startup ramp).  11 generators in CEN
    # PCP carry this, mostly KELAR fuel variants.
    startup_ramp: float = 0.0  # MW (per-block startup envelope)
    # No-load cost ($/hr): fixed cost when the unit is committed,
    # independent of power output. For PLEXOS quadratic heat-rate
    # formulations, ``noload_cost = Heat Rate Base × Fuel Price``.
    noload_cost: float = 0.0


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
    user_constraints: tuple[UserConstraintSpec, ...] = field(default_factory=tuple)
