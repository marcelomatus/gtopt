# SPDX-License-Identifier: BSD-3-Clause
"""Template builder for igtopt – generates the gtopt Excel template from C++ headers."""

from __future__ import annotations

import pathlib
import re
import sys
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
            "conversion_rate",
            _J_SCHED,
            False,
            "Water-to-power conversion [MW/(m³/s)] (productivity)",
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

# ------------------------------------------------------------------
# Options sheet metadata
# (field, description, default_value_or_None)
#
# The flat Excel sheet uses a single "options" tab.  igtopt.py
# partitions these keys into top-level, sddp_options,
# cascade_options, and monolithic_options sub-objects when writing
# the JSON output.
# Keys in SDDP_OPTION_KEYS go into "sddp_options".
# Keys prefixed with "cascade_" go into "cascade_options"
# (with the prefix stripped).
# Keys prefixed with "monolithic_" go into "monolithic_options"
# (with the prefix stripped).
# ------------------------------------------------------------------

# Keys that belong inside the ``sddp_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<SddpOptions>`` in
# ``include/gtopt/json/json_options.hpp``.
SDDP_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "cut_sharing_mode",
        "cut_directory",
        "api_enabled",
        "production_factor_update_skip",
        "max_iterations",
        "min_iterations",
        "convergence_tol",
        "elastic_penalty",
        "alpha_min",
        "alpha_max",
        "cut_recovery_mode",
        "recovery_mode",
        "save_per_iteration",
        "cuts_input_file",
        "sentinel_file",
        "elastic_mode",
        "multi_cut_threshold",
        "apertures",
        "num_apertures",
        "aperture_directory",
        "aperture_timeout",
        "save_aperture_lp",
        "boundary_cuts_file",
        "boundary_cuts_mode",
        "boundary_max_iterations",
        "named_cuts_file",
        "max_cuts_per_phase",
        "cut_prune_interval",
        "prune_dual_threshold",
        "single_cut_storage",
        "max_stored_cuts",
        "use_clone_pool",
        "simulation_mode",
        "state_propagation",
        "warm_start",
        "stationary_tol",
        "stationary_window",
        "update_lp_skip",
        "forward_solver_options",
        "backward_solver_options",
    }
)

# Cascade options now use a hierarchical ``levels`` array structure
# that is too complex for the flat Excel template.  Cascade
# configuration should be done directly in JSON.  This frozenset is
# kept empty for backward compatibility with imports.
CASCADE_OPTION_KEYS: frozenset[str] = frozenset()

# Keys that belong inside the ``monolithic_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<MonolithicOptions>``.
# In the flat Excel sheet these are prefixed with ``monolithic_`` to
# distinguish them from the identically-named SDDP fields (e.g.
# ``monolithic_boundary_cuts_file`` → ``boundary_cuts_file``).
MONOLITHIC_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "solve_mode",
        "boundary_cuts_file",
        "boundary_cuts_mode",
        "boundary_max_iterations",
        "solver_options",
    }
)

# Keys that belong inside the ``solver_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<SolverOptions>`` in
# ``include/gtopt/json/json_solver_options.hpp``.
# In the flat Excel sheet these are prefixed with ``solver_`` to
# distinguish them from other options (e.g.
# ``solver_time_limit`` → ``time_limit``).
SOLVER_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "algorithm",
        "threads",
        "presolve",
        "optimal_eps",
        "feasible_eps",
        "barrier_eps",
        "log_level",
        "time_limit",
        "reuse_basis",
    }
)

