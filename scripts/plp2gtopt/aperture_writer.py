"""Aperture writer — convert PLP aperture definitions to gtopt format.

Reads the parsed PLP aperture index files (``plpidape.dat`` /
``plpidap2.dat``) and writes:

1. An ``aperture_array`` in the simulation JSON block — each entry maps an
   aperture UID to a ``source_scenario`` UID with equal probability.
2. Optionally, Parquet files in an ``aperture_directory`` when the aperture
   references a hydrology class that is *not* part of the forward-scenario
   set, requiring separate affluent data.
"""

from typing import Any, Dict, List, Optional

from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq


def _unique_hydro_indices(
    idape_parser: Any,
    idap2_parser: Any,
    num_simulations: int,
    num_stages: int,
) -> set:
    """Collect all unique 1-based hydrology indices referenced by apertures."""
    indices: set = set()

    if idap2_parser is not None:
        for entry in idap2_parser.items:
            indices.update(entry["indices"])

    if idape_parser is not None:
        for entry in idape_parser.items:
            indices.update(entry["indices"])

    return indices


def build_aperture_array(
    idap2_parser: Any,
    scenario_hydro_map: Dict[int, int],
    num_stages: int,
) -> List[Dict[str, Any]]:
    """Build the ``aperture_array`` for the simulation JSON block.

    Uses ``plpidap2.dat`` (simulation-independent) as the primary source
    since it defines a single aperture set shared by all simulations,
    which maps naturally to the gtopt ``aperture_array`` (also global).

    Each unique hydrology index referenced by the apertures is mapped to
    a gtopt scenario UID (either an existing forward scenario or a new
    aperture-only scenario).

    Parameters
    ----------
    idap2_parser
        Parsed ``plpidap2.dat`` data (``IdAp2Parser``), or ``None``.
    scenario_hydro_map : dict
        Maps 0-based hydrology index → gtopt scenario UID.
    num_stages : int
        Total number of PLP stages (1-based).

    Returns
    -------
    list of dict
        List of aperture definitions, each with ``uid``, ``source_scenario``,
        and ``probability_factor``.
    """
    if idap2_parser is None:
        return []

    # Use the aperture set from stage 1 as the canonical definition.
    # (In most PLP cases the aperture set is the same for all stages.)
    stage1 = idap2_parser.get_apertures(1)
    if not stage1:
        # Try first available stage
        if idap2_parser.items:
            stage1 = idap2_parser.items[0]["indices"]
        else:
            return []

    # De-duplicate while preserving order
    seen: set = set()
    unique_hydros: list = []
    for h in stage1:
        if h not in seen:
            seen.add(h)
            unique_hydros.append(h)

    num_apertures = len(unique_hydros)
    if num_apertures == 0:
        return []

    prob = 1.0 / num_apertures

    aperture_array: List[Dict[str, Any]] = []
    for ap_idx, hydro_1based in enumerate(unique_hydros):
        hydro_0based = hydro_1based - 1
        # Look up the gtopt scenario UID for this hydrology
        scenario_uid = scenario_hydro_map.get(hydro_0based)
        if scenario_uid is None:
            # This hydrology isn't in the forward set → use the hydro index
            # as the scenario UID (will be served from aperture_directory)
            scenario_uid = hydro_1based

        aperture_array.append(
            {
                "uid": ap_idx + 1,
                "source_scenario": scenario_uid,
                "probability_factor": prob,
            }
        )

    return aperture_array


def write_aperture_afluents(
    aflce_parser: Any,
    central_parser: Any,
    block_parser: Any,
    aperture_hydros: List[int],
    forward_hydros: set,
    output_dir: Path,
    options: Optional[Dict[str, Any]] = None,
) -> None:
    """Write affluent Parquet files for aperture-only hydrology classes.

    Only hydrologies that are NOT in the forward-scenario set need separate
    files in the aperture directory.  The output format matches the standard
    gtopt ``Afluent/<central_name>.parquet`` layout so the solver can load
    them using the same reader.

    Parameters
    ----------
    aflce_parser
        Parsed ``plpaflce.dat`` data.
    central_parser
        Parsed ``plpcnfce.dat`` data.
    block_parser
        Parsed ``plpblo.dat`` data.
    aperture_hydros : list of int
        0-based hydrology indices used by apertures.
    forward_hydros : set of int
        0-based hydrology indices already in the forward-scenario set.
    output_dir : Path
        Base directory for aperture data files (the ``aperture_directory``).
    options : dict, optional
        Conversion options.
    """
    # Determine which hydros need separate files
    extra_hydros = sorted(set(aperture_hydros) - forward_hydros)
    if not extra_hydros:
        return

    afluent_dir = output_dir / "Afluent"
    afluent_dir.mkdir(parents=True, exist_ok=True)

    if aflce_parser is None or central_parser is None or block_parser is None:
        return

    blocks = block_parser.items

    for central in aflce_parser.items:
        central_name = central["name"]
        flows = central.get("flows", [])
        if not flows:
            continue

        # Build columns for the extra hydrology scenarios
        # Each hydrology becomes a scenario column: "uid:<scenario_uid>"
        scenario_col = []
        stage_col = []
        block_col = []
        value_cols: Dict[int, List[float]] = {}

        for hydro_0based in extra_hydros:
            # The scenario UID for aperture hydros = hydro_1based
            scenario_uid = hydro_0based + 1
            value_cols[scenario_uid] = []

        for block_idx, block in enumerate(blocks):
            stage_num = block.get("stage", 1)
            block_num = block_idx + 1

            for hydro_0based in extra_hydros:
                scenario_uid = hydro_0based + 1
                # Get flow value for this (block, hydrology)
                if block_idx < len(flows) and hydro_0based < len(flows[block_idx]):
                    value = flows[block_idx][hydro_0based]
                else:
                    value = 0.0
                value_cols[scenario_uid].append(value)

            scenario_col.append(0)  # placeholder; actual scenario in col name
            stage_col.append(stage_num)
            block_col.append(block_num)

        # Write one Parquet file per central, with scenario columns
        if not value_cols:
            continue

        arrays = {
            "stage": pa.array(stage_col, type=pa.int32()),
            "block": pa.array(block_col, type=pa.int32()),
        }
        for scen_uid, values in sorted(value_cols.items()):
            arrays[f"uid:{scen_uid}"] = pa.array(values, type=pa.float64())

        table = pa.table(arrays)
        out_path = afluent_dir / f"{central_name}.parquet"
        pq.write_table(table, out_path, compression="gzip")
