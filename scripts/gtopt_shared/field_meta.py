# SPDX-License-Identifier: BSD-3-Clause
"""Canonical per-array field metadata for the gtopt JSON contract.

Pure data — no executable logic.  Each entry in ``FIELD_META`` is a list of
``(field, json_type, required, description, example)`` tuples that drives the
header / help / example rows of the corresponding worksheet (e.g. the
``bus_array`` sheet uses ``FIELD_META["bus_array"]``).  Lifted to
``gtopt_shared/`` because it's the canonical source of truth for entity
field schemas across all five planning-writer converters, and will be
the schema source for the Phase 4 entity-builder unification work.

Lifted from ``igtopt/_field_meta.py`` to ``gtopt_shared/`` on 2026-06-06
as part of issue #507 Phase 1.  The original ``igtopt._field_meta``
module remains as a thin re-export shim for backward compatibility.
"""

from __future__ import annotations

from typing import Any

_J_INT = "integer"
_J_STR = "string"
_J_NUM = "number"
_J_BOOL = "boolean (true/false)"
_J_ID = "integer or string (uid or name)"
_J_SCHED = "number | array | filename"

# Each entry: (json_type, required, description, example_value_or_None)
_COMMON_ID_FIELDS: list[tuple[str, str, bool, str, Any]] = [
    ("uid", _J_INT, True, "Unique numeric identifier", 1),
    ("name", _J_STR, True, "Human-readable element name", "elem1"),
    ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
]