_OPTIONS_FIELDS: list[tuple[str, str, Any]] = [
    # ------------------------------------------------------------------
    # Top-level options (stay at root of "options" JSON object)
    # ------------------------------------------------------------------
    ("input_directory", "Directory for input time-series files", "input"),
    (
        "input_format",
        "Preferred input file format: 'parquet' or 'csv'",
        "parquet",
    ),
    ("demand_fail_cost", "Penalty for unserved load [$/MWh]", 1000),
    ("reserve_fail_cost", "Penalty for unserved spinning reserve [$/MW]", 5000),
    ("use_line_losses", "Enable line loss modelling (true/false)", True),
    (
        "loss_segments",
        "Number of piecewise-linear loss segments (1=linear only)",
        1,
    ),
    ("use_kirchhoff", "Apply DC Kirchhoff OPF constraints (true/false)", True),
    (
        "use_single_bus",
        "Copper-plate (no network) mode – ignores all line limits (true/false)",
        False,
    ),
    (
        "kirchhoff_threshold",
        "Minimum bus voltage [kV] for Kirchhoff constraints",
        None,
    ),
    (
        "scale_objective",
        "Divide objective coefficients by this value for solver numerics",
        1000,
    ),
    (
        "scale_theta",
        "Angle variable scaling factor (default: 1000)",
        1000,
    ),
    (
        "annual_discount_rate",
        "Annual discount rate for CAPEX [p.u.] (e.g. 0.10 = 10 %)",
        0.1,
    ),
    ("output_directory", "Directory for solution output files", "output"),
    ("output_format", "Output file format: 'parquet' or 'csv'", "parquet"),
    (
        "output_compression",
        "Parquet compression codec: 'gzip', 'snappy', 'zstd', or ''",
        "gzip",
    ),
    (
        "use_lp_names",
        "LP naming level: 0=none, 1=names+warn, 2=names+error",
        None,
    ),
    (
        "use_uid_fname",
        "Use uid-based filenames for output (true/false)",
        None,
    ),
    (
        "method",
        "Planning method: 'monolithic' (default), 'sddp', or 'cascade'",
        None,
    ),
    ("log_directory", "Directory for solver log files", "logs"),
    (
        "lp_debug",
        "Save debug LP files to log directory (true/false)",
        None,
    ),
    (
        "lp_compression",
        "Compression codec for debug LP files (e.g. 'gzip')",
        None,
    ),
    ("lp_build", "Build LP without solving (true/false)", None),
    (
        "lp_coeff_ratio_threshold",
        "Warn when LP coefficient ratio exceeds this value",
        None,
    ),
    # ------------------------------------------------------------------
    # Solver options (nested into "solver_options" in JSON output)
    # In the flat Excel sheet these use a "solver_" prefix.
    # The prefix is stripped when writing the JSON sub-object.
    # ------------------------------------------------------------------
    (
        "solver_algorithm",
        "[solver] LP algorithm: 0=default, 1=primal, 2=dual, 3=barrier (default: 3)",
        3,
    ),
    (
        "solver_threads",
        "[solver] Number of parallel LP threads (0 = automatic, default: 0)",
        0,
    ),
    (
        "solver_presolve",
        "[solver] Apply LP presolve optimizations (true/false, default: true)",
        True,
    ),
    (
        "solver_time_limit",
        "[solver] Per-solve time limit in seconds; "
        "passed to LP backend (CLP setMaximumSeconds, HiGHS time_limit). "
        "0 = no limit",
        None,
    ),
    (
        "solver_optimal_eps",
        "[solver] Optimality tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_feasible_eps",
        "[solver] Feasibility tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_barrier_eps",
        "[solver] Barrier convergence tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_log_level",
        "[solver] Solver output verbosity (0 = none, default: 0)",
        0,
    ),
    (
        "solver_reuse_basis",
        "[solver] Enable basis-reuse for resolves (true/false, default: false)",
        None,
    ),
    # ------------------------------------------------------------------
    # SDDP options (nested into "sddp_options" in JSON output)
    # ------------------------------------------------------------------
    (
        "cut_sharing_mode",
        "[sddp] How Benders cuts are shared: 'none', 'expected', 'accumulate', or 'max'",
        None,
    ),
    (
        "cut_directory",
        "[sddp] Directory for SDDP Benders cut files",
        "cuts",
    ),
    (
        "api_enabled",
        "[sddp] Write SDDP status JSON for monitoring (true/false)",
        None,
    ),
    (
        "production_factor_update_skip",
        "[sddp] SDDP iterations between reservoir efficiency updates",
        None,
    ),
    ("max_iterations", "[sddp] Maximum SDDP outer iterations", None),
    ("min_iterations", "[sddp] Minimum SDDP outer iterations", None),
    (
        "convergence_tol",
        "[sddp] SDDP convergence tolerance (gap between bounds)",
        None,
    ),
    (
        "elastic_penalty",
        "[sddp] Penalty for elastic constraint relaxation",
        None,
    ),
    ("alpha_min", "[sddp] Minimum alpha (future cost) lower bound", None),
    ("alpha_max", "[sddp] Maximum alpha (future cost) upper bound", None),
    (
        "cut_recovery_mode",
        "[sddp] Cut persistence mode: 'none' (default), 'keep', 'append', or 'replace'",
        None,
    ),
    (
        "recovery_mode",
        "[sddp] Recovery mode: 'none' (0), 'cuts' (1), or 'full' (2, default)",
        None,
    ),
    (
        "warm_start",
        "[sddp] Enable warm-start for SDDP resolves (true/false, default: true)",
        None,
    ),
    (
        "save_per_iteration",
        "[sddp] Save cuts after every iteration (true/false)",
        None,
    ),
    (
        "cuts_input_file",
        "[sddp] Path to pre-computed Benders cuts file",
        None,
    ),
    (
        "sentinel_file",
        "[sddp] Path to sentinel file that stops SDDP early",
        None,
    ),
    (
        "elastic_mode",
        "[sddp] Elastic filter mode: 'single_cut', 'multi_cut', or 'backpropagate'",
        None,
    ),
    (
        "multi_cut_threshold",
        "[sddp] Threshold for multi-cut aggregation",
        None,
    ),
    (
        "num_apertures",
        "[sddp] Number of backward-pass apertures",
        None,
    ),
    (
        "aperture_directory",
        "[sddp] Directory for aperture definition files",
        None,
    ),
    (
        "aperture_timeout",
        "[sddp] Timeout in seconds for each aperture solve",
        None,
    ),
    (
        "save_aperture_lp",
        "[sddp] Save LP files for infeasible apertures (true/false)",
        None,
    ),
    (
        "boundary_cuts_file",
        "[sddp] Path to boundary (future-cost) cuts CSV for last stage",
        None,
    ),
    (
        "boundary_cuts_mode",
        "[sddp] Boundary cuts load mode: 'noload', 'separated', 'combined'",
        "separated",
    ),
    (
        "boundary_max_iterations",
        "[sddp] Max SDDP iterations to load from boundary cuts (0=all)",
        0,
    ),
    (
        "named_cuts_file",
        "[sddp] Path to named cuts file for warm-starting SDDP",
        None,
    ),
    (
        "max_cuts_per_phase",
        "[sddp] Max retained cuts per (scene,phase) LP (0=unlimited, no pruning)",
        0,
    ),
    (
        "cut_prune_interval",
        "[sddp] Iterations between cut pruning passes (requires max_cuts_per_phase>0)",
        10,
    ),
    (
        "prune_dual_threshold",
        "[sddp] Dual threshold for inactive cut detection during pruning",
        1e-8,
    ),
    (
        "single_cut_storage",
        "[sddp] Store cuts per-scene only, build combined on demand (saves memory)",
        False,
    ),
    (
        "max_stored_cuts",
        "[sddp] Max stored cuts per scene (0=unlimited; oldest dropped first)",
        0,
    ),
    (
        "use_clone_pool",
        "[sddp] Reuse cached LP clones for aperture solves (avoids repeated allocation)",
        True,
    ),
    (
        "simulation_mode",
        "[sddp] Forward-only evaluation of loaded cuts, no training (true/false)",
        None,
    ),
    # NOTE: Cascade options now use a hierarchical ``levels`` array
    # structure (with lp_options, solver, and transition sub-objects per
    # level).  This is too complex for the flat Excel template; cascade
    # configuration should be done directly in JSON.
    # ------------------------------------------------------------------
    # Monolithic options (nested into "monolithic_options" in JSON output)
    # In the flat Excel sheet these use a "monolithic_" prefix to avoid
    # name collisions with the SDDP options above.  The prefix is
    # stripped when writing the JSON sub-object.
    # ------------------------------------------------------------------
    (
        "monolithic_solve_mode",
        "[monolithic] Solve mode: 'monolithic' or 'relaxed'",
        None,
    ),
    (
        "monolithic_boundary_cuts_file",
        "[monolithic] Path to boundary cuts CSV for monolithic solver",
        None,
    ),
    (
        "monolithic_boundary_cuts_mode",
        "[monolithic] Boundary cuts mode: 'noload', 'separated', 'combined'",
        None,
    ),
    (
        "monolithic_boundary_max_iterations",
        "[monolithic] Max iterations to load from boundary cuts (0=all)",
        None,
    ),
]

