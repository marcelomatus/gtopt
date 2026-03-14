"""Convert gtopt JSON cases to pandapower networks for DC OPF.

The conversion handles the subset of gtopt elements that pandapower
can model:

- **Bus** → ``net.bus``
- **Generator** → ``net.gen`` (and ``net.ext_grid`` for the reference bus)
- **Demand** → ``net.load``
- **Line** → ``net.line`` or ``net.trafo`` (depending on ``type`` field)

Hydro-specific elements (Junction, Waterway, Reservoir, Turbine, Flow,
Filtration) are silently skipped since pandapower does not have
equivalents.

For multi-block / multi-scenario cases the converter produces one
pandapower network per (scenario, block) pair with the appropriate
demand and generator profiles applied.

Usage::

    from gtopt2pp.convert import convert, load_gtopt_case

    # Load a gtopt JSON case and get a pandapower net for block 0
    case = load_gtopt_case("ieee_9b.json")
    net = convert(case, scenario=0, block=0)

    # Run pandapower DC OPF
    import pandapower as pp
    pp.rundcopp(net)
"""

import json
import math
from pathlib import Path
from typing import Any

import pandapower as pp

_BASE_MVA = 100.0
_TMAX_UNLIMITED = 9999.0


def load_gtopt_case(path: str | Path) -> dict[str, Any]:
    """Load a gtopt JSON case file.

    Parameters
    ----------
    path
        Path to the ``.json`` file.

    Returns
    -------
    dict
        The parsed JSON as a Python dictionary.

    Raises
    ------
    FileNotFoundError
        If *path* does not exist.
    """
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"gtopt case not found: {p}")
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def _get_bus_uid_to_idx(system: dict[str, Any]) -> dict[int, int]:
    """Build a mapping from gtopt bus UID → 0-based pandapower bus index."""
    bus_array = system.get("bus_array", [])
    return {int(b["uid"]): i for i, b in enumerate(bus_array)}


def _get_reference_bus_uid(system: dict[str, Any]) -> int | None:
    """Return the UID of the reference bus (theta = 0), or None."""
    for bus in system.get("bus_array", []):
        if "reference_theta" in bus:
            return int(bus["uid"])
    return None


def _resolve_field_sched(
    field: Any,
    scenario_idx: int,
    block_idx: int,
) -> float | None:
    """Resolve a FieldSched value to a scalar for a given scenario/block.

    FieldSched can be:
    - A scalar (int/float) → return directly
    - A nested list [[v1, v2, ...], ...] → index [scenario][block]
    - A single list [v1, v2, ...] → index [block]
    - A string (FileSched) → not supported, return None
    - None → return None
    """
    if field is None:
        return None
    if isinstance(field, (int, float)):
        return float(field)
    if isinstance(field, str):
        # FileSched (Parquet file reference) — cannot resolve without I/O
        return None
    if isinstance(field, list):
        if not field:
            return None
        if isinstance(field[0], list):
            # Nested: [[scenario_0_blocks], [scenario_1_blocks], ...]
            if scenario_idx < len(field):
                row = field[scenario_idx]
                if isinstance(row, list) and block_idx < len(row):
                    return float(row[block_idx])
            return None
        # Flat list: [block_0, block_1, ...]
        if block_idx < len(field):
            return float(field[block_idx])
        return None
    return None


def _pu_to_ohm(x_pu: float, base_kv: float, base_mva: float = 100.0) -> float:
    """Convert per-unit reactance to Ohm."""
    z_base = base_kv**2 / base_mva
    return x_pu * z_base