#: Per-array field metadata.
#: Keys match the JSON array names (e.g. "bus_array").
#: Values are lists of (field, json_type, required, description, example).
FIELD_META: dict[str, list[tuple[str, str, bool, str, Any]]] = {
    # ------------------------------------------------------------------
    # Simulation
    # ------------------------------------------------------------------
    "block_array": [
        ("uid", _J_INT, True, "Unique block identifier", 1),
        ("name", _J_STR, False, "Optional block label", "b1"),
        (
            "duration",
            _J_NUM,
            True,
            "Block duration in hours (e.g. 1.0 for hourly)",
            1.0,
        ),
    ],
    "stage_array": [
        ("uid", _J_INT, True, "Unique stage identifier", 1),
        ("name", _J_STR, False, "Optional stage label", "s1"),
        ("active", _J_BOOL, False, "1 = active stage (default: 1)", None),
        (
            "first_block",
            _J_INT,
            True,
            "0-based index of the first block belonging to this stage",
            0,
        ),
        ("count_block", _J_INT, True, "Number of blocks in this stage", 8760),
        (
            "discount_factor",
            _J_NUM,
            False,
            "Discount factor applied to this stage's costs (default: 1.0)",
            1.0,
        ),
        (
            "month",
            _J_STR,
            False,
            "Calendar month tag (jan, feb, …, dec). Drives seasonal "
            "parameter lookups (e.g. irrigation schedules, monthly user_param).",
            None,
        ),
        (
            "chronological",
            _J_BOOL,
            False,
            "When true, blocks in this stage are chronologically ordered and "
            "unit-commitment startup/shutdown transitions are enforced. "
            "False / unset = commitment transitions silently skipped.",
            None,
        ),
    ],
    "scenario_array": [
        ("uid", _J_INT, True, "Unique scenario identifier", 1),
        ("name", _J_STR, False, "Optional scenario label", "sc1"),
        ("active", _J_BOOL, False, "1 = active scenario (default: 1)", None),
        (
            "probability_factor",
            _J_NUM,
            False,
            "Probability weight of this scenario (default: 1.0)",
            1.0,
        ),
        (
            "hydrology",
            _J_INT,
            False,
            "Optional 0-based PLP hydrology class index (metadata for "
            "provenance / post-processing; not consumed by the LP solver).",
            None,
        ),
    ],
    "phase_array": [
        ("uid", _J_INT, True, "Unique phase identifier (SDDP)", 1),
        ("name", _J_STR, False, "Optional phase label", "ph1"),
        ("active", _J_BOOL, False, "1 = active (default: 1)", None),
        (
            "first_stage",
            _J_INT,
            True,
            "0-based index of the first stage in this phase",
            0,
        ),
        ("count_stage", _J_INT, True, "Number of stages in this phase", 1),
        (
            "apertures",
            _J_STR,
            False,
            "Comma-separated aperture UIDs for this phase (empty = use all global apertures)",
            None,
        ),
        (
            "continuous",
            _J_BOOL,
            False,
            "If true, integer variables in this phase are relaxed to continuous "
            "(LP relaxation). Defaults to false; can also be set globally via "
            "model_options.continuous_phases.",
            None,
        ),
    ],
    "scene_array": [
        ("uid", _J_INT, True, "Unique scene identifier (SDDP)", 1),
        ("name", _J_STR, False, "Optional scene label", "sc1"),
        ("active", _J_BOOL, False, "1 = active (default: 1)", None),
        (
            "first_scenario",
            _J_INT,
            True,
            "0-based index of the first scenario in this scene",
            0,
        ),
        (
            "count_scenario",
            _J_INT,
            True,
            "Number of scenarios in this scene",
            1,
        ),
    ],
    # ------------------------------------------------------------------
    # System — network
    # ------------------------------------------------------------------
    "bus_array": [
        ("uid", _J_INT, True, "Unique bus identifier", 1),
        ("name", _J_STR, True, "Bus name (used to reference this bus)", "b1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            "Element type tag (free-form metadata; not used by the LP).",
            None,
        ),
        ("voltage", _J_NUM, False, "Nominal voltage level [kV]", 220.0),
        (
            "reference_theta",
            _J_NUM,
            False,
            "Fixed voltage angle for the reference bus [rad] (default: none)",
            None,
        ),
        (
            "use_kirchhoff",
            _J_BOOL,
            False,
            "Override global Kirchhoff setting for this bus (true/false)",
            None,
        ),
    ],
    "line_array": [
        ("uid", _J_INT, True, "Unique line identifier", 1),
        ("name", _J_STR, True, "Line name", "l1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "bus_a",
            _J_ID,
            True,
            "Sending-end bus uid or name",
            "b1",
        ),
        (
            "bus_b",
            _J_ID,
            True,
            "Receiving-end bus uid or name",
            "b2",
        ),
        ("voltage", _J_SCHED, False, "Line voltage [kV]", None),
        ("resistance", _J_SCHED, False, "Series resistance [Ω] (for losses)", None),
        (
            "reactance",
            _J_SCHED,
            False,
            "Series reactance [p.u.] (required for Kirchhoff DC OPF)",
            None,
        ),
        (
            "type",
            _J_STR,
            False,
            'Element type tag; use "transformer" for tap-changing / PST branches',
            None,
        ),
        (
            "tap_ratio",
            _J_SCHED,
            False,
            "Off-nominal tap ratio τ [p.u.] (default 1.0); "
            "effective susceptance = B/τ (per-stage schedule supported)",
            None,
        ),
        (
            "phase_shift_deg",
            _J_SCHED,
            False,
            "Phase-shift angle φ [degrees] for PSTs (default 0); "
            "shifts Kirchhoff RHS by -σ_θ·φ_rad (per-stage schedule supported)",
            None,
        ),
        (
            "lossfactor",
            _J_SCHED,
            False,
            "Linear loss factor [p.u.] (fraction of flow lost)",
            None,
        ),
        (
            "use_line_losses",
            _J_BOOL,
            False,
            "Enable resistive loss model for this line (default: global setting)",
            None,
        ),
        (
            "loss_segments",
            _J_INT,
            False,
            "Number of piecewise-linear loss segments (>1 enables quadratic model)",
            None,
        ),
        (
            "loss_allocation_mode",
            _J_STR,
            False,
            'How losses are allocated: "receiver" (default), "sender", or "split" (50/50)',
            None,
        ),
        ("tmax_ba", _J_SCHED, False, "Max power flow B→A [MW]", 500.0),
        ("tmax_ab", _J_SCHED, False, "Max power flow A→B [MW]", 500.0),
        (
            "tmax_normal_ba",
            _J_SCHED,
            False,
            "Soft (normal) flow threshold B→A [MW]. When set together with "
            "overload_penalty, flow above this threshold is priced (still "
            "capped at tmax_ba).",
            None,
        ),
        (
            "tmax_normal_ab",
            _J_SCHED,
            False,
            "Soft (normal) flow threshold A→B [MW] — analogous to tmax_normal_ba.",
            None,
        ),
        (
            "overload_penalty",
            _J_SCHED,
            False,
            "Overload penalty above tmax_normal_* [$/MWh] per-(stage, block).",
            None,
        ),
        (
            "line_losses_mode",
            _J_STR,
            False,
            "Per-line loss-model override: 'none', 'linear', 'piecewise', "
            "'bidirectional', 'adaptive', 'dynamic'. When unset, inherits "
            "from ModelOptions.",
            None,
        ),
        ("tcost", _J_SCHED, False, "Transmission cost [$/MWh]", None),
        ("capacity", _J_SCHED, False, "Initial installed capacity [MW]", None),
        (
            "expcap",
            _J_SCHED,
            False,
            "Capacity per expansion module [MW/module]",
            None,
        ),
        (
            "expmod",
            _J_SCHED,
            False,
            "Maximum number of expansion modules (null = no expansion)",
            None,
        ),
        (
            "integer_expmod",
            _J_BOOL,
            False,
            "When true, restrict the expansion-module count to integer values "
            "(otherwise continuous; default: false).",
            None,
        ),
        (
            "capmax",
            _J_SCHED,
            False,
            "Absolute maximum capacity after expansion [MW]",
            None,
        ),
        (
            "annual_capcost",
            _J_SCHED,
            False,
            "Annualized investment cost [$/MW-year]",
            None,
        ),
        (
            "annual_derating",
            _J_SCHED,
            False,
            "Annual capacity derating factor [p.u./year]",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — generation
    # ------------------------------------------------------------------
    "generator_array": [
        ("uid", _J_INT, True, "Unique generator identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Generator name (used in profile and output files)",
            "g1",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "description",
            _J_STR,
            False,
            "Free-form label for UI/post-processing; not used by the LP solver.",
            None,
        ),
        (
            "type",
            _J_STR,
            False,
            "Element type tag (free-form metadata; not used by the LP).",
            None,
        ),
        ("bus", _J_ID, True, "Connected bus uid or name", "b1"),
        ("pmin", _J_SCHED, False, "Minimum active power output [MW]", 0.0),
        ("pmax", _J_SCHED, False, "Maximum active power output [MW]", 100.0),
        (
            "lossfactor",
            _J_SCHED,
            False,
            "Network loss factor [p.u.] (default: 0)",
            None,
        ),
        (
            "gcost",
            _J_SCHED,
            False,
            "Variable generation cost [$/MWh]",
            30.0,
        ),
        (
            "fuel",
            _J_ID,
            False,
            "Optional FK to a Fuel element. When set with heat_rate, "
            "per-MWh fuel cost = Fuel.price × heat_rate.",
            None,
        ),
        (
            "heat_rate",
            _J_SCHED,
            False,
            "Per-(stage, block) heat-rate slope [<fuel_unit>/MWh]; combines "
            "with Fuel.price to produce a per-MWh generation cost.",
            None,
        ),
        (
            "pmax_segments",
            "JSON array",
            False,
            "Piecewise-linear cumulative power breakpoints [MW] for convex "
            "heat-rate functions. Mutually exclusive with heat_rate.",
            None,
        ),
        (
            "heat_rate_segments",
            "JSON array",
            False,
            "Piecewise-linear heat-rate per segment [<fuel_unit>/MWh].",
            None,
        ),
        (
            "emission_rate",
            _J_SCHED,
            False,
            "Direct CO2 emission rate [tCO2/MWh] per-(stage, block); "
            "additive with fuel-derived combustion+upstream emissions.",
            None,
        ),
        (
            "emissions",
            "JSON array",
            False,
            "Inline list of EmissionSource rows scoped to this generator "
            "(each with zone + emission FKs and per-MWh rate / upstream_rate).",
            None,
        ),
        (
            "emission_captures",
            "JSON array",
            False,
            "Inline list of EmissionCapture (CCS / abatement) rows scoped "
            "to this generator.",
            None,
        ),
        ("capacity", _J_SCHED, False, "Initial installed capacity [MW]", None),
        (
            "expcap",
            _J_SCHED,
            False,
            "Capacity per expansion module [MW/module]",
            None,
        ),
        (
            "expmod",
            _J_SCHED,
            False,
            "Maximum number of expansion modules",
            None,
        ),
        (
            "integer_expmod",
            _J_BOOL,
            False,
            "When true, restrict the expansion-module count to integer values "
            "(otherwise continuous; default: false).",
            None,
        ),
        (
            "capmax",
            _J_SCHED,
            False,
            "Absolute maximum capacity after expansion [MW]",
            None,
        ),
        (
            "annual_capcost",
            _J_SCHED,
            False,
            "Annualized investment cost [$/MW-year]",
            None,
        ),
        (
            "annual_derating",
            _J_SCHED,
            False,
            "Annual capacity derating factor [p.u./year]",
            None,
        ),
    ],
    "generator_profile_array": [
        ("uid", _J_INT, True, "Unique generator-profile identifier", 1),
        ("name", _J_STR, True, "Profile name", "gp1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "generator",
            _J_ID,
            True,
            "Generator uid or name that this profile applies to",
            "g1",
        ),
        (
            "profile",
            _J_SCHED,
            True,
            "Capacity-factor values [0-1]: scalar, array, or filename "
            "(e.g. GeneratorProfile@profile sheet → 'profile')",
            "profile",
        ),
        (
            "scost",
            _J_SCHED,
            False,
            "Spilling cost when output is curtailed below profile [$/MWh]",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — capacity profiles (unified replacement for
    # generator_profile / demand_profile, owner-kind-tagged)
    # ------------------------------------------------------------------
    "capacity_profile_array": [
        ("uid", _J_INT, True, "Unique capacity-profile identifier", 1),
        ("name", _J_STR, True, "Profile name", "cp1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "owner_kind",
            _J_STR,
            True,
            "Owner element type: 'generator' or 'demand'.",
            "generator",
        ),
        (
            "owner",
            _J_ID,
            True,
            "FK to the owning element (generator or demand) by uid or name.",
            "g3",
        ),
        (
            "profile",
            _J_SCHED,
            True,
            "Capacity-factor profile [p.u.] — per-(scenario, stage, block); "
            "scalar, array, or filename.",
            "profile",
        ),
        (
            "scost",
            _J_SCHED,
            False,
            "Slack column unit cost override [$/MWh].",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — fuels (referenced by Generator.fuel / Commitment.fuel)
    # ------------------------------------------------------------------
    "fuel_array": [
        ("uid", _J_INT, True, "Unique fuel identifier", 1),
        ("name", _J_STR, True, "Fuel name (e.g. 'natgas', 'diesel')", "natgas"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "emission",
            _J_ID,
            False,
            "Legacy single-pollutant shortcut: Emission uid or name. Folded "
            "into `emission_factors[]` together with `combustion` / `upstream`.",
            None,
        ),
        (
            "combustion",
            _J_SCHED,
            False,
            "Legacy single-pollutant combustion (tank-to-stack) emission "
            "factor [t/<fuel_unit>]. Used with `emission`; folded into "
            "`emission_factors[]` at parse time.",
            None,
        ),
        (
            "upstream",
            _J_SCHED,
            False,
            "Legacy single-pollutant upstream (well-to-tank) emission factor "
            "[t/<fuel_unit>]. Used with `emission`; folded into "
            "`emission_factors[]` at parse time.",
            None,
        ),
        (
            "price",
            _J_SCHED,
            False,
            "Fuel price [$/<fuel_unit>], stage-schedulable.",
            None,
        ),
        (
            "heat_content",
            _J_SCHED,
            False,
            "Heat content [GJ/<fuel_unit>], stage-schedulable (optional; "
            "enables physical/energy fuel-consumption reporting).",
            None,
        ),
        (
            "combustion_emission_factor",
            _J_SCHED,
            False,
            "Legacy single-pollutant combustion CO2 factor "
            "[tCO2/<fuel_unit>]. Folded into emission_factors[] at parse time.",
            None,
        ),
        (
            "upstream_emission_factor",
            _J_SCHED,
            False,
            "Legacy single-pollutant upstream CO2 factor [tCO2/<fuel_unit>].",
            None,
        ),
        (
            "emission_factors",
            "JSON array",
            False,
            "Multi-pollutant per-fuel emission factors. JSON array of "
            "{emission, combustion [t/<fuel_unit>], upstream [t/<fuel_unit>]}.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — commitment (unit-commitment data linked to Generator)
    # ------------------------------------------------------------------
    "commitment_array": [
        ("uid", _J_INT, True, "Unique commitment identifier", 1),
        ("name", _J_STR, True, "Commitment name (e.g. 'thermal1_uc')", "uc1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("generator", _J_ID, True, "Generator uid or name", "g1"),
        (
            "pmin",
            _J_NUM,
            False,
            "Minimum output when committed [MW]. When set, the LP enforces "
            "generation >= pmin × u; otherwise the linked Generator's `pmin` "
            "is gated by u in the usual way.",
            None,
        ),
        (
            "startup_cost",
            _J_SCHED,
            False,
            "Startup cost [$/start], stage-schedulable.",
            None,
        ),
        (
            "shutdown_cost",
            _J_SCHED,
            False,
            "Shutdown cost [$/stop], stage-schedulable.",
            None,
        ),
        (
            "noload_cost",
            _J_NUM,
            False,
            "No-load cost when committed [$/h].",
            None,
        ),
        ("min_up_time", _J_NUM, False, "Minimum up time [h].", None),
        ("min_down_time", _J_NUM, False, "Minimum down time [h].", None),
        ("ramp_up", _J_NUM, False, "Ramp-up limit while online [MW/h].", None),
        (
            "ramp_down",
            _J_NUM,
            False,
            "Ramp-down limit while online [MW/h].",
            None,
        ),
        (
            "startup_ramp",
            _J_NUM,
            False,
            "Max output in startup block [MW].",
            None,
        ),
        (
            "shutdown_ramp",
            _J_NUM,
            False,
            "Max output in shutdown block [MW].",
            None,
        ),
        (
            "initial_status",
            _J_NUM,
            False,
            "Initial on/off status (1.0 = online, 0.0 = offline).",
            None,
        ),
        (
            "initial_hours",
            _J_NUM,
            False,
            "Hours in current state at t=0 [h].",
            None,
        ),
        (
            "relax",
            _J_BOOL,
            False,
            "LP relaxation: u/v/w continuous in [0,1].",
            None,
        ),
        ("must_run", _J_BOOL, False, "Force committed: u = 1 always.", None),
        (
            "fixed_status",
            _J_SCHED,
            False,
            "Per-(stage, block) forced commitment status: 1.0 = committed, "
            "0.0 = not committed. Pins the u variable.",
            None,
        ),
        (
            "commitment_period",
            _J_NUM,
            False,
            "Binary variable period [h]. When set, u/v/w live at a coarser "
            "time resolution than the generation blocks.",
            None,
        ),
        (
            "hot_start_cost",
            _J_NUM,
            False,
            "Startup cost when recently offline [$/start].",
            None,
        ),
        (
            "warm_start_cost",
            _J_NUM,
            False,
            "Startup cost at medium offline duration [$/start].",
            None,
        ),
        (
            "cold_start_cost",
            _J_NUM,
            False,
            "Startup cost when long offline [$/start].",
            None,
        ),
        (
            "hot_start_time",
            _J_NUM,
            False,
            "Max offline hours for hot start [h].",
            None,
        ),
        (
            "cold_start_time",
            _J_NUM,
            False,
            "Min offline hours for cold start [h].",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — simple_commitment (minimal UC: pmin gating)
    # ------------------------------------------------------------------
    "simple_commitment_array": [
        ("uid", _J_INT, True, "Unique simple-commitment identifier", 1),
        ("name", _J_STR, True, "SimpleCommitment name", "sc1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("generator", _J_ID, True, "Generator uid or name", "g1"),
        (
            "dispatch_pmin",
            _J_SCHED,
            False,
            "Minimum output when dispatched [MW] per-(stage, block).",
            None,
        ),
        (
            "relax",
            _J_BOOL,
            False,
            "LP relaxation: u continuous in [0,1].",
            None,
        ),
        ("must_run", _J_BOOL, False, "Force committed: u = 1 always.", None),
    ],
    # ------------------------------------------------------------------
    # System — demand
    # ------------------------------------------------------------------
    "demand_array": [
        ("uid", _J_INT, True, "Unique demand identifier", 1),
        ("name", _J_STR, True, "Demand name (used in profile and output files)", "d1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            "Element type tag (free-form metadata; not used by the LP).",
            None,
        ),
        ("bus", _J_ID, True, "Connected bus uid or name", "b1"),
        (
            "lmax",
            _J_SCHED,
            False,
            "Maximum demand (load) [MW]: scalar, array, or filename",
            100.0,
        ),
        (
            "lmin",
            _J_SCHED,
            False,
            "Minimum served load [MW] per-(stage, block) — forces dispatch "
            ">= lmin every block.",
            None,
        ),
        (
            "lossfactor",
            _J_SCHED,
            False,
            "Network loss factor [p.u.] (default: 0)",
            None,
        ),
        (
            "fcost",
            _J_SCHED,
            False,
            "Failure cost for unserved load [$/MWh] (overrides global demand_fail_cost)",
            None,
        ),
        (
            "forced",
            _J_BOOL,
            False,
            "When true, load is fixed at lmax (PLP-style forced load).",
            None,
        ),
        (
            "emin",
            _J_SCHED,
            False,
            "Minimum energy that must be served per stage [MWh]",
            None,
        ),
        (
            "ecost",
            _J_SCHED,
            False,
            "Energy storage cost for demand-side storage [$/MWh]",
            None,
        ),
        ("capacity", _J_SCHED, False, "Initial installed demand capacity [MW]", None),
        (
            "expcap",
            _J_SCHED,
            False,
            "Demand capacity per expansion module [MW/module]",
            None,
        ),
        ("expmod", _J_SCHED, False, "Maximum number of expansion modules", None),
        (
            "integer_expmod",
            _J_BOOL,
            False,
            "When true, restrict the expansion-module count to integer values "
            "(otherwise continuous; default: false).",
            None,
        ),
        (
            "capmax",
            _J_SCHED,
            False,
            "Absolute maximum demand capacity after expansion [MW]",
            None,
        ),
        (
            "annual_capcost",
            _J_SCHED,
            False,
            "Annualized investment cost [$/MW-year]",
            None,
        ),
        (
            "annual_derating",
            _J_SCHED,
            False,
            "Annual capacity derating factor [p.u./year]",
            None,
        ),
    ],
    "demand_profile_array": [
        ("uid", _J_INT, True, "Unique demand-profile identifier", 1),
        ("name", _J_STR, True, "Profile name", "dp1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "demand",
            _J_ID,
            True,
            "Demand uid or name that this profile scales",
            "d1",
        ),
        (
            "profile",
            _J_SCHED,
            True,
            "Load-shape values [0-1]: scalar, array, or filename",
            "lmax",
        ),
        (
            "scost",
            _J_SCHED,
            False,
            "Curtailment cost [$/MWh]",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — storage
    # ------------------------------------------------------------------
    "battery_array": [
        ("uid", _J_INT, True, "Unique battery identifier", 1),
        ("name", _J_STR, True, "Battery name", "bat1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            "Element type tag (free-form metadata; not used by the LP).",
            None,
        ),
        ("bus", _J_ID, False, "Connected bus uid or name (optional)", None),
        (
            "source_generator",
            _J_ID,
            False,
            "Co-located generator uid or name for generation-coupled mode "
            "(solar+battery); when set, the generator feeds the battery directly",
            None,
        ),
        (
            "input_efficiency",
            _J_SCHED,
            False,
            "Charge efficiency [p.u.] (default: 1.0)",
            0.95,
        ),
        (
            "output_efficiency",
            _J_SCHED,
            False,
            "Discharge efficiency [p.u.] (default: 1.0)",
            0.95,
        ),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Annual self-discharge loss factor [p.u./year]",
            None,
        ),
        ("emin", _J_SCHED, False, "Minimum state-of-charge [MWh]", 0.0),
        ("emax", _J_SCHED, False, "Maximum state-of-charge [MWh]", 100.0),
        (
            "ecost",
            _J_SCHED,
            False,
            "Energy storage cost (terminal value) [$/MWh]",
            None,
        ),
        (
            "eini",
            _J_NUM,
            False,
            "Initial state-of-charge [MWh] (default: emin)",
            None,
        ),
        (
            "efin",
            _J_NUM,
            False,
            "Required final state-of-charge [MWh] (default: free)",
            None,
        ),
        (
            "efin_cost",
            _J_NUM,
            False,
            "Penalty cost per unit of efin shortfall [$/MWh] "
            "(soft end-of-horizon target; mirrors Reservoir.efin_cost)",
            None,
        ),
        (
            "soft_emin",
            _J_SCHED,
            False,
            "Soft minimum SoC [MWh] — allows SoC to drop below at a penalty cost",
            None,
        ),
        (
            "soft_emin_cost",
            _J_SCHED,
            False,
            "Penalty cost [$/MWh] for SoC below soft_emin",
            None,
        ),
        (
            "pmax_charge",
            _J_SCHED,
            False,
            "Maximum charging power [MW]",
            50.0,
        ),
        (
            "pmax_discharge",
            _J_SCHED,
            False,
            "Maximum discharging power [MW]",
            50.0,
        ),
        (
            "pmin_charge",
            _J_SCHED,
            False,
            "Minimum charging power [MW] per-(stage, block); HARD floor "
            "(gated by commitment when set). Mirrors UC.jl Min Charge Rate.",
            None,
        ),
        (
            "pmin_discharge",
            _J_SCHED,
            False,
            "Minimum discharging power [MW] per-(stage, block); HARD floor. "
            "Mirrors UC.jl Min Discharge Rate.",
            None,
        ),
        (
            "discharge_cost",
            _J_SCHED,
            False,
            "Per-MWh cost paid when discharging the battery [$/MWh] per-(stage, block)",
            None,
        ),
        (
            "charge_cost",
            _J_SCHED,
            False,
            "Per-MWh cost paid when charging the battery [$/MWh] "
            "per-(stage, block); counterpart to discharge_cost.",
            None,
        ),
        (
            "commitment",
            _J_BOOL,
            False,
            "When true, the synthetic Converter adds per-block binaries "
            "u_charge / u_discharge to gate pmin_charge / pmin_discharge.",
            None,
        ),
        ("capacity", _J_SCHED, False, "Initial installed capacity [MWh]", None),
        (
            "expcap",
            _J_SCHED,
            False,
            "Capacity per expansion module [MWh/module]",
            None,
        ),
        ("expmod", _J_SCHED, False, "Maximum number of expansion modules", None),
        (
            "integer_expmod",
            _J_BOOL,
            False,
            "When true, restrict the expansion-module count to integer values "
            "(otherwise continuous; default: false).",
            None,
        ),
        (
            "capmax",
            _J_SCHED,
            False,
            "Absolute maximum energy capacity [MWh]",
            None,
        ),
        (
            "annual_capcost",
            _J_SCHED,
            False,
            "Annualized investment cost [$/MWh-year]",
            None,
        ),
        (
            "annual_derating",
            _J_SCHED,
            False,
            "Annual capacity derating factor [p.u./year]",
            None,
        ),
        (
            "use_state_variable",
            _J_BOOL,
            False,
            "Link SoC across planning stages/phases (true/false, default: false)",
            None,
        ),
        (
            "daily_cycle",
            _J_BOOL,
            False,
            "Reset SoC to eini at the start of each day (true/false)",
            None,
        ),
    ],
    "converter_array": [
        ("uid", _J_INT, True, "Unique converter identifier", 1),
        ("name", _J_STR, True, "Converter name", "conv1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "battery",
            _J_ID,
            True,
            "Battery uid or name linked by this converter",
            "bat1",
        ),
        (
            "generator",
            _J_ID,
            True,
            "Generator uid or name used for battery discharge",
            "g_bat1",
        ),
        (
            "demand",
            _J_ID,
            True,
            "Demand uid or name used for battery charge",
            "d_bat1",
        ),
        (
            "conversion_rate",
            _J_SCHED,
            False,
            "Energy conversion ratio [MWh/MWh] (default: 1.0)",
            None,
        ),
        (
            "commitment",
            _J_BOOL,
            False,
            "When true, add per-block u_charge / u_discharge binaries to gate "
            "pmin_charge / pmin_discharge on the linked Battery.",
            None,
        ),
        ("capacity", _J_SCHED, False, "Initial converter capacity [MW]", None),
        (
            "expcap",
            _J_SCHED,
            False,
            "Capacity per expansion module [MW/module]",
            None,
        ),
        ("expmod", _J_SCHED, False, "Maximum number of expansion modules", None),
        (
            "integer_expmod",
            _J_BOOL,
            False,
            "When true, restrict the expansion-module count to integer values "
            "(otherwise continuous; default: false).",
            None,
        ),
        (
            "capmax",
            _J_SCHED,
            False,
            "Absolute maximum converter capacity [MW]",
            None,
        ),
        (
            "annual_capcost",
            _J_SCHED,
            False,
            "Annualized investment cost [$/MW-year]",
            None,
        ),
        (
            "annual_derating",
            _J_SCHED,
            False,
            "Annual capacity derating factor [p.u./year]",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — LNG terminals
    # ------------------------------------------------------------------
    "lng_terminal_array": [
        ("uid", _J_INT, True, "Unique LNG terminal identifier", 1),
        ("name", _J_STR, True, "Terminal name", "gnl1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("emin", _J_SCHED, False, "Minimum tank level [m³]", 0),
        ("emax", _J_SCHED, False, "Maximum tank level [m³]", 150000),
        ("eini", _J_NUM, False, "Initial tank level [m³]", 80000),
        ("efin", _J_NUM, False, "End-of-horizon minimum level [m³]", None),
        (
            "efin_cost",
            _J_NUM,
            False,
            "Penalty cost per unit of efin shortfall [$/m³]. "
            "When set (and > 0) the hard vol_end >= efin row becomes soft.",
            None,
        ),
        ("ecost", _J_SCHED, False, "Holding cost [$/m³]", None),
        ("annual_loss", _J_SCHED, False, "Boil-off gas rate [p.u./year]", 0.001),
        ("sendout_max", _J_NUM, False, "Max regasification rate [m³/h]", None),
        ("sendout_min", _J_NUM, False, "Min regasification rate [m³/h]", None),
        ("delivery", _J_SCHED, False, "Scheduled LNG delivery [m³/stage]", None),
        ("spillway_cost", _J_NUM, False, "Venting penalty cost [$/m³]", 100),
        ("spillway_capacity", _J_NUM, False, "Max venting rate [m³/h]", None),
        ("use_state_variable", _J_INT, False, "SDDP state (1=yes, 0=no)", 1),
        (
            "mean_production_factor",
            _J_NUM,
            False,
            "Power conversion factor [MWh/m³] for scost",
            None,
        ),
        ("scost", _J_SCHED, False, "State penalty cost [$/m³]", None),
        ("soft_emin", _J_SCHED, False, "Soft minimum tank level [m³]", None),
        ("soft_emin_cost", _J_SCHED, False, "Soft minimum penalty [$/m³]", None),
        (
            "flow_conversion_rate",
            _J_NUM,
            False,
            "Flow conversion factor [m³/(m³/h·h)] (default: 1.0)",
            None,
        ),
        (
            "generators",
            _J_STR,
            False,
            "JSON array of {generator, heat_rate} links "
            '(e.g. [{"generator": 10, "heat_rate": 0.18}])',
            None,
        ),
        (
            "generator",
            _J_ID,
            False,
            "Legacy single-generator shortcut: folded into `generators` "
            "with the top-level `heat_rate` at parse time.",
            None,
        ),
        (
            "heat_rate",
            _J_NUM,
            False,
            "Legacy single-generator heat-rate companion to the `generator` "
            "shortcut [m³/MWh]; folded into the first `generators` entry.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — multi-carrier balance nodes (PR #483 / #484)
    # ------------------------------------------------------------------
    "thermal_node_array": [
        ("uid", _J_INT, True, "Unique thermal-node identifier", 1),
        ("name", _J_STR, True, "Thermal-node name", "tn1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
    ],
    "hydrogen_node_array": [
        ("uid", _J_INT, True, "Unique hydrogen-node identifier", 1),
        ("name", _J_STR, True, "Hydrogen-node name", "h2_node1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
    ],
    "ammonia_node_array": [
        ("uid", _J_INT, True, "Unique ammonia-node identifier", 1),
        ("name", _J_STR, True, "Ammonia-node name", "nh3_node1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
    ],
    # ------------------------------------------------------------------
    # System — multi-carrier storage peers (PR #483)
    # ------------------------------------------------------------------
    "thermal_storage_array": [
        ("uid", _J_INT, True, "Unique thermal-storage identifier", 1),
        ("name", _J_STR, True, "Thermal storage name (e.g. molten-salt TES)", "tes1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("type", _J_STR, False, 'Optional tag (e.g. "molten_salt", "concrete")', None),
        (
            "thermal_node",
            _J_ID,
            False,
            "FK into thermal_node_array (NOT a Bus).",
            None,
        ),
        (
            "input_efficiency",
            _J_SCHED,
            False,
            "Charging efficiency [p.u.] per-(stage, block)",
            0.98,
        ),
        (
            "output_efficiency",
            _J_SCHED,
            False,
            "Discharging efficiency [p.u.] per-(stage, block)",
            0.98,
        ),
        ("annual_loss", _J_SCHED, False, "TES self-discharge [p.u./year]", 0.05),
        ("emin", _J_SCHED, False, "Minimum thermal SoC [MWh_th]", 0),
        ("emax", _J_SCHED, False, "Maximum thermal SoC [MWh_th]", 1500),
        ("ecost", _J_SCHED, False, "Holding cost [$/MWh_th]", None),
        ("eini", _J_NUM, False, "Initial thermal SoC [MWh_th]", 0),
        ("efin", _J_NUM, False, "Minimum terminal SoC [MWh_th]", None),
        ("efin_cost", _J_NUM, False, "Soft-cap shortfall penalty [$/MWh_th]", None),
        ("soft_emin", _J_SCHED, False, "Soft minimum SoC [MWh_th]", None),
        ("soft_emin_cost", _J_SCHED, False, "Soft-emin penalty [$/MWh_th]", None),
        ("capacity", _J_SCHED, False, "Installed thermal capacity [MWh_th]", None),
        ("use_state_variable", _J_INT, False, "SDDP state (1=yes, 0=no)", 1),
        (
            "daily_cycle",
            _J_INT,
            False,
            "PLP-style daily-cycle scaling (default 0 for CSP)",
            None,
        ),
    ],
    "hydrogen_storage_array": [
        ("uid", _J_INT, True, "Unique hydrogen-storage identifier", 1),
        ("name", _J_STR, True, "Hydrogen storage name", "salt_cavern"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            'Optional tag ("salt_cavern", "high_pressure", "lh2", "lohc")',
            None,
        ),
        (
            "hydrogen_node",
            _J_ID,
            False,
            "FK into hydrogen_node_array (NOT a Bus / NOT an AmmoniaNode).",
            None,
        ),
        (
            "input_efficiency",
            _J_SCHED,
            False,
            "Compression / liquefaction efficiency [p.u.]",
            0.95,
        ),
        ("output_efficiency", _J_SCHED, False, "Withdrawal efficiency [p.u.]", 0.99),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Self-discharge [p.u./year] (salt cavern ≈ 0.01; LH₂ ≈ 0.18)",
            0.005,
        ),
        ("emin", _J_SCHED, False, "Minimum stored energy [MWh_LHV]", 0),
        ("emax", _J_SCHED, False, "Maximum stored energy [MWh_LHV]", 200000),
        ("ecost", _J_SCHED, False, "Holding cost [$/MWh_LHV]", None),
        ("eini", _J_NUM, False, "Initial stored energy [MWh_LHV]", 0),
        ("efin", _J_NUM, False, "Minimum terminal stored energy [MWh_LHV]", None),
        ("efin_cost", _J_NUM, False, "Soft-cap shortfall penalty [$/MWh_LHV]", None),
        ("soft_emin", _J_SCHED, False, "Soft minimum SoC [MWh_LHV]", None),
        ("soft_emin_cost", _J_SCHED, False, "Soft-emin penalty [$/MWh_LHV]", None),
        ("capacity", _J_SCHED, False, "Installed storage size [MWh_LHV]", None),
        ("use_state_variable", _J_INT, False, "SDDP state (1=yes, 0=no)", 1),
        ("daily_cycle", _J_INT, False, "PLP-style daily-cycle scaling", None),
    ],
    "ammonia_storage_array": [
        ("uid", _J_INT, True, "Unique ammonia-storage identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Ammonia storage name (e.g. refrigerated tank)",
            "nh3_tank",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            'Optional tag ("refrigerated", "pressurised", "underground")',
            None,
        ),
        (
            "ammonia_node",
            _J_ID,
            False,
            "FK into ammonia_node_array (NOT a HydrogenNode / NOT a Bus).",
            None,
        ),
        ("input_efficiency", _J_SCHED, False, "Charging efficiency [p.u.]", None),
        ("output_efficiency", _J_SCHED, False, "Withdrawal efficiency [p.u.]", None),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Boil-off [p.u./year] (refrigerated NH₃ ≈ 0.025)",
            0.025,
        ),
        ("emin", _J_SCHED, False, "Minimum stored energy [MWh_LHV]", 0),
        ("emax", _J_SCHED, False, "Maximum stored energy [MWh_LHV]", 310000),
        ("ecost", _J_SCHED, False, "Holding cost [$/MWh_LHV]", None),
        ("eini", _J_NUM, False, "Initial stored energy [MWh_LHV]", 0),
        ("efin", _J_NUM, False, "Minimum terminal stored energy [MWh_LHV]", None),
        ("efin_cost", _J_NUM, False, "Soft-cap shortfall penalty [$/MWh_LHV]", None),
        ("soft_emin", _J_SCHED, False, "Soft minimum SoC [MWh_LHV]", None),
        ("soft_emin_cost", _J_SCHED, False, "Soft-emin penalty [$/MWh_LHV]", None),
        ("capacity", _J_SCHED, False, "Installed storage size [MWh_LHV]", None),
        ("use_state_variable", _J_INT, False, "SDDP state (1=yes, 0=no)", 1),
        ("daily_cycle", _J_INT, False, "PLP-style daily-cycle scaling", None),
    ],
    # ------------------------------------------------------------------
    # System — multi-carrier converter (PR #485)
    # ------------------------------------------------------------------
    "carrier_converter_array": [
        ("uid", _J_INT, True, "Unique converter identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Converter name (e.g. electrolyser, fuel_cell, haber_bosch, "
            "nh3_cracker, power_block)",
            "pem_100mw",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("type", _J_STR, False, "Optional tag", None),
        (
            "from_carrier",
            _J_STR,
            True,
            'Source carrier — one of "electric", "water", "hydrogen", '
            '"thermal", "ammonia"',
            "electric",
        ),
        (
            "to_carrier",
            _J_STR,
            True,
            "Destination carrier (same values as from_carrier)",
            "hydrogen",
        ),
        (
            "from_node",
            _J_ID,
            False,
            "FK into the carrier-matching node array (Bus, ThermalNode, "
            "HydrogenNode, AmmoniaNode, or Junction)",
            None,
        ),
        ("to_node", _J_ID, False, "FK into the destination-carrier node array", None),
        (
            "efficiency",
            _J_SCHED,
            False,
            "Conversion efficiency η (output per input). PEM ≈ 0.7; fuel "
            "cell ≈ 0.5; Haber-Bosch ≈ 0.7; CSP power block ≈ 0.4",
            0.7,
        ),
        ("ocost", _J_SCHED, False, "Operating cost per unit input [$/unit]", None),
        ("capacity", _J_SCHED, False, "Installed input capacity (per-block MW)", None),
        ("expcap", _J_SCHED, False, "Capacity per expansion module", None),
        ("expmod", _J_SCHED, False, "Maximum number of expansion modules", None),
        ("capmax", _J_SCHED, False, "Absolute maximum capacity", None),
        ("annual_capcost", _J_SCHED, False, "Annualised investment cost", None),
        ("annual_derating", _J_SCHED, False, "Annual derating factor", None),
        ("integer_expmod", _J_INT, False, "Integer-constrain expmod (1=yes)", None),
    ],
    # ------------------------------------------------------------------
    # System — CO₂ cap-and-trade allowance pool (PR #495 / #496)
    # ------------------------------------------------------------------
    "allowance_pool_array": [
        ("uid", _J_INT, True, "Unique allowance-pool identifier", 1),
        (
            "name",
            _J_STR,
            True,
            'Pool name (e.g. "EU_ETS", "CA_CapTrade", "RGGI")',
            "EU_ETS",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "type",
            _J_STR,
            False,
            'Regulatory regime tag (e.g. "eu_ets", "california_capntrade")',
            None,
        ),
        ("emission", _J_ID, False, 'FK into emission_array (typically "co2")', "co2"),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Allowance-decay rate [p.u./year] (e.g. EU ETS MSR ≈ 0.12 above "
            "threshold; default unset = no decay)",
            None,
        ),
        (
            "emin",
            _J_SCHED,
            False,
            "Minimum banked allowances [tCO₂] (set < 0 to permit borrowing)",
            0,
        ),
        ("emax", _J_SCHED, False, "Maximum banked allowances [tCO₂]", None),
        ("ecost", _J_SCHED, False, "Holding cost [$/tCO₂]", None),
        ("eini", _J_NUM, False, "Initial banked allowances [tCO₂]", 0),
        ("efin", _J_NUM, False, "Minimum terminal banked allowances [tCO₂]", None),
        (
            "efin_cost",
            _J_NUM,
            False,
            "Per-unit penalty on efin shortfall [$/tCO₂]",
            None,
        ),
        ("soft_emin", _J_SCHED, False, "Soft minimum banked level [tCO₂]", None),
        ("soft_emin_cost", _J_SCHED, False, "Soft-emin penalty [$/tCO₂]", None),
        (
            "delivery",
            _J_SCHED,
            False,
            "Free-allocation per stage [tCO₂/stage] (grandfathering / benchmarking)",
            None,
        ),
        (
            "auction_price",
            _J_SCHED,
            False,
            "[Phase 4 hook] Secondary-market purchase price [$/tCO₂]",
            None,
        ),
        (
            "auction_cap",
            _J_SCHED,
            False,
            "[Phase 4 hook] Per-block purchase cap [tCO₂]",
            None,
        ),
        ("capacity", _J_SCHED, False, "Installed pool size [tCO₂]", None),
        ("use_state_variable", _J_INT, False, "SDDP state (1=yes, 0=no)", 1),
        ("daily_cycle", _J_INT, False, "PLP-style daily-cycle scaling", None),
    ],
    # ------------------------------------------------------------------
    # System — reserves
    # ------------------------------------------------------------------
    "reserve_zone_array": [
        ("uid", _J_INT, True, "Unique reserve zone identifier", 1),
        ("name", _J_STR, True, "Reserve zone name", "rz1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "urreq",
            _J_SCHED,
            False,
            "Up-spinning reserve requirement [MW]",
            None,
        ),
        (
            "drreq",
            _J_SCHED,
            False,
            "Down-spinning reserve requirement [MW]",
            None,
        ),
        (
            "urcost",
            _J_SCHED,
            False,
            "Up-reserve failure cost [$/MW] (overrides global reserve_fail_cost)",
            None,
        ),
        (
            "drcost",
            _J_SCHED,
            False,
            "Down-reserve failure cost [$/MW]",
            None,
        ),
    ],
    "reserve_provision_array": [
        ("uid", _J_INT, True, "Unique reserve provision identifier", 1),
        ("name", _J_STR, True, "Provision name", "rp1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "generator",
            _J_ID,
            True,
            "Generator uid or name that provides the reserve",
            "g1",
        ),
        (
            "reserve_zones",
            _J_STR,
            True,
            "Comma-separated list of reserve zone names (e.g. 'rz1,rz2')",
            "rz1",
        ),
        ("urmax", _J_SCHED, False, "Maximum up-reserve contribution [MW]", None),
        ("drmax", _J_SCHED, False, "Maximum down-reserve contribution [MW]", None),
        (
            "urmin",
            _J_SCHED,
            False,
            "Minimum up-reserve contribution [MW] (hard floor).",
            None,
        ),
        (
            "drmin",
            _J_SCHED,
            False,
            "Minimum down-reserve contribution [MW] (hard floor).",
            None,
        ),
        (
            "ur_capacity_factor",
            _J_SCHED,
            False,
            "Fraction of installed capacity available for up-reserve [p.u.]",
            None,
        ),
        (
            "dr_capacity_factor",
            _J_SCHED,
            False,
            "Fraction of installed capacity available for down-reserve [p.u.]",
            None,
        ),
        (
            "ur_provision_factor",
            _J_SCHED,
            False,
            "Fraction of urmax actually provided as reserve [p.u.]",
            None,
        ),
        (
            "dr_provision_factor",
            _J_SCHED,
            False,
            "Fraction of drmax actually provided as reserve [p.u.]",
            None,
        ),
        ("urcost", _J_SCHED, False, "Up-reserve provision cost [$/MW]", None),
        ("drcost", _J_SCHED, False, "Down-reserve provision cost [$/MW]", None),
    ],
    # ------------------------------------------------------------------
    # System — inertia (system-inertia requirements + provision links)
    # ------------------------------------------------------------------
    "inertia_zone_array": [
        ("uid", _J_INT, True, "Unique inertia-zone identifier", 1),
        ("name", _J_STR, True, "InertiaZone name", "iz1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "requirement",
            _J_SCHED,
            False,
            "Minimum inertia requirement [MWs] per-(stage, block).",
            None,
        ),
        (
            "cost",
            _J_SCHED,
            False,
            "Inertia shortage penalty [$/MWs] per-(stage, block).",
            None,
        ),
    ],
    # TODO(unit-audit): reconcile InertiaProvision.cost vs InertiaZone.cost ($/MW vs $/MWs)
    "inertia_provision_array": [
        ("uid", _J_INT, True, "Unique inertia-provision identifier", 1),
        ("name", _J_STR, True, "InertiaProvision name", "ip1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "generator",
            _J_ID,
            True,
            "FK to the providing generator (uid or name).",
            "g1",
        ),
        (
            "inertia_zones",
            _J_STR,
            False,
            "Typed array of InertiaZone references (uid or name per element); "
            "JSON array form (replaces legacy colon-delimited string).",
            None,
        ),
        (
            "inertia_constant",
            _J_NUM,
            False,
            "Machine inertia constant H [s].",
            None,
        ),
        (
            "rated_power",
            _J_NUM,
            False,
            "Rated apparent power S [MVA].",
            None,
        ),
        (
            "provision_max",
            _J_SCHED,
            False,
            "Max inertia provision [MW] per-(stage, block).",
            None,
        ),
        (
            "provision_factor",
            _J_SCHED,
            False,
            "Effectiveness factor FE [MWs/MW] per-(stage, block).",
            None,
        ),
        (
            "cost",
            _J_SCHED,
            False,
            "Provision cost [$/MW] per-(stage, block). "
            "(see TODO above — unit may be $/MWs after audit.)",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — emissions (pollutant registry + zones + sources)
    # ------------------------------------------------------------------
    "emission_array": [
        # Pollutant registry: minimal (uid/name/active). Required when any
        # emission_zone is defined.
        ("uid", _J_INT, True, "Unique pollutant identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Pollutant name (e.g. 'co2', 'ch4', 'n2o', 'sf6').",
            "co2",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
    ],
    "emission_zone_array": [
        ("uid", _J_INT, True, "Unique emission-zone identifier", 1),
        ("name", _J_STR, True, "EmissionZone name", "global_co2"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "emission",
            _J_ID,
            False,
            "Legacy single-pollutant shortcut: Emission uid or name, "
            "auto-promoted to a 1-element `emissions=[{emission, weight=1}]`.",
            None,
        ),
        (
            "weight",
            _J_NUM,
            False,
            "GWP weight for the legacy `emission` shortcut (default: 1.0). "
            "Ignored when `emissions[]` is set explicitly.",
            None,
        ),
        (
            "emissions",
            "JSON array",
            False,
            "List of pollutants covered by this zone, each with a GWP weight. "
            "JSON array of {emission, weight [p.u.]}. Example: "
            '[{"emission":"co2","weight":1.0},{"emission":"ch4","weight":27.9}].',
            None,
        ),
        (
            "cap",
            _J_SCHED,
            False,
            "Per-stage cap on total emissions [CO2-eq t / stage] across "
            "all sources in this zone. Hard by default; soft if cap_cost set.",
            None,
        ),
        (
            "cap_cost",
            _J_SCHED,
            False,
            "Slack penalty above cap [$/t] (converts cap from hard to soft).",
            None,
        ),
        (
            "price",
            _J_SCHED,
            False,
            "Per-ton tax / permit price [$/t], stage-schedulable.",
            None,
        ),
        (
            "allowance_pool",
            _J_ID,
            False,
            "FK to an AllowancePool that banks this zone's allowances "
            "(cap-and-trade w/ banking). When set, production is drawn "
            "from the pool's bank and the standalone `cap` row is skipped.",
            None,
        ),
    ],
    # NOTE: `emission_capture` does not have a top-level array — instances
    # live inline only on `Generator.emission_captures[]`.
    "emission_source_array": [
        ("uid", _J_INT, False, "Unique emission-source identifier", 1),
        ("name", _J_STR, False, "EmissionSource name", "ngcc_co2"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "generator",
            _J_ID,
            False,
            "FK to the contributing generator. Required for the canonical "
            "top-level form; omitted (and auto-filled) in the inline "
            "Generator.emissions[] shorthand.",
            None,
        ),
        (
            "zone",
            _J_ID,
            True,
            "FK to the EmissionZone the source contributes to.",
            "global_co2",
        ),
        (
            "emission",
            _J_ID,
            True,
            "FK to the Emission pollutant kind.",
            "co2",
        ),
        (
            "rate",
            _J_SCHED,
            False,
            "Combustion / tank-to-stack (TTW) emission rate [t/MWh], "
            "stage-schedulable.",
            None,
        ),
        (
            "upstream_rate",
            _J_SCHED,
            False,
            "Upstream / well-to-tank (WTT) emission rate [t/MWh], stage-schedulable.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — hydro
    # ------------------------------------------------------------------
    "junction_array": [
        ("uid", _J_INT, True, "Unique junction identifier", 1),
        ("name", _J_STR, True, "Junction name (hydraulic node)", "j1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "drain",
            _J_BOOL,
            False,
            "If true, excess water drains freely (spills without cost)",
            None,
        ),
    ],
    "waterway_array": [
        ("uid", _J_INT, True, "Unique waterway identifier", 1),
        ("name", _J_STR, True, "Waterway name (channel between junctions)", "ww1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "junction_a",
            _J_ID,
            True,
            "Upstream junction uid or name",
            "j1",
        ),
        (
            "junction_b",
            _J_ID,
            False,
            "Downstream junction uid or name.  OPTIONAL: when unset the "
            "waterway is an outflow that debits ``junction_a`` and drains "
            "its flow out of the system (no downstream credit, no synthetic "
            "ocean/sink junction needed).  Mirrors ``Turbine.junction_b`` "
            "built-in waterway drain mode.",
            "j2",
        ),
        ("capacity", _J_SCHED, False, "Maximum flow capacity [m³/s]", None),
        (
            "lossfactor",
            _J_SCHED,
            False,
            "Water loss factor [p.u.] (fraction of flow lost in transit)",
            None,
        ),
        (
            "fmin",
            _J_SCHED,
            False,
            "Minimum flow rate [m³/s] (environmental constraint)",
            None,
        ),
        ("fmax", _J_SCHED, False, "Maximum flow rate [m³/s]", None),
        (
            "fcost",
            _J_SCHED,
            False,
            "Per-unit flow cost [$/m³/s/h] charged on the waterway flow column.",
            None,
        ),
    ],
    "flow_array": [
        ("uid", _J_INT, True, "Unique flow identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Flow name (exogenous inflow or outflow at a junction)",
            "f1",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "direction",
            _J_INT,
            False,
            "Flow direction: 1 = inflow (positive), -1 = outflow (negative)",
            1,
        ),
        (
            "junction",
            _J_ID,
            True,
            "Junction uid or name where this flow is applied",
            "j1",
        ),
        (
            "discharge",
            _J_SCHED,
            True,
            "Flow discharge [m³/s]: scalar, array, or filename",
            10.0,
        ),
    ],
    "reservoir_array": [
        ("uid", _J_INT, True, "Unique reservoir identifier", 1),
        ("name", _J_STR, True, "Reservoir name", "res1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "junction",
            _J_ID,
            True,
            "Hydraulic junction uid or name associated with this reservoir",
            "j1",
        ),
        (
            "spill_junction",
            _J_ID,
            False,
            "Optional downstream junction uid or name for spill routing. "
            "When set, spilled water flows downstream instead of vanishing "
            "(mirrors PLP SerVer chaining).",
            None,
        ),
        (
            "spillway_capacity",
            _J_NUM,
            False,
            "Maximum spillway flow capacity [m³/s]",
            None,
        ),
        (
            "spillway_cost",
            _J_NUM,
            False,
            "Penalty cost per unit of spillway flow [$/(m³/s)/h] "
            "— LP pays spillway_cost · q · duration per block "
            "(same convention as Waterway.fcost)",
            None,
        ),
        ("capacity", _J_SCHED, False, "Reservoir storage capacity [hm³]", None),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Annual evaporation/seepage loss factor [p.u./year]",
            None,
        ),
        ("emin", _J_SCHED, False, "Minimum reservoir volume [hm³]", None),
        ("emax", _J_SCHED, False, "Maximum reservoir volume [hm³]", None),
        (
            "ecost",
            _J_SCHED,
            False,
            "Terminal energy value (water value) [$/hm³]",
            None,
        ),
        ("eini", _J_NUM, False, "Initial reservoir volume [hm³]", None),
        ("efin", _J_NUM, False, "Required final reservoir volume [hm³]", None),
        (
            "efin_cost",
            _J_NUM,
            False,
            "Penalty cost per unit of efin shortfall [$/hm³]. "
            "When set (and > 0) the hard vol_end >= efin row becomes soft.",
            None,
        ),
        (
            "soft_emin",
            _J_SCHED,
            False,
            "Soft minimum volume [hm³] — allows volume to drop below at a penalty cost",
            None,
        ),
        (
            "soft_emin_cost",
            _J_SCHED,
            False,
            "Penalty cost [$/hm³] for volume below soft_emin",
            None,
        ),
        (
            "mean_production_factor",
            _J_NUM,
            False,
            "Expected turbine production factor [MWh/hm³]. Converts the "
            "global state_violation_cost ($/MWh) into reservoir-specific "
            "units ($/hm³). Defaults to 5.0 MWh/hm³.",
            None,
        ),
        (
            "scost",
            _J_SCHED,
            False,
            "State cost: elastic penalty for SDDP state-variable violations "
            "[$/hm³] per-(stage, block). When unset, computed from "
            "state_fail_cost × mean_production_factor.",
            None,
        ),
        (
            "fmin",
            _J_NUM,
            False,
            "Minimum turbine discharge [m³/s]",
            None,
        ),
        (
            "fmax",
            _J_NUM,
            False,
            "Maximum turbine discharge [m³/s]",
            None,
        ),
        (
            "flow_conversion_rate",
            _J_NUM,
            False,
            "Conversion factor from flow [m³/s] to volume [hm³/block]",
            None,
        ),
        (
            "use_state_variable",
            _J_BOOL,
            False,
            "Link reservoir volume across planning stages/phases (true/false)",
            None,
        ),
        (
            "daily_cycle",
            _J_BOOL,
            False,
            "Reset reservoir to eini at the start of each day",
            None,
        ),
        (
            "seepage",
            "JSON array",
            False,
            "Inline list of ReservoirSeepage rows scoped to this reservoir "
            "(folded into the top-level `reservoir_seepage_array` at parse time).",
            None,
        ),
        (
            "discharge_limit",
            "JSON array",
            False,
            "Inline list of ReservoirDischargeLimit rows scoped to this "
            "reservoir (folded into the top-level "
            "`reservoir_discharge_limit_array` at parse time).",
            None,
        ),
        (
            "production_factor",
            "JSON array",
            False,
            "Inline list of ReservoirProductionFactor rows scoped to this "
            "reservoir (folded into the top-level "
            "`reservoir_production_factor_array` at parse time).",
            None,
        ),
    ],
    "reservoir_seepage_array": [
        ("uid", _J_INT, True, "Unique seepage identifier", 1),
        ("name", _J_STR, True, "ReservoirSeepage name", "filt1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "waterway",
            _J_ID,
            True,
            "Waterway uid or name that loses water via seepage",
            "ww1",
        ),
        (
            "reservoir",
            _J_ID,
            True,
            "Reservoir uid or name that receives the filtered water",
            "res1",
        ),
        (
            "slope",
            _J_SCHED,
            False,
            "Seepage slope [m³/s per hm³] — scalar, per-stage array, or filename. "
            "Used when segments is empty or as the initial value before the "
            "first volume-dependent update.",
            None,
        ),
        (
            "constant",
            _J_SCHED,
            False,
            "Constant seepage rate [m³/s] — scalar, per-stage array, or filename. "
            "Used when segments is empty or as the initial value before the "
            "first volume-dependent update.",
            None,
        ),
        (
            "volume",
            _J_NUM,
            False,
            "Legacy single-segment shortcut: lower-bound volume [hm³] for the "
            "(volume, slope, constant) entry promoted into `segments[]` at "
            "parse time.",
            None,
        ),
        (
            "segments",
            "JSON array",
            False,
            "Piecewise-linear seepage curve (plpfilemb.dat model): JSON array "
            "of {volume [hm³], slope [m³/s per hm³], constant [m³/s]} objects. "
            "When present, the active segment is selected at each phase based on "
            "the current reservoir volume and the LP constraint coefficients "
            "(slope on eini/efin and the RHS) are updated directly in the LP. "
            'Example: [{"volume":0,"slope":0.00016,"constant":2.19},'
            '{"volume":500000,"slope":0.0001,"constant":4.8}]',
            None,
        ),
    ],
    "reservoir_discharge_limit_array": [
        ("uid", _J_INT, True, "Unique discharge-limit identifier", 1),
        ("name", _J_STR, True, "ReservoirDischargeLimit name", "ddl1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "waterway",
            _J_ID,
            False,
            "Waterway uid or name whose discharge is limited.  Exactly one "
            "of ``waterway`` / ``turbine`` must be set: ``waterway`` is the "
            "legacy reference for the classic Reservoir → Waterway → drain "
            "topology; ``turbine`` is the new reference for built-in "
            "waterway turbines (``Turbine.junction_a/b``) that own their "
            "own flow column instead of going through a Waterway.",
            "ww1",
        ),
        (
            "turbine",
            _J_ID,
            False,
            "Turbine uid or name whose built-in waterway flow is limited.  "
            "Alternative to ``waterway`` — exactly one of the two must be "
            "set.  Use this when the cascade plant carries its flow via the "
            "Turbine itself (``junction_a/junction_b``) rather than through "
            "a separate Waterway element.",
            None,
        ),
        (
            "reservoir",
            _J_ID,
            True,
            "Reservoir uid or name providing the volume reference",
            "res1",
        ),
        (
            "volume",
            _J_NUM,
            False,
            "Legacy single-segment shortcut: lower-bound volume [hm³] for the "
            "(volume, slope, intercept) entry promoted into `segments[]` at "
            "parse time.",
            None,
        ),
        (
            "slope",
            _J_NUM,
            False,
            "Legacy single-segment slope [m³/s per hm³]; promoted into "
            "`segments[]` together with `volume` / `intercept`.",
            None,
        ),
        (
            "intercept",
            _J_NUM,
            False,
            "Legacy single-segment intercept [m³/s]; promoted into "
            "`segments[]` together with `volume` / `slope`.",
            None,
        ),
        (
            "segments",
            "JSON array",
            False,
            "Piecewise-linear discharge-limit curve: JSON array of "
            "{volume [hm³], slope [m³/s per hm³], intercept [m³/s]} objects. "
            "The active segment is selected based on reservoir volume. "
            'Example: [{"volume":0,"slope":6.9868e-5,"intercept":15.787},'
            '{"volume":757000,"slope":1.3985e-4,"intercept":57.454}]',
            None,
        ),
    ],
    "turbine_array": [
        ("uid", _J_INT, True, "Unique turbine identifier", 1),
        ("name", _J_STR, True, "Turbine name", "turb1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "waterway",
            _J_ID,
            False,
            "Waterway uid or name associated with this turbine (optional if flow set)",
            "ww1",
        ),
        (
            "flow",
            _J_ID,
            False,
            "Flow uid or name (alternative to waterway for pasada/run-of-river mode)",
            None,
        ),
        (
            "junction_a",
            _J_ID,
            False,
            "Upstream/intake junction uid or name.  Setting this enables "
            "the built-in waterway mode: the turbine owns its own flow "
            "column, debits ``junction_a``, credits ``junction_b`` (when "
            "set), and converts the carried flow to power — replacing the "
            "separate penstock Waterway.  Mode priority: flow > junctions "
            "> waterway.",
            None,
        ),
        (
            "junction_b",
            _J_ID,
            False,
            "Downstream junction uid or name.  Optional companion to "
            "``junction_a`` in built-in waterway mode.  When unset, the "
            "turbined flow drains out of the system (run-to-sea plants — "
            "no synthetic ocean junction needed).",
            None,
        ),
        (
            "generator",
            _J_ID,
            True,
            "Generator uid or name that represents this turbine's output",
            "g_hydro",
        ),
        (
            "drain",
            _J_BOOL,
            False,
            "If true, excess water bypasses the turbine (spills)",
            None,
        ),
        (
            "production_factor",
            _J_SCHED,
            False,
            "Water-to-power production factor [MW/(m³/s)]",
            1.0,
        ),
        (
            "efficiency",
            _J_SCHED,
            False,
            "Turbine efficiency [p.u.] (default 1.0). Effective conversion "
            "rate = efficiency × production_factor.",
            None,
        ),
        (
            "capacity",
            _J_SCHED,
            False,
            "Maximum turbine flow [m³/s]",
            None,
        ),
        (
            "main_reservoir",
            _J_ID,
            False,
            "Reservoir uid or name that is the primary head source",
            None,
        ),
    ],
    "reservoir_production_factor_array": [
        ("uid", _J_INT, True, "Unique reservoir-efficiency identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "Reservoir efficiency name",
            "re1",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "turbine",
            _J_ID,
            True,
            "Turbine uid or name that this efficiency curve applies to",
            "turb1",
        ),
        (
            "reservoir",
            _J_ID,
            True,
            "Reservoir uid or name providing the hydraulic head",
            "res1",
        ),
        (
            "mean_production_factor",
            _J_NUM,
            False,
            "Mean productivity [MW/(m³/s)] averaged over operating range",
            None,
        ),
        (
            "volume",
            _J_NUM,
            False,
            "Legacy single-segment shortcut: lower-bound volume [hm³] for the "
            "(volume, slope, constant) entry promoted into `segments[]` at "
            "parse time.",
            None,
        ),
        (
            "slope",
            _J_NUM,
            False,
            "Legacy single-segment slope [MW/(m³/s)/hm³]; promoted into "
            "`segments[]` together with `volume` / `constant`.",
            None,
        ),
        (
            "constant",
            _J_NUM,
            False,
            "Legacy single-segment constant [MW/(m³/s)]; promoted into "
            "`segments[]` together with `volume` / `slope`.",
            None,
        ),
        (
            "segments",
            "JSON array",
            False,
            "Piecewise-linear efficiency segments: JSON array of "
            "{volume, slope, constant} objects",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — pumps (electrical-to-water conversion)
    # ------------------------------------------------------------------
    "pump_array": [
        ("uid", _J_INT, True, "Unique pump identifier", 1),
        ("name", _J_STR, True, "Pump name", "pmp1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "waterway",
            _J_ID,
            True,
            "Pumping waterway uid or name (junction_a = downstream intake, "
            "junction_b = upstream discharge).",
            "ww_pump1",
        ),
        (
            "demand",
            _J_ID,
            True,
            "Electrical demand uid or name (the pump load).",
            "d_pump1",
        ),
        (
            "pump_factor",
            _J_SCHED,
            False,
            "Power consumed per unit flow [MW/(m³/s)], stage-schedulable.",
            None,
        ),
        (
            "efficiency",
            _J_SCHED,
            False,
            "Pump efficiency [p.u.] (default 1.0), stage-schedulable.",
            None,
        ),
        (
            "capacity",
            _J_SCHED,
            False,
            "Maximum pump flow [m³/s], stage-schedulable.",
            None,
        ),
        (
            "main_reservoir",
            _J_ID,
            False,
            "Optional reservoir uid or name whose volume drives the pump's "
            "conversion rate (future SDDP dynamic update).",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — water rights (flow-based and volume-based)
    # ------------------------------------------------------------------
    "flow_right_array": [
        ("uid", _J_INT, True, "Unique flow-right identifier", 1),
        ("name", _J_STR, True, "FlowRight name", "fr1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "purpose",
            _J_STR,
            False,
            "Use case: 'irrigation', 'generation', 'environmental', etc. "
            "Metadata only — does not affect LP.",
            None,
        ),
        (
            "junction_a",
            _J_ID,
            False,
            "Reference junction uid or name where the right is exercised. "
            "When set, the flow is subtracted from the junction's balance.",
            None,
        ),
        (
            "junction_b",
            _J_ID,
            False,
            "Optional bypass / pressure-release junction uid or name. When "
            "set, excess flow above the consumptive cap is routed here "
            "instead of through a synthetic parallel waterway.",
            None,
        ),
        (
            "direction",
            _J_INT,
            False,
            "Direction sign: +1 = supply (inflow), -1 = withdrawal "
            "(consumptive extraction).",
            None,
        ),
        (
            "fmin",
            _J_SCHED,
            False,
            "Hard lower bound on the served flow [m³/s] per-(stage, block). Default 0.",
            None,
        ),
        (
            "fmax",
            _J_SCHED,
            False,
            "Hard upper bound on the served flow [m³/s] per-(stage, block). "
            "When unset, defaults to target (or fmin).",
            None,
        ),
        (
            "target",
            _J_SCHED,
            False,
            "Soft kink point [m³/s] per-(stage, block). Below target: fcost "
            "penalty; above target: uvalue bonus/penalty. "
            "Aliases legacy `discharge`.",
            None,
        ),
        (
            "discharge",
            _J_SCHED,
            False,
            "Legacy alias for `target`; normalized to `target` by the AMPL "
            "naming-registry before LP build.",
            None,
        ),
        (
            "flow_mode",
            _J_STR,
            False,
            "Time-resolution mode: 'per_block' (default), 'stage_average', "
            "or 'stage_uniform'.",
            None,
        ),
        (
            "use_average",
            _J_BOOL,
            False,
            "Deprecated: true → flow_mode='stage_average'; false → "
            "'per_block'. Ignored when flow_mode is set explicitly.",
            None,
        ),
        (
            "fcost",
            _J_SCHED,
            False,
            "Penalty cost for unmet flow below target [$/m³/s/h] per-(stage, block).",
            None,
        ),
        (
            "uvalue",
            _J_SCHED,
            False,
            "Value of exercising the right above target [$/m³/s/h] "
            "per-(stage, block). Positive = incentivize; negative = penalize.",
            None,
        ),
        (
            "priority",
            _J_NUM,
            False,
            "Priority level for allocation ordering [dimensionless].",
            None,
        ),
        (
            "bound_rule",
            "JSON object",
            False,
            "Volume-dependent piecewise-linear bound rule for dynamic fmax "
            "(reservoir-volume-driven cushion zones).",
            None,
        ),
    ],
    "volume_right_array": [
        ("uid", _J_INT, True, "Unique volume-right identifier", 1),
        ("name", _J_STR, True, "VolumeRight name", "vr1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "purpose",
            _J_STR,
            False,
            "Use case: 'irrigation', 'generation', 'economy', etc. "
            "Metadata only — does not affect LP.",
            None,
        ),
        (
            "reservoir",
            _J_ID,
            False,
            "Physical source reservoir uid or name. When set, the right's "
            "input flow is subtracted from the reservoir's balance.",
            None,
        ),
        (
            "right_reservoir",
            _J_ID,
            False,
            "Optional FK to another VolumeRight for hierarchical rights balance.",
            None,
        ),
        (
            "direction",
            _J_INT,
            False,
            "Direction sign on right_reservoir balance: +1 = supply, -1 = withdrawal.",
            None,
        ),
        (
            "emin",
            _J_SCHED,
            False,
            "Minimum accumulated right volume [hm³] per-(stage, block).",
            None,
        ),
        (
            "emax",
            _J_SCHED,
            False,
            "Maximum accumulated right volume [hm³] per-(stage, block).",
            None,
        ),
        (
            "ecost",
            _J_SCHED,
            False,
            "Shadow cost of accumulated rights [$/hm³] per-(stage, block).",
            None,
        ),
        (
            "eini",
            _J_NUM,
            False,
            "Initial accumulated volume at start of horizon [hm³].",
            None,
        ),
        (
            "efin",
            _J_NUM,
            False,
            "Minimum required accumulated volume at end [hm³].",
            None,
        ),
        (
            "efin_cost",
            _J_NUM,
            False,
            "Penalty cost per unit of efin shortfall [$/hm³].",
            None,
        ),
        (
            "soft_emin",
            _J_SCHED,
            False,
            "Soft minimum volume [hm³] per-(stage, block).",
            None,
        ),
        (
            "soft_emin_cost",
            _J_SCHED,
            False,
            "Penalty cost for soft_emin violation [$/hm³] per-(stage, block).",
            None,
        ),
        (
            "demand",
            _J_SCHED,
            False,
            "Required volume delivery per stage [hm³].",
            None,
        ),
        (
            "fmax",
            _J_SCHED,
            False,
            "Maximum extraction rate from the right [m³/s] per-(stage, block).",
            None,
        ),
        (
            "fail_cost",
            _J_NUM,
            False,
            "Penalty cost for unmet volume demand [$/hm³].",
            None,
        ),
        (
            "priority",
            _J_NUM,
            False,
            "Priority level for allocation ordering [dimensionless].",
            None,
        ),
        (
            "saving_rate",
            _J_SCHED,
            False,
            "Maximum saving deposit rate per block [m³/s] "
            "(only for economy VolumeRights).",
            None,
        ),
        (
            "flow_conversion_rate",
            _J_NUM,
            False,
            "Converts m³/s × hours into hm³ [hm³/(m³/s·h)] (default 0.0036).",
            None,
        ),
        (
            "use_state_variable",
            _J_BOOL,
            False,
            "Propagate accumulated volume state across phases via "
            "StateVariables (SDDP coupling).",
            None,
        ),
        (
            "annual_loss",
            _J_SCHED,
            False,
            "Annual fractional loss [p.u./year], stage-schedulable.",
            None,
        ),
        (
            "reset_month",
            _J_STR,
            False,
            "Calendar month at which rights are re-provisioned "
            "(e.g. 'april'). At reset, eini is set from bound_rule (or emax).",
            None,
        ),
        (
            "bound_rule",
            "JSON object",
            False,
            "Volume-dependent piecewise-linear rule for dynamic extraction "
            "adjustment and at-reset eini provisioning.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — user constraints
    # ------------------------------------------------------------------
    "user_constraint_array": [
        ("uid", _J_INT, True, "Unique constraint identifier", 1),
        ("name", _J_STR, True, "Human-readable constraint name", "uc1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "expression",
            _J_STR,
            True,
            "Constraint expression in AMPL-inspired syntax "
            "(e.g. 'generator(\"g1\").generation <= 300')",
            'generator("g1").generation <= 300',
        ),
        (
            "description",
            _J_STR,
            False,
            "Optional free-text description of the constraint",
            None,
        ),
        (
            "constraint_type",
            _J_STR,
            False,
            "Dual scaling hint: 'power' (default), 'energy', 'raw', or 'unitless'",
            None,
        ),
        (
            "penalty",
            _J_NUM,
            False,
            "Per-unit slack cost. When set and > 0, the constraint is "
            "relaxed via auto-created slack columns.",
            None,
        ),
        (
            "penalty_class",
            _J_STR,
            False,
            "Penalty unit hint: 'raw' (default) or 'hydro_flow'. "
            "Controls how `penalty` is interpreted physically.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — user parameters (named constants for AMPL-style scripts)
    # ------------------------------------------------------------------
    "user_param_array": [
        (
            "name",
            _J_STR,
            True,
            "Parameter name (referenced in pseudo-AMPL constraint scripts).",
            "pct_elec",
        ),
        (
            "value",
            _J_NUM,
            False,
            "Scalar parameter value (mutually exclusive with `monthly`).",
            None,
        ),
        (
            "monthly",
            "JSON array",
            False,
            "Monthly-indexed parameter [jan=0..dec=11]. Must be a 12-element "
            "array. Resolved using the stage's calendar month.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # System — free continuous decision variables (referenced by
    # user constraints via `decision_variable("X").value`)
    # ------------------------------------------------------------------
    "decision_variable_array": [
        ("uid", _J_INT, True, "Unique decision-variable identifier", 1),
        (
            "name",
            _J_STR,
            True,
            "DecisionVariable name (referenced in PAMPL user-constraint "
            'scripts as `decision_variable("name").value`).',
            "dv1",
        ),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "lower_bound",
            _J_NUM,
            False,
            "Lower bound on the LP column. Unset = free below (≥ -LP_INFINITY); "
            "set to 0 to enforce a non-negative variable.",
            None,
        ),
        (
            "upper_bound",
            _J_NUM,
            False,
            "Upper bound on the LP column. Unset = free above (≤ LP_INFINITY).",
            None,
        ),
        (
            "cost",
            _J_NUM,
            False,
            "Per-MW objective contribution [$/MWh]. Scaled by block duration "
            "via CostHelper::block_ecost so the units match `Generator.gcost`.",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # Simulation — apertures (SDDP backward-pass scenario sampling)
    # ------------------------------------------------------------------
    "aperture_array": [
        ("uid", _J_INT, True, "Unique aperture identifier", 1),
        ("name", _J_STR, False, "Optional human-readable label", "ap1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        (
            "source_scenario",
            _J_INT,
            True,
            "UID of the scenario whose affluent data to use",
            1,
        ),
        (
            "probability_factor",
            _J_NUM,
            False,
            "Probability weight of this aperture [p.u.] (default: 1.0)",
            1.0,
        ),
    ],
    # ------------------------------------------------------------------
    # Simulation — iteration overrides (SDDP per-iteration control)
    # ------------------------------------------------------------------
    "iteration_array": [
        ("index", _J_INT, True, "0-based iteration index", 0),
        (
            "update_lp",
            _J_BOOL,
            False,
            "Whether to dispatch update_lp for this iteration (default: true)",
            None,
        ),
    ],
    # ------------------------------------------------------------------
    # Options — variable scales
    # ------------------------------------------------------------------
    "variable_scales": [
        (
            "class_name",
            _J_STR,
            True,
            'Element class (e.g. "Bus", "Reservoir", "Battery")',
            "Reservoir",
        ),
        (
            "variable",
            _J_STR,
            True,
            'Variable name (e.g. "theta", "volume", "energy")',
            "volume",
        ),
        (
            "uid",
            _J_INT,
            False,
            "Element UID (-1 or omit = all elements of the class)",
            None,
        ),
        (
            "scale",
            _J_NUM,
            True,
            "Scale factor: physical_value = LP_value * scale (default: 1.0)",
            1000.0,
        ),
    ],
    # ------------------------------------------------------------------
    # Options — solver options
    # ------------------------------------------------------------------
    "solver_options": [
        (
            "algorithm",
            _J_INT,
            False,
            "LP algorithm: 0=default, 1=primal, 2=dual, 3=barrier (default: 3)",
            3,
        ),
        (
            "threads",
            _J_INT,
            False,
            "Number of parallel threads (0 = automatic, default: 0)",
            0,
        ),
        (
            "presolve",
            _J_BOOL,
            False,
            "Apply presolve optimizations (default: true)",
            True,
        ),
        (
            "optimal_eps",
            _J_NUM,
            False,
            "Optimality tolerance (nullopt = use solver default)",
            None,
        ),
        (
            "feasible_eps",
            _J_NUM,
            False,
            "Feasibility tolerance (nullopt = use solver default)",
            None,
        ),
        (
            "barrier_eps",
            _J_NUM,
            False,
            "Barrier convergence tolerance (nullopt = use solver default)",
            None,
        ),
        (
            "log_level",
            _J_INT,
            False,
            "Solver output verbosity (0 = none, default: 0)",
            0,
        ),
        (
            "time_limit",
            _J_NUM,
            False,
            "Per-solve time limit in seconds (0 = no limit); passed to the LP backend "
            "(CLP setMaximumSeconds, HiGHS time_limit)",
            None,
        ),
    ],
}