# ------------------------------------------------------------------
# Ordered sheet list (determines workbook tab order)
# ------------------------------------------------------------------
_TEMPLATE_SIMULATION_SHEETS = [
    "block_array",
    "stage_array",
    "scenario_array",
    "phase_array",
    "scene_array",
    "aperture_array",
    "iteration_array",
]

_TEMPLATE_SYSTEM_SHEETS = [
    "bus_array",
    "generator_array",
    "generator_profile_array",
    "demand_array",
    "demand_profile_array",
    "line_array",
    "battery_array",
    "converter_array",
    "reserve_zone_array",
    "reserve_provision_array",
    "junction_array",
    "waterway_array",
    "flow_array",
    "reservoir_array",
    "reservoir_seepage_array",
    "reservoir_discharge_limit_array",
    "turbine_array",
    "reservoir_production_factor_array",
    "user_constraint_array",
]

# ------------------------------------------------------------------
# Introduction sheet content
# ------------------------------------------------------------------
_INTRO_LINES = [
    ("gtopt Planning Template", "TITLE"),
    ("", ""),
    (
        "This workbook is a ready-to-use template for building gtopt planning "
        "cases with the igtopt converter.",
        "BODY",
    ),
    (
        "Fill in each sheet with your system data, then run:  igtopt <this_file>.xlsx",
        "BODY",
    ),
    ("", ""),
    ("Sheet Guide", "HEADING"),
    ("", ""),
    ("Sheet name", "Header", "Description", "TABLE_HEADER"),
    ("options", "", "Global solver and I/O options (one row per option key)", "TABLE"),
    (
        "block_array",
        "Simulation",
        "Operating time blocks (hours)",
        "TABLE",
    ),
    (
        "stage_array",
        "Simulation",
        "Planning stages (groups of blocks)",
        "TABLE",
    ),
    (
        "scenario_array",
        "Simulation",
        "Probability-weighted scenarios",
        "TABLE",
    ),
    (
        "phase_array",
        "Simulation",
        "SDDP phases (groups of stages) — leave empty for monolithic",
        "TABLE",
    ),
    (
        "scene_array",
        "Simulation",
        "SDDP scenes (groups of scenarios) — leave empty for monolithic",
        "TABLE",
    ),
    (
        "aperture_array",
        "Simulation",
        "SDDP backward-pass apertures (scenario sampling) — leave empty for default",
        "TABLE",
    ),
    (
        "iteration_array",
        "Simulation",
        "SDDP per-iteration control flags — leave empty for defaults",
        "TABLE",
    ),
    ("bus_array", "System", "Electrical busbars (nodes)", "TABLE"),
    (
        "generator_array",
        "System",
        "Thermal / renewable / hydro generators",
        "TABLE",
    ),
    (
        "generator_profile_array",
        "System",
        "Time-varying capacity-factor profiles for generators",
        "TABLE",
    ),
    ("demand_array", "System", "Electricity consumers (loads)", "TABLE"),
    (
        "demand_profile_array",
        "System",
        "Time-varying load-shape profiles for demands",
        "TABLE",
    ),
    ("line_array", "System", "Transmission lines / branches", "TABLE"),
    (
        "battery_array",
        "System",
        "Energy storage devices (batteries, pumped-hydro, etc.)",
        "TABLE",
    ),
    (
        "converter_array",
        "System",
        "Battery charge/discharge converter links",
        "TABLE",
    ),
    ("reserve_zone_array", "System", "Spinning-reserve zones", "TABLE"),
    (
        "reserve_provision_array",
        "System",
        "Generator contributions to reserve zones",
        "TABLE",
    ),
    ("junction_array", "System", "Hydraulic junctions (nodes)", "TABLE"),
    (
        "waterway_array",
        "System",
        "Water channels between junctions",
        "TABLE",
    ),
    (
        "flow_array",
        "System",
        "Exogenous inflows / outflows at junctions",
        "TABLE",
    ),
    (
        "reservoir_array",
        "System",
        "Water reservoirs (lakes, dams)",
        "TABLE",
    ),
    (
        "reservoir_seepage_array",
        "System",
        "Water seepage from waterways into reservoirs",
        "TABLE",
    ),
    (
        "reservoir_discharge_limit_array",
        "System",
        "Volume-dependent discharge limits for reservoirs (Ralco-type constraints)",
        "TABLE",
    ),
    (
        "turbine_array",
        "System",
        "Hydro turbines linking waterways to generators",
        "TABLE",
    ),
    (
        "reservoir_production_factor_array",
        "System",
        "Volume-dependent turbine productivity curves",
        "TABLE",
    ),
    (
        "user_constraint_array",
        "System",
        "User-defined linear constraints (AMPL-inspired syntax)",
        "TABLE",
    ),
    ("", ""),
    ("Time-series Sheets (@-sheets)", "HEADING"),
    ("", ""),
    (
        "Sheets named  Element@field  (e.g. 'Demand@lmax', 'GeneratorProfile@profile')",
        "BODY",
    ),
    (
        "are written as Parquet/CSV files to the input directory. Column headers",
        "BODY",
    ),
    (
        "must be: scenario, stage, block, then one column per element (by name).",
        "BODY",
    ),
    ("", ""),
    ("Conventions", "HEADING"),
    ("", ""),
    ("• Sheets / columns whose name starts with '.' are silently skipped.", "BODY"),
    (
        "• Leave cells empty (or use NaN) for optional fields — they are omitted from JSON.",
        "BODY",
    ),
    (
        "• Fields of type  number|array|filename  accept a scalar, an inline array",
        "BODY",
    ),
    ("  (JSON syntax, e.g. [100,90,80]), or a filename stem.", "BODY"),
    (
        "• The 'active' column accepts 1 (active) or 0 (inactive); "
        "leave blank to use the default (1).",
        "BODY",
    ),
    ("", ""),
    (
        "Generated by igtopt --make-template — re-run to refresh after C++ changes.",
        "FOOTER",
    ),
]

