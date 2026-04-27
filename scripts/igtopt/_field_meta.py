# SPDX-License-Identifier: BSD-3-Clause
"""Per-array field metadata for the gtopt Excel template.

Pure data — no executable logic.  Each entry in ``FIELD_META`` is a list of
``(field, json_type, required, description, example)`` tuples that drives the
header / help / example rows of the corresponding worksheet (e.g. the
``bus_array`` sheet uses ``FIELD_META["bus_array"]``).

Imported by :mod:`igtopt.template_builder`; clients should usually go through
that module, but importing the dict directly is also supported.
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
    # System — demand
    # ------------------------------------------------------------------
    "demand_array": [
        ("uid", _J_INT, True, "Unique demand identifier", 1),
        ("name", _J_STR, True, "Demand name (used in profile and output files)", "d1"),
        ("active", _J_INT, False, "1 = active, 0 = inactive (default: 1)", None),
        ("bus", _J_ID, True, "Connected bus uid or name", "b1"),
        (
            "lmax",
            _J_SCHED,
            False,
            "Maximum demand (load) [MW]: scalar, array, or filename",
            100.0,
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
        ("gcost", _J_SCHED, False, "Discharge operation cost [$/MWh]", None),
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
        (
            "energy_scale",
            _J_NUM,
            False,
            "Energy scale for LP numerics: LP var = energy / scale"
            " (optional, default: 1.0)",
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
            True,
            "Downstream junction uid or name",
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
            "Cost per unit of spilled water [$/m³/s]",
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
            "energy_scale",
            _J_NUM,
            False,
            "Energy scaling factor [hm³/unit] (default: 1.0)",
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
            "Seepage slope [m³/s per dam³] — scalar, per-stage array, or filename. "
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
            "segments",
            "JSON array",
            False,
            "Piecewise-linear seepage curve (plpfilemb.dat model): JSON array "
            "of {volume [dam³], slope [m³/s per dam³], constant [m³/s]} objects. "
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
            True,
            "Waterway uid or name whose discharge is limited",
            "ww1",
        ),
        (
            "reservoir",
            _J_ID,
            True,
            "Reservoir uid or name providing the volume reference",
            "res1",
        ),
        (
            "segments",
            "JSON array",
            False,
            "Piecewise-linear discharge-limit curve: JSON array of "
            "{volume [dam³], slope [m³/s per dam³], intercept [m³/s]} objects. "
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
            "segments",
            "JSON array",
            False,
            "Piecewise-linear efficiency segments: JSON array of "
            "{volume, slope, constant} objects",
            None,
        ),
        (
            "sddp_production_factor_update_skip",
            _J_INT,
            False,
            "SDDP iterations between efficiency updates (default: 1)",
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