def convert(
    case: dict[str, Any],
    scenario: int = 0,
    block: int = 0,
    base_mva: float = _BASE_MVA,
) -> pp.pandapowerNet:
    """Convert a gtopt JSON case to a pandapower network.

    Produces a pandapower network suitable for ``pp.rundcopp()`` (DC OPF).
    Demand and generator values are resolved for the specified
    (scenario, block) pair.

    Parameters
    ----------
    case
        Parsed gtopt JSON (as returned by :func:`load_gtopt_case`).
    scenario
        0-based scenario index (default 0).
    block
        0-based block index within the scenario (default 0).
    base_mva
        System base MVA for impedance conversions (default 100).

    Returns
    -------
    pp.pandapowerNet
        A pandapower network with buses, generators, loads, and lines
        populated from the gtopt case.
    """
    system = case.get("system", {})

    bus_uid_to_idx = _get_bus_uid_to_idx(system)
    ref_bus_uid = _get_reference_bus_uid(system)

    net = pp.create_empty_network(
        name=system.get("name", "gtopt_case"),
        sn_mva=base_mva,
    )

    # ── Buses ─────────────────────────────────────────────────────────────
    for bus in system.get("bus_array", []):
        bus_uid = int(bus["uid"])
        vn_kv = float(bus.get("voltage", 110.0))
        pp.create_bus(
            net,
            vn_kv=vn_kv,
            name=bus.get("name", f"b{bus_uid}"),
            index=bus_uid_to_idx[bus_uid],
        )

    # ── Generators ────────────────────────────────────────────────────────
    gen_profiles = _build_gen_profile_map(system, scenario, block)

    for gen in system.get("generator_array", []):
        gen_uid = int(gen["uid"])
        bus_uid = int(gen["bus"])
        if bus_uid not in bus_uid_to_idx:
            continue
        bus_idx = bus_uid_to_idx[bus_uid]

        pmax = _resolve_field_sched(gen.get("pmax"), scenario, block)
        if pmax is None:
            pmax = float(gen.get("capacity", 0.0))

        # Apply generator profile if available
        profile_factor = gen_profiles.get(gen_uid, 1.0)
        effective_pmax = pmax * profile_factor

        pmin = _resolve_field_sched(gen.get("pmin"), scenario, block)
        if pmin is None:
            pmin = 0.0

        gcost = float(gen.get("gcost", 0.0))

        is_slack = ref_bus_uid is not None and bus_uid == ref_bus_uid

        if is_slack:
            # ext_grid: the reference generator
            pp.create_ext_grid(
                net,
                bus=bus_idx,
                max_p_mw=effective_pmax,
                min_p_mw=pmin,
                name=gen.get("name", f"g{gen_uid}"),
            )
            # Add cost for ext_grid
            pp.create_poly_cost(
                net,
                element=net.ext_grid.index[-1],
                et="ext_grid",
                cp1_eur_per_mw=gcost,
                cp0_eur=0.0,
                cp2_eur_per_mw2=0.0,
            )
        else:
            gen_idx = pp.create_gen(
                net,
                bus=bus_idx,
                p_mw=effective_pmax * 0.5,  # initial dispatch guess
                max_p_mw=effective_pmax,
                min_p_mw=pmin,
                name=gen.get("name", f"g{gen_uid}"),
                controllable=True,
                slack=False,
            )
            pp.create_poly_cost(
                net,
                element=gen_idx,
                et="gen",
                cp1_eur_per_mw=gcost,
                cp0_eur=0.0,
                cp2_eur_per_mw2=0.0,
            )

    # ── Demands (loads) ───────────────────────────────────────────────────
    demand_profiles = _build_demand_profile_map(system, scenario, block)

    for dem in system.get("demand_array", []):
        dem_uid = int(dem["uid"])
        bus_uid = int(dem["bus"])
        if bus_uid not in bus_uid_to_idx:
            continue
        bus_idx = bus_uid_to_idx[bus_uid]

        lmax = _resolve_field_sched(dem.get("lmax"), scenario, block)
        if lmax is None:
            lmax = 0.0

        # Apply demand profile if available
        profile_factor = demand_profiles.get(dem_uid, 1.0)
        effective_load = lmax * profile_factor

        if effective_load > 0.0:
            pp.create_load(
                net,
                bus=bus_idx,
                p_mw=effective_load,
                name=dem.get("name", f"d{dem_uid}"),
            )

    # ── Lines ─────────────────────────────────────────────────────────────
    for line in system.get("line_array", []):
        bus_a = int(line["bus_a"])
        bus_b = int(line["bus_b"])
        if bus_a not in bus_uid_to_idx or bus_b not in bus_uid_to_idx:
            continue
        idx_a = bus_uid_to_idx[bus_a]
        idx_b = bus_uid_to_idx[bus_b]

        x_pu = float(line.get("reactance", 0.01))
        line_type = line.get("type", "line")

        tmax_ab = float(line.get("tmax_ab", _TMAX_UNLIMITED))
        tmax_ba = float(line.get("tmax_ba", _TMAX_UNLIMITED))
        tmax = min(tmax_ab, tmax_ba)

        if line_type == "transformer":
            # Model as transformer
            sn_mva = tmax if tmax < _TMAX_UNLIMITED else base_mva
            vk_percent = x_pu * 100.0 * (sn_mva / base_mva)

            hv_kv = float(net.bus.at[idx_a, "vn_kv"])
            lv_kv = float(net.bus.at[idx_b, "vn_kv"])

            pp.create_transformer_from_parameters(
                net,
                hv_bus=idx_a,
                lv_bus=idx_b,
                sn_mva=sn_mva,
                vn_hv_kv=hv_kv,
                vn_lv_kv=lv_kv,
                vkr_percent=0.0,
                vk_percent=max(vk_percent, 0.01),
                pfe_kw=0.0,
                i0_percent=0.0,
                name=line.get("name", f"t{bus_a}_{bus_b}"),
            )
        else:
            # Model as standard line
            from_kv = float(net.bus.at[idx_a, "vn_kv"])
            x_ohm = _pu_to_ohm(x_pu, from_kv, base_mva)

            # pandapower needs max_i_ka for thermal limit
            max_i_ka = (
                tmax / (from_kv * math.sqrt(3))
                if tmax < _TMAX_UNLIMITED
                else _TMAX_UNLIMITED
            )

            pp.create_line_from_parameters(
                net,
                from_bus=idx_a,
                to_bus=idx_b,
                length_km=1.0,
                r_ohm_per_km=0.0,
                x_ohm_per_km=x_ohm,
                c_nf_per_km=0.0,
                max_i_ka=max_i_ka,
                name=line.get("name", f"l{bus_a}_{bus_b}"),
            )

    return net