# ------------------------------------------------------------------
# Example rows for common sheets
# ------------------------------------------------------------------
_EXAMPLES: dict[str, list[dict[str, Any]]] = {
    "block_array": [{"uid": 1, "name": "b1", "duration": 1.0}],
    "stage_array": [{"uid": 1, "name": "s1", "first_block": 0, "count_block": 8760}],
    "scenario_array": [
        {"uid": 1, "name": "sc1", "active": 1, "probability_factor": 1.0}
    ],
    "bus_array": [
        {"uid": 1, "name": "b1", "voltage": 220.0},
        {"uid": 2, "name": "b2", "voltage": 220.0},
    ],
    "generator_array": [
        {"uid": 1, "name": "g1", "bus": "b1", "pmax": 100.0, "gcost": 20.0},
        {"uid": 2, "name": "g2", "bus": "b2", "pmax": 200.0, "gcost": 35.0},
    ],
    "demand_array": [
        {"uid": 1, "name": "d1", "bus": "b1", "lmax": 80.0},
        {"uid": 2, "name": "d2", "bus": "b2", "lmax": 150.0},
    ],
    "line_array": [
        {
            "uid": 1,
            "name": "l1_2",
            "bus_a": "b1",
            "bus_b": "b2",
            "tmax_ab": 200.0,
            "tmax_ba": 200.0,
            "reactance": 0.05,
        }
    ],
    "options": [],  # handled separately — no example rows in data sheet
}


