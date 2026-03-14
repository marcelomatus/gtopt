"""Aperture writer — convert PLP aperture definitions to gtopt format.

Reads the parsed PLP aperture index files (``plpidape.dat`` /
``plpidap2.dat``) and writes:

1. An ``aperture_array`` in the simulation JSON block — each entry maps an
   aperture UID to a ``source_scenario`` UID with equal probability.
2. Optionally, Parquet files in an ``aperture_directory`` when the aperture
   references a hydrology class that is *not* part of the forward-scenario
   set, requiring separate affluent data.
"""

import logging
from typing import Any, Dict, List, Optional

from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

_LOG = logging.getLogger(__name__)


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

    Uses ``plpidap2.dat`` (simulation-independent) as the primary source.
    ``plpidap2.dat`` is **stage-indexed**: each stage entry lists the 1-based
    hydrology class indices that serve as apertures at that stage.  The global
    aperture set is the union of all stages 1..``num_stages``, collected in
    first-appearance order so that late-stage wrapping indices (e.g. hydros
    ``01``, ``02`` at the end of a two-year case) are included.

    Each unique hydrology index is mapped to a gtopt scenario UID: an existing
    forward-scenario UID when the hydrology is in the forward set, or the
    1-based hydrology index as a new aperture-only scenario UID otherwise.

    Parameters
    ----------
    idap2_parser
        Parsed ``plpidap2.dat`` data (``IdAp2Parser``), or ``None``.
    scenario_hydro_map : dict
        Maps 0-based hydrology index → gtopt scenario UID.
    num_stages : int
        Number of output stages (only stages 1..num_stages are considered).

    Returns
    -------
    list of dict
        List of aperture definitions, each with ``uid``, ``source_scenario``,
        and ``probability_factor``.
    """
    if idap2_parser is None:
        return []

    # Collect the union of all used stages' aperture indices preserving
    # first-appearance order.  plpidap2.dat is stage-indexed — we must
    # iterate over all output stages, not only stage 1.
    seen: set = set()
    unique_hydros: list = []
    for entry in idap2_parser.items:
        if 1 <= entry["stage"] <= num_stages:
            for h in entry["indices"]:
                if h not in seen:
                    seen.add(h)
                    unique_hydros.append(h)

    # Fallback: if num_stages filter yielded nothing, use the first entry
    if not unique_hydros and idap2_parser.items:
        for h in idap2_parser.items[0]["indices"]:
            if h not in seen:
                seen.add(h)
                unique_hydros.append(h)

    if not unique_hydros:
        return []

    num_apertures = len(unique_hydros)
    prob = 1.0 / num_apertures

    aperture_array: List[Dict[str, Any]] = []
    for ap_idx, hydro_1based in enumerate(unique_hydros):
        hydro_0based = hydro_1based - 1
        # Look up the gtopt scenario UID for this hydrology
        scenario_uid = scenario_hydro_map.get(hydro_0based)
        if scenario_uid is None:
            # Not in the forward set → use 1-based hydro index as scenario UID
            # (served from the aperture_directory)
            scenario_uid = hydro_1based

        aperture_array.append(
            {
                "uid": ap_idx + 1,
                "source_scenario": scenario_uid,
                "probability_factor": prob,
            }
        )

    return aperture_array


def build_phase_aperture_sets(
    idap2_parser: Any,
    aperture_array: List[Dict[str, Any]],
    phase_array: List[Dict[str, Any]],
    num_stages: int,
) -> None:
    """Populate ``aperture_set`` on each phase from per-stage PLP aperture data.

    PLP's ``plpidap2.dat`` stores aperture indices **per stage**.  Since each
    gtopt phase maps to one or more PLP stages, this function computes the
    union of all aperture hydrology indices across the stages in each phase
    and maps them to the corresponding aperture UIDs from ``aperture_array``.

    The function modifies ``phase_array`` **in place**, adding an
    ``"aperture_set"`` key to each phase dict.  If all phases share the same
    aperture set (the common case for single-year problems), ``aperture_set``
    is left empty (meaning "use all apertures") to keep the JSON compact.

    Parameters
    ----------
    idap2_parser
        Parsed ``plpidap2.dat`` data, or ``None``.
    aperture_array : list of dict
        The global aperture definitions (output of :func:`build_aperture_array`).
    phase_array : list of dict
        The phase definitions to update in place.
    num_stages : int
        Number of output stages.
    """
    if idap2_parser is None or not aperture_array or not phase_array:
        return

    # Build a mapping: 1-based hydrology index → aperture UID
    # We need to reconstruct which hydro each aperture references.
    # build_aperture_array assigns UIDs sequentially from unique_hydros,
    # so we rebuild the same unique_hydros list to create the reverse map.
    seen: set = set()
    unique_hydros: list = []
    for entry in idap2_parser.items:
        if 1 <= entry["stage"] <= num_stages:
            for h in entry["indices"]:
                if h not in seen:
                    seen.add(h)
                    unique_hydros.append(h)
    if not unique_hydros and idap2_parser.items:
        for h in idap2_parser.items[0]["indices"]:
            if h not in seen:
                seen.add(h)
                unique_hydros.append(h)

    hydro_to_aperture_uid: Dict[int, int] = {}
    for ap_idx, hydro_1based in enumerate(unique_hydros):
        hydro_to_aperture_uid[hydro_1based] = ap_idx + 1

    # For each phase, collect the aperture hydros across its stages.
    # Duplicates are preserved: if the same hydro index appears in multiple
    # stages of a phase, the corresponding aperture UID is listed once per
    # occurrence.  The C++ solver then solves each unique aperture LP once
    # and scales its weight by the repetition count N.
    # (PLP stages are 1-based; phase["first_stage"] is 0-based).
    phase_sets: List[List[int]] = []
    for phase in phase_array:
        first_stage_0 = phase["first_stage"]
        count = phase["count_stage"]
        phase_hydros: list = []
        for stage_0 in range(first_stage_0, first_stage_0 + count):
            plp_stage = stage_0 + 1  # convert to 1-based PLP stage
            if plp_stage > num_stages:
                break
            aps = idap2_parser.get_apertures(plp_stage)
            if aps:
                phase_hydros.extend(aps)
        # Map hydro indices to aperture UIDs (preserving duplicates)
        ap_uids = sorted(
            hydro_to_aperture_uid[h] for h in phase_hydros if h in hydro_to_aperture_uid
        )
        phase_sets.append(ap_uids)

    # Only add aperture_set if phases differ; otherwise leave empty.
    # For the uniform check, compare the sorted *unique* sets — a phase
    # that spans two stages produces duplicates but still uses the same
    # aperture definitions as a single-stage phase.
    all_ap_uids = sorted(hydro_to_aperture_uid.values())
    all_same = all(sorted(set(s)) == all_ap_uids for s in phase_sets)

    if not all_same:
        for phase, ap_set in zip(phase_array, phase_sets):
            phase["aperture_set"] = ap_set


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

    # block_parser provides the ordered list of blocks (block-indexed info).
    # plpidap2/plpidape are stage-indexed; we use them only to determine
    # *which* hydrology indices are extra — not to drive the block iteration.
    blocks = block_parser.items

    for central_data in aflce_parser.items:
        central_name = central_data["name"]
        # plpaflce.dat stores flow data as a 2-D numpy array:
        # shape = (num_central_blocks, num_hydrologies).
        # Key is "flow" (not "flows").
        flow_matrix = central_data.get("flow")
        if flow_matrix is None or len(flow_matrix) == 0:
            continue

        # The afflce block list may not start at 1; build a mapping so we
        # can look up the correct row by block number.
        central_block_nums = central_data.get("block")  # numpy array of block numbers
        block_num_to_row: Dict[int, int] = {}
        if central_block_nums is not None:
            for row_idx, blk_num in enumerate(central_block_nums):
                block_num_to_row[int(blk_num)] = row_idx

        num_hydro_cols: int = flow_matrix.shape[1] if len(flow_matrix.shape) > 1 else 0

        # One scenario column per extra hydrology
        stage_col: List[int] = []
        block_col: List[int] = []
        value_cols: Dict[int, List[float]] = {}
        for hydro_0based in extra_hydros:
            # The scenario UID for aperture-only hydros = 1-based hydro index
            scenario_uid = hydro_0based + 1
            value_cols[scenario_uid] = []

        for block in blocks:
            block_num = block["number"]
            stage_num = block.get("stage", 1)
            mat_row: Optional[int] = block_num_to_row.get(block_num)

            for hydro_0based in extra_hydros:
                scenario_uid = hydro_0based + 1
                if mat_row is not None and hydro_0based < num_hydro_cols:
                    value = float(flow_matrix[mat_row, hydro_0based])
                else:
                    if mat_row is None:
                        _LOG.debug(
                            "Block %d not found in afluent data for central %s; using 0.0",
                            block_num,
                            central_name,
                        )
                    value = 0.0
                value_cols[scenario_uid].append(value)

            stage_col.append(stage_num)
            block_col.append(block_num)

        if not value_cols:
            continue

        arrays: Dict[str, Any] = {
            "stage": pa.array(stage_col, type=pa.int32()),
            "block": pa.array(block_col, type=pa.int32()),
        }
        for scen_uid, values in sorted(value_cols.items()):
            arrays[f"uid:{scen_uid}"] = pa.array(values, type=pa.float64())

        table = pa.table(arrays)
        out_path = afluent_dir / f"{central_name}.parquet"
        pq.write_table(table, out_path, compression="gzip")