def _build_gen_profile_map(
    system: dict[str, Any],
    scenario_idx: int,
    block_idx: int,
) -> dict[int, float]:
    """Build a generator UID → profile factor map for the given block."""
    profiles: dict[int, float] = {}
    for gp in system.get("generator_profile_array", []):
        gen_uid = int(gp.get("generator", gp.get("uid", 0)))
        profile = gp.get("profile")
        if profile is None:
            continue
        factor = _resolve_field_sched(profile, scenario_idx, block_idx)
        if factor is not None:
            profiles[gen_uid] = factor
    return profiles


def _build_demand_profile_map(
    system: dict[str, Any],
    scenario_idx: int,
    block_idx: int,
) -> dict[int, float]:
    """Build a demand UID → profile factor map for the given block."""
    profiles: dict[int, float] = {}
    for dp in system.get("demand_profile_array", []):
        dem_uid = int(dp.get("demand", dp.get("uid", 0)))
        profile = dp.get("profile")
        if profile is None:
            continue
        factor = _resolve_field_sched(profile, scenario_idx, block_idx)
        if factor is not None:
            profiles[dem_uid] = factor
    return profiles


def convert_all_blocks(
    case: dict[str, Any],
    scenario: int = 0,
    base_mva: float = _BASE_MVA,
) -> list[pp.pandapowerNet]:
    """Convert a gtopt case to one pandapower network per block.

    Parameters
    ----------
    case
        Parsed gtopt JSON.
    scenario
        0-based scenario index.
    base_mva
        System base MVA.

    Returns
    -------
    list[pp.pandapowerNet]
        One network per block in the simulation.
    """
    simulation = case.get("simulation", {})
    blocks = simulation.get("block_array", [{"uid": 1, "duration": 1}])
    return [
        convert(case, scenario=scenario, block=bi, base_mva=base_mva)
        for bi in range(len(blocks))
    ]