def _find_repo_root(start: pathlib.Path) -> pathlib.Path:
    """Walk up the directory tree to find the repository root."""
    resolved = start.resolve()
    for parent in [resolved, *resolved.parents]:
        if (parent / "include" / "gtopt").is_dir():
            return parent
    return resolved


# Regex to extract json_member field names from JSON headers
_FIELD_NAME_RE = re.compile(r'json_\w+<"([^"]+)"')


def parse_json_header_fields(header_path: pathlib.Path) -> list[str]:
    """Extract JSON field names from a json_*.hpp header file.

    Returns the ordered list of field names as they appear in the
    json_member_list for the *last* struct defined in the file
    (which is typically the main user-facing struct, not the Attrs variant).
    """
    text = header_path.read_text(encoding="utf-8", errors="ignore")
    fields = _FIELD_NAME_RE.findall(text)
    seen: set[str] = set()
    result = []
    for f in fields:
        if f not in seen:
            seen.add(f)
            result.append(f)
    return result


def parse_system_arrays(json_system_hpp: pathlib.Path) -> list[str]:
    """Parse json_system.hpp to get the ordered list of array field names."""
    text = json_system_hpp.read_text(encoding="utf-8", errors="ignore")
    return re.findall(r'json_array_null<"([^"]+)"', text)


