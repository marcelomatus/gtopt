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

Scenario and block parameters always use **UIDs** (as they appear in
the JSON ``scenario_array`` / ``block_array``), never 0-based C++
indices.

Usage::

    from gtopt2pp.convert import convert, load_gtopt_case

    # Load a gtopt JSON case and get a pandapower net for block UID 1
    case = load_gtopt_case("ieee_9b.json")
    net = convert(case, scenario=1, block=1)

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

    The returned dict includes a ``_case_dir`` key with the directory
    containing the JSON file, used for resolving FieldSched file
    references.

    Parameters
    ----------
    path
        Path to the ``.json`` file.

    Returns
    -------
    dict
        The parsed JSON as a Python dictionary (with ``_case_dir``).

    Raises
    ------
    FileNotFoundError
        If *path* does not exist.
    """
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"gtopt case not found: {p}")
    with open(p, encoding="utf-8") as fh:
        case = json.load(fh)
    case["_case_dir"] = str(p.parent.resolve())
    return case


def _build_bus_ref_map(
    system: dict[str, Any],
) -> dict[Any, int]:
    """Build a mapping from bus UID or name → 0-based pandapower bus index.

    gtopt JSON allows bus references by integer UID or by name string.
    This map resolves both to the pandapower index.
    """
    ref_map: dict[Any, int] = {}
    for i, bus in enumerate(system.get("bus_array", [])):
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid is not None:
            ref_map[uid] = i
        if name:
            ref_map[name] = i
    return ref_map


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


def _uid_to_index(
    uid: int,
    array: list[dict[str, Any]],
    label: str,
) -> int:
    """Convert a UID to a 0-based array index.

    Raises
    ------
    ValueError
        If *uid* is not found in *array*.
    """
    for i, elem in enumerate(array):
        if elem.get("uid") == uid:
            return i
    raise ValueError(
        f"{label} UID {uid} not found; available UIDs: {[e.get('uid') for e in array]}"
    )


def _first_uid(
    array: list[dict[str, Any]],
    label: str,
) -> int:
    """Return the UID of the first element in *array*.

    Raises
    ------
    ValueError
        If *array* is empty.
    """
    if not array:
        raise ValueError(f"No {label} defined in the simulation")
    uid = array[0].get("uid")
    if uid is None:
        raise ValueError(f"First {label} has no uid field")
    return int(uid)


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


def _resolve_field_sched_with_file(
    field: Any,
    scenario_idx: int,
    block_idx: int,
    case_dir: str | None,
    input_dir: str,
    class_name: str,
    element_uid: int | None,
    element_name: str | None,
    scenario_uid: int | None,
    block_uid: int | None,
    preferred_format: str = "parquet",
) -> float | None:
    """Resolve a FieldSched value, reading from file if needed.

    For scalar and list types, delegates to :func:`_resolve_field_sched`.
    For string (FileSched) references, reads the external Parquet/CSV
    file using the gtopt input file conventions.
    """
    if field is None:
        return None
    if isinstance(field, (int, float)):
        return float(field)
    if isinstance(field, list):
        return _resolve_field_sched(field, scenario_idx, block_idx)
    if isinstance(field, str) and case_dir is not None:
        return _read_file_sched_value(
            case_dir,
            input_dir,
            class_name,
            field,
            element_uid,
            element_name,
            scenario_uid,
            block_uid,
            preferred_format,
        )
    return None


def _read_file_sched_value(
    case_dir: str,
    input_dir: str,
    class_name: str,
    field_name: str,
    element_uid: int | None,
    element_name: str | None,
    scenario_uid: int | None,
    block_uid: int | None,
    preferred_format: str,
) -> float | None:
    """Read a single value from a FieldSched Parquet/CSV file."""
    import pandas as pd  # pylint: disable=import-outside-toplevel

    # Build table path (mirrors C++ build_table_path)
    if "@" in field_name:
        parts = field_name.split("@", 1)
        base = str(Path(case_dir) / input_dir / parts[0] / parts[1])
    else:
        base = str(Path(case_dir) / input_dir / class_name / field_name)

    # Try to load with format fallback
    extensions = (
        [".parquet", ".parquet.gz", ".csv", ".csv.gz"]
        if preferred_format == "parquet"
        else [".csv", ".csv.gz", ".parquet", ".parquet.gz"]
    )

    df = None
    for ext in extensions:
        fpath = Path(base + ext)
        if not fpath.exists():
            continue
        try:
            if ".parquet" in ext:
                df = pd.read_parquet(fpath)
            else:
                df = pd.read_csv(fpath)
            break
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            continue

    if df is None:
        return None

    # Find the element column (uid:<N>, name, or bare UID)
    col = None
    if element_uid is not None:
        uid_col = f"uid:{element_uid}"
        if uid_col in df.columns:
            col = uid_col
    if col is None and element_name and element_name in df.columns:
        col = element_name
    if col is None and element_uid is not None:
        if str(element_uid) in df.columns:
            col = str(element_uid)
    if col is None:
        return None

    # Filter by scenario/block UIDs
    filtered = df
    if scenario_uid is not None and "scenario" in filtered.columns:
        filtered = filtered[filtered["scenario"] == scenario_uid]
    if block_uid is not None and "block" in filtered.columns:
        filtered = filtered[filtered["block"] == block_uid]

    if filtered.empty:
        if not df.empty:
            return float(df[col].iloc[0])
        return None

    return float(filtered[col].iloc[0])


def _pu_to_ohm(x_pu: float, base_kv: float, base_mva: float = 100.0) -> float:
    """Convert per-unit reactance to Ohm."""
    z_base = base_kv**2 / base_mva
    return x_pu * z_base


def _resolve_bus_ref(
    ref: Any,
    bus_ref_map: dict[Any, int],
) -> int | None:
    """Resolve a bus reference (int UID or str name) to a pp bus index."""
    if ref is None:
        return None
    if ref in bus_ref_map:
        return bus_ref_map[ref]
    # Try integer conversion for string-encoded UIDs
    if isinstance(ref, str):
        try:
            int_ref = int(ref)
            if int_ref in bus_ref_map:
                return bus_ref_map[int_ref]
        except ValueError:
            pass
    return None


def convert(
    case: dict[str, Any],
    scenario: int | None = None,
    block: int | None = None,
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
        Scenario **UID** (as in the JSON ``scenario_array``).
        ``None`` defaults to the first scenario's UID.
    block
        Block **UID** (as in the JSON ``block_array``).
        ``None`` defaults to the first block's UID.
    base_mva
        System base MVA for impedance conversions (default 100).

    Returns
    -------
    pp.pandapowerNet
        A pandapower network with buses, generators, loads, and lines
        populated from the gtopt case.
    """
    system = case.get("system", {})
    simulation = case.get("simulation", {})
    options = case.get("options", {})

    # File-reading context
    case_dir = case.get("_case_dir")
    input_dir = options.get("input_directory", "input")
    preferred_fmt = options.get("input_format", "parquet")

    scenarios = simulation.get("scenario_array", [{"uid": 1}])
    blocks = simulation.get("block_array", [{"uid": 1, "duration": 1}])

    # Resolve UID → 0-based index for FieldSched lookup
    if scenario is None:
        scenario = _first_uid(scenarios, "scenario")
    scenario_idx = _uid_to_index(scenario, scenarios, "scenario")

    if block is None:
        block = _first_uid(blocks, "block")
    block_idx = _uid_to_index(block, blocks, "block")

    # Shorthand for resolving a FieldSched with file fallback
    def _rfs(
        field: Any,
        cls: str,
        elem_uid: int | None = None,
        elem_name: str | None = None,
    ) -> float | None:
        return _resolve_field_sched_with_file(
            field,
            scenario_idx,
            block_idx,
            case_dir,
            input_dir,
            cls,
            elem_uid,
            elem_name,
            scenario,
            block,
            preferred_fmt,
        )

    bus_ref_map = _build_bus_ref_map(system)
    ref_bus_uid = _get_reference_bus_uid(system)

    net = pp.create_empty_network(
        name=system.get("name", "gtopt_case"),
        sn_mva=base_mva,
    )

    # ── Buses ─────────────────────────────────────────────────────────────
    for i, bus in enumerate(system.get("bus_array", [])):
        bus_uid = bus.get("uid")
        vn_kv = float(bus.get("voltage", 110.0))
        pp.create_bus(
            net,
            vn_kv=vn_kv,
            name=bus.get("name", f"b{bus_uid}"),
            index=i,
        )

    # ── Generators ────────────────────────────────────────────────────────
    gen_profiles = _build_gen_profile_map(system, scenario_idx, block_idx)

    # Determine the reference bus index.  When no bus carries
    # ``reference_theta``, fall back to the bus of the first generator
    # (matching the DC-OPF convention used by PLP and gtopt itself, where
    # bus 1 is the implicit slack).  If there are no generators at all,
    # fall back to the first bus in the network (index 0).
    if ref_bus_uid is None:
        generator_array = system.get("generator_array", [])
        if generator_array:
            first_gen_bus_ref = generator_array[0].get("bus")
            fallback_bus_idx = _resolve_bus_ref(first_gen_bus_ref, bus_ref_map)
        else:
            fallback_bus_idx = 0 if net.bus is not None and len(net.bus) else None
    else:
        fallback_bus_idx = None  # ref_bus_uid drives the slack selection below

    for gen in system.get("generator_array", []):
        gen_uid = int(gen["uid"])
        gen_name = gen.get("name", "")
        bus_ref = gen.get("bus")
        bus_idx = _resolve_bus_ref(bus_ref, bus_ref_map)
        if bus_idx is None:
            continue

        pmax = _rfs(gen.get("pmax"), "Generator", gen_uid, gen_name)
        if pmax is None:
            cap_val = _rfs(
                gen.get("capacity"),
                "Generator",
                gen_uid,
                gen_name,
            )
            pmax = cap_val if cap_val is not None else 0.0

        # Apply generator profile if available
        profile_factor = gen_profiles.get(gen_uid, 1.0)
        effective_pmax = pmax * profile_factor

        pmin = _rfs(gen.get("pmin"), "Generator", gen_uid, gen_name)
        if pmin is None:
            pmin = 0.0

        gcost_val = _rfs(gen.get("gcost"), "Generator", gen_uid, gen_name)
        gcost = gcost_val if gcost_val is not None else 0.0

        # Decide whether this generator becomes the ext_grid (slack).
        # Priority: explicit reference_theta bus → first-generator fallback.
        if ref_bus_uid is not None:
            is_slack = _resolve_bus_ref(bus_ref, bus_ref_map) == _resolve_bus_ref(
                ref_bus_uid, bus_ref_map
            )
        else:
            is_slack = bus_idx == fallback_bus_idx
            if is_slack:
                fallback_bus_idx = None  # consume the fallback slot

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

    # If no ext_grid was created at all (no generators in the case or all
    # generators were skipped due to unresolvable bus references), add a
    # zero-cost ext_grid on the first available bus so that pandapower
    # diagnostics and power-flow tools have a valid reference bus.
    if len(net.ext_grid) == 0 and len(net.bus) > 0:
        pp.create_ext_grid(net, bus=0, name="slack")

    # ── Demands (loads) ───────────────────────────────────────────────────
    demand_profiles = _build_demand_profile_map(system, scenario_idx, block_idx)

    for dem in system.get("demand_array", []):
        dem_uid = int(dem["uid"])
        dem_name = dem.get("name", "")
        bus_ref = dem.get("bus")
        bus_idx = _resolve_bus_ref(bus_ref, bus_ref_map)
        if bus_idx is None:
            continue

        lmax = _rfs(dem.get("lmax"), "Demand", dem_uid, dem_name)
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
        line_uid = line.get("uid")
        line_name = line.get("name", "")
        bus_a_ref = line.get("bus_a")
        bus_b_ref = line.get("bus_b")
        idx_a = _resolve_bus_ref(bus_a_ref, bus_ref_map)
        idx_b = _resolve_bus_ref(bus_b_ref, bus_ref_map)
        if idx_a is None or idx_b is None:
            continue

        x_pu_val = _rfs(
            line.get("reactance"),
            "Line",
            line_uid,
            line_name,
        )
        x_pu = x_pu_val if x_pu_val is not None else 0.01

        line_type = line.get("type", "line")

        tmax_ab_val = _rfs(
            line.get("tmax_ab"),
            "Line",
            line_uid,
            line_name,
        )
        tmax_ab = tmax_ab_val if tmax_ab_val is not None else _TMAX_UNLIMITED

        tmax_ba_val = _rfs(
            line.get("tmax_ba"),
            "Line",
            line_uid,
            line_name,
        )
        tmax_ba = tmax_ba_val if tmax_ba_val is not None else _TMAX_UNLIMITED

        tmax = min(tmax_ab, tmax_ba)

        display_name = line_name or f"l{bus_a_ref}_{bus_b_ref}"

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
                name=display_name,
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
                name=display_name,
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
    scenario: int | None = None,
    base_mva: float = _BASE_MVA,
) -> list[pp.pandapowerNet]:
    """Convert a gtopt case to one pandapower network per block.

    Parameters
    ----------
    case
        Parsed gtopt JSON.
    scenario
        Scenario **UID**.  ``None`` defaults to the first scenario.
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
        convert(
            case,
            scenario=scenario,
            block=int(b["uid"]),
            base_mva=base_mva,
        )
        for b in blocks
    ]


def run_dcopp(
    case: dict[str, Any],
    scenario: int | None = None,
    block: int | None = None,
) -> pp.pandapowerNet:
    """Convert and solve a DC OPF for a single (scenario, block).

    Parameters
    ----------
    case
        Parsed gtopt JSON.
    scenario
        Scenario **UID**.  ``None`` defaults to the first scenario.
    block
        Block **UID**.  ``None`` defaults to the first block.

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