def run_dcopp(
    case: dict[str, Any],
    scenario: int = 0,
    block: int = 0,
) -> pp.pandapowerNet:
    """Convert and solve a DC OPF for a single (scenario, block).

    Parameters
    ----------
    case
        Parsed gtopt JSON.
    scenario
        0-based scenario index.
    block
        0-based block index.

    Returns
    -------
    pp.pandapowerNet
        The solved pandapower network (results in ``net.res_*`` tables).

    Raises
    ------
    RuntimeError
        If the DC OPF fails to converge.
    """
    net = convert(case, scenario=scenario, block=block)
    pp.rundcopp(net)
    if not net.OPF_converged:
        raise RuntimeError(
            f"pandapower DC OPF failed to converge (scenario={scenario}, block={block})"
        )
    return net


def get_ac_opf_requirements() -> dict[str, str]:
    """Return a description of additional data needed for AC OPF.

    pandapower's AC OPF (``pp.runopp()``) requires additional electrical
    parameters that are not present in the standard gtopt JSON:

    - **Line resistance** (``r_ohm_per_km``): gtopt only stores reactance.
    - **Shunt capacitance** (``c_nf_per_km``): for charging susceptance.
    - **Reactive power limits** (``min_q_mvar``, ``max_q_mvar``): for
      generators.
    - **Reactive load** (``q_mvar``): for demand entries.
    - **Voltage limits** (``min_vm_pu``, ``max_vm_pu``): per bus.
    - **Transformer parameters** (``vkr_percent``, ``pfe_kw``,
      ``i0_percent``): for real transformer losses.

    These can be included in the gtopt JSON as extension fields in the
    ``ac_opf`` sub-object of each element.  For example::

        {
            "uid": 1, "name": "l1_2",
            "bus_a": 1, "bus_b": 2,
            "reactance": 0.05,
            "ac_opf": {
                "r_pu": 0.01,
                "b_pu": 0.02,
                "rate_a_mva": 100.0
            }
        }

    Returns
    -------
    dict[str, str]
        Mapping from parameter name to description.
    """
    return {
        "line.r_pu": (
            "Line series resistance in per-unit. "
            "Include as ac_opf.r_pu in the line element."
        ),
        "line.b_pu": (
            "Line shunt susceptance in per-unit. "
            "Include as ac_opf.b_pu in the line element."
        ),
        "generator.min_q_mvar": (
            "Minimum reactive power output in Mvar. "
            "Include as ac_opf.min_q_mvar in the generator element."
        ),
        "generator.max_q_mvar": (
            "Maximum reactive power output in Mvar. "
            "Include as ac_opf.max_q_mvar in the generator element."
        ),
        "demand.q_mvar": (
            "Reactive load in Mvar. Include as ac_opf.q_mvar in the demand element."
        ),
        "bus.min_vm_pu": (
            "Minimum bus voltage magnitude in per-unit. "
            "Include as ac_opf.min_vm_pu in the bus element."
        ),
        "bus.max_vm_pu": (
            "Maximum bus voltage magnitude in per-unit. "
            "Include as ac_opf.max_vm_pu in the bus element."
        ),
        "transformer.vkr_percent": (
            "Transformer real short-circuit voltage in percent. "
            "Include as ac_opf.vkr_percent in the transformer line element."
        ),
    }