def parse_simulation_arrays(json_simulation_hpp: pathlib.Path) -> list[str]:
    """Parse json_simulation.hpp to get the ordered list of array field names."""
    text = json_simulation_hpp.read_text(encoding="utf-8", errors="ignore")
    return re.findall(r'json_array_null<"([^"]+)"', text)


def _build_workbook(  # noqa: PLR0912,PLR0915
    output_path: pathlib.Path, header_dir: pathlib.Path
) -> None:
    """Build the Excel template workbook and write to *output_path*."""
    try:
        import openpyxl
        from openpyxl.styles import (
            Alignment,
            Font,
            PatternFill,
        )
        from openpyxl.utils import get_column_letter
    except ImportError as exc:
        print(
            f"Error: openpyxl is required.  Install it with: pip install openpyxl\n{exc}",
            file=sys.stderr,
        )
        sys.exit(1)

    # Styles
    TITLE_FONT = Font(name="Calibri", size=16, bold=True, color="1F3864")
    HEADING_FONT = Font(name="Calibri", size=12, bold=True, color="2F5496")
    BODY_FONT = Font(name="Calibri", size=10)
    FOOTER_FONT = Font(name="Calibri", size=9, italic=True, color="808080")
    HEADER_FILL = PatternFill("solid", fgColor="1F3864")
    ALT_ROW_FILL = PatternFill("solid", fgColor="DCE6F1")
    HELP_FILL = PatternFill("solid", fgColor="EBF3FB")
    REQUIRED_FILL = PatternFill("solid", fgColor="FFF2CC")
    HEADER_FONT_WHITE = Font(name="Calibri", bold=True, color="FFFFFF")
    TABLE_HEADER_FILL = PatternFill("solid", fgColor="4472C4")
    TABLE_HEADER_FONT = Font(name="Calibri", bold=True, color="FFFFFF")

    wb = openpyxl.Workbook()
    default_sheet = wb.active
    default_sheet.title = ".introduction"

    # ------------------------------------------------------------------
    # .introduction sheet
    # ------------------------------------------------------------------
    ws = default_sheet
    ws.column_dimensions["A"].width = 28
    ws.column_dimensions["B"].width = 14
    ws.column_dimensions["C"].width = 58

    row = 1
    for entry in _INTRO_LINES:
        if entry[-1] == "TITLE":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = TITLE_FONT
            ws.merge_cells(f"A{row}:C{row}")
        elif entry[-1] == "HEADING":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = HEADING_FONT
            ws.merge_cells(f"A{row}:C{row}")
        elif entry[-1] == "BODY":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = BODY_FONT
            ws.merge_cells(f"A{row}:C{row}")
        elif entry[-1] == "FOOTER":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = FOOTER_FONT
            ws.merge_cells(f"A{row}:C{row}")
        elif entry[-1] == "TABLE_HEADER":
            for col_idx, val in enumerate(entry[:3], start=1):
                c = ws.cell(row=row, column=col_idx, value=val)
                c.font = TABLE_HEADER_FONT
                c.fill = TABLE_HEADER_FILL
        elif entry[-1] == "TABLE":
            for col_idx, val in enumerate(entry[:3], start=1):
                c = ws.cell(row=row, column=col_idx, value=val)
                c.font = BODY_FONT
                if row % 2 == 0:
                    c.fill = ALT_ROW_FILL
        elif entry[0] == "":
            pass  # blank spacer row
        row += 1

    # ------------------------------------------------------------------
    # options sheet
    # ------------------------------------------------------------------
    ws_opts = wb.create_sheet("options")
    ws_opts.column_dimensions["A"].width = 34
    ws_opts.column_dimensions["B"].width = 22
    ws_opts.column_dimensions["C"].width = 64

    # Header row
    for col_idx, header in enumerate(["option", "value", "description"], start=1):
        c = ws_opts.cell(row=1, column=col_idx, value=header)
        c.font = HEADER_FONT_WHITE
        c.fill = HEADER_FILL
        c.alignment = Alignment(horizontal="left")

    for row_idx, (key, desc, default) in enumerate(_OPTIONS_FIELDS, start=2):
        ws_opts.cell(row=row_idx, column=1, value=key).font = Font(bold=False)
        if default is not None:
            ws_opts.cell(row=row_idx, column=2, value=default)
        ws_opts.cell(row=row_idx, column=3, value=desc).font = Font(
            italic=True, color="404040"
        )

    ws_opts.freeze_panes = "A2"

    # ------------------------------------------------------------------
    # Helper to build a data sheet
    # ------------------------------------------------------------------
    def _add_data_sheet(
        sheet_name: str,
        fields: list[tuple[str, str, bool, str, Any]],
        examples: list[dict[str, Any]] | None = None,
    ) -> None:
        ws_data = wb.create_sheet(sheet_name)

        num_fields = len(fields)
        for col_idx, (fname, ftype, required, desc, _example) in enumerate(
            fields, start=1
        ):
            # Header row
            header_cell = ws_data.cell(row=1, column=col_idx, value=fname)
            header_cell.font = HEADER_FONT_WHITE
            if required:
                header_cell.fill = HEADER_FILL  # dark blue = required
            else:
                header_cell.fill = PatternFill("solid", fgColor="2F5496")  # medium
            header_cell.alignment = Alignment(horizontal="left")

            # Help row
            help_text = f"[{ftype}]{'*' if required else ''} {desc}"
            help_cell = ws_data.cell(row=2, column=col_idx, value=help_text)
            help_cell.font = Font(name="Calibri", size=8, italic=True, color="404040")
            if required:
                help_cell.fill = REQUIRED_FILL
            else:
                help_cell.fill = HELP_FILL

        # Column widths
        for col_idx, (fname, _ft, _req, _desc, _ex) in enumerate(fields, start=1):
            col_letter = get_column_letter(col_idx)
            ws_data.column_dimensions[col_letter].width = max(14, len(fname) + 4)

        # Example rows
        if examples:
            for row_offset, example_row in enumerate(examples):
                data_row = 3 + row_offset
                for col_idx, (fname, _ft, _req, _desc, _default_ex) in enumerate(
                    fields, start=1
                ):
                    val = example_row.get(fname, None)
                    if val is not None:
                        ws_data.cell(row=data_row, column=col_idx, value=val)
        else:
            # Pre-fill a single blank example row using per-field defaults
            for col_idx, (_fname, _ft, _req, _desc, example) in enumerate(
                fields, start=1
            ):
                if example is not None:
                    ws_data.cell(row=3, column=col_idx, value=example)

        ws_data.freeze_panes = "A3"

        # Legend in a far-right column
        legend_col = num_fields + 2
        leg_label = ws_data.cell(row=1, column=legend_col, value="Legend")
        leg_label.font = Font(bold=True)
        ws_data.cell(row=2, column=legend_col, value="Dark blue = required")
        ws_data.cell(row=3, column=legend_col, value="Medium blue = optional")
        ws_data.cell(row=4, column=legend_col, value="* = required field")
        ws_data.cell(
            row=5,
            column=legend_col,
            value="number|array|filename: scalar, inline array, or filename",
        )
        ws_data.column_dimensions[get_column_letter(legend_col)].width = 46

    # ------------------------------------------------------------------
    # Simulation sheets
    # ------------------------------------------------------------------
    for sheet in _TEMPLATE_SIMULATION_SHEETS:
        fields = FIELD_META.get(sheet, [])
        if not fields:
            continue
        examples = _EXAMPLES.get(sheet)
        _add_data_sheet(sheet, fields, examples)

    # ------------------------------------------------------------------
    # System sheets
    # ------------------------------------------------------------------
    for sheet in _TEMPLATE_SYSTEM_SHEETS:
        fields = FIELD_META.get(sheet, [])
        if not fields:
            continue
        examples = _EXAMPLES.get(sheet)
        _add_data_sheet(sheet, fields, examples)

    # ------------------------------------------------------------------
    # Example time-series sheets
    # ------------------------------------------------------------------
    # Demand@lmax
    ws_ts = wb.create_sheet("Demand@lmax")
    for col_idx, header in enumerate(
        ["scenario", "stage", "block", "d1", "d2"], start=1
    ):
        c = ws_ts.cell(row=1, column=col_idx, value=header)
        c.font = HEADER_FONT_WHITE
        c.fill = HEADER_FILL
    ws_ts.cell(row=2, column=1, value=1)
    ws_ts.cell(row=2, column=2, value=1)
    ws_ts.cell(row=2, column=3, value=1)
    ws_ts.cell(row=2, column=4, value=80.0)
    ws_ts.cell(row=2, column=5, value=150.0)
    ws_ts.freeze_panes = "A2"

    # GeneratorProfile@profile
    ws_ts2 = wb.create_sheet("GeneratorProfile@profile")
    for col_idx, header in enumerate(
        ["scenario", "stage", "block", "g_solar"], start=1
    ):
        c = ws_ts2.cell(row=1, column=col_idx, value=header)
        c.font = HEADER_FONT_WHITE
        c.fill = HEADER_FILL
    ws_ts2.cell(row=2, column=1, value=1)
    ws_ts2.cell(row=2, column=2, value=1)
    ws_ts2.cell(row=2, column=3, value=1)
    ws_ts2.cell(row=2, column=4, value=0.75)
    ws_ts2.freeze_panes = "A2"

    # ------------------------------------------------------------------
    # Boundary cuts sheet (SDDP future-cost function / varphi)
    # ------------------------------------------------------------------
    ws_bc = wb.create_sheet("boundary_cuts")
    bc_headers = [
        "name",
        "iteration",
        "scene",
        "rhs",
        "Reservoir1",
        "Reservoir2",
    ]
    for col_idx, header in enumerate(bc_headers, start=1):
        cell = ws_bc.cell(row=1, column=col_idx, value=header)
        cell.font = HEADER_FONT_WHITE
        cell.fill = HEADER_FILL
    # Example row
    ws_bc.cell(row=2, column=1, value="bc_1_1")
    ws_bc.cell(row=2, column=2, value=1)
    ws_bc.cell(row=2, column=3, value=1)
    ws_bc.cell(row=2, column=4, value=-5000.0)
    ws_bc.cell(row=2, column=5, value=0.25)
    ws_bc.cell(row=2, column=6, value=0.75)
    ws_bc.freeze_panes = "A2"
    # Add a note explaining the sheet
    ws_bc.cell(
        row=4,
        column=1,
        value="# Columns after 'rhs' are state-variable names",
    )
    ws_bc.cell(
        row=5,
        column=1,
        value="# (reservoir/junction names). Values are gradient coefficients.",
    )
    ws_bc.cell(
        row=6,
        column=1,
        value="# 'iteration' = SDDP iteration (PLP IPDNumIte); "
        "'scene' = scene UID (PLP ISimul maps to scene UID in plp2gtopt).",
    )

    wb.save(output_path)
    print(f"Template written to: {output_path}", file=sys.stderr)


def _list_sheets(header_dir: pathlib.Path) -> None:
    """Print the sheets that would be generated (does not write a file)."""
    json_dir = header_dir / "json"
    if not json_dir.is_dir():
        json_dir = header_dir

    sim_file = json_dir / "json_simulation.hpp"
    sys_file = json_dir / "json_system.hpp"

    print("# Simulation sheets (from json_simulation.hpp):")
    if sim_file.exists():
        for arr in parse_simulation_arrays(sim_file):
            print(f"  {arr}")
    else:
        for arr in _TEMPLATE_SIMULATION_SHEETS:
            print(f"  {arr}  (fallback — json_simulation.hpp not found)")

    print("\n# System sheets (from json_system.hpp):")
    if sys_file.exists():
        for arr in parse_system_arrays(sys_file):
            print(f"  {arr}")
    else:
        for arr in _TEMPLATE_SYSTEM_SHEETS:
            print(f"  {arr}  (fallback — json_system.hpp not found)")
