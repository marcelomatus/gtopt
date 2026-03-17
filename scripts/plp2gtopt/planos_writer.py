"""Writer for gtopt boundary-cuts and hot-start-cuts CSV files.

Converts the output of :class:`PlanosParser` into the CSV formats understood
by the SDDP solver:

**Boundary-cuts CSV** (``load_boundary_cuts()``)::

    name,iteration,scene,rhs,Reservoir1,Reservoir2,...
    bc_1_1,1,1,-5000.0,0.25,0.75,...

**Hot-start-cuts CSV** (``load_named_cuts()``)::

    name,iteration,scene,phase,rhs,Reservoir1,Reservoir2,...
    hs_1_1_3,1,1,3,-5000.0,0.25,0.75,...

Column headers after ``rhs`` are the state-variable names (reservoirs or
junctions) that the solver maps to LP columns.  The ``scene`` column
contains the **scene UID** (matching the ``uid`` field in gtopt's
``scene_array``).  The ``phase`` column is the **phase UID** and is only
present in hot-start-cuts files.
"""

import csv
import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)


def write_boundary_cuts_csv(
    cuts: List[Dict[str, Any]],
    reservoir_names: List[str],
    output_path: Path | str,
) -> Path:
    """Write boundary cuts to a CSV file in gtopt format.

    Parameters
    ----------
    cuts
        List of cut dicts, each with keys ``name``, ``iteration``, ``scene``,
        ``rhs``, and ``coefficients`` (a dict mapping reservoir names to
        floats).  The ``scene`` value is the scene UID.
    reservoir_names
        Ordered list of reservoir/junction names for column headers.
    output_path
        Path for the output CSV file.

    Returns
    -------
    Path
        The path to the written CSV file.
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    header = ["name", "iteration", "scene", "rhs"] + list(reservoir_names)

    with open(output_path, "w", newline="", encoding="utf-8") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)

        for cut in cuts:
            row = [
                cut["name"],
                cut.get("iteration", 0),
                cut["scene"],
                f"{cut['rhs']:.10g}",
            ]
            coeffs = cut.get("coefficients", {})
            for rname in reservoir_names:
                row.append(f"{coeffs.get(rname, 0.0):.10g}")
            writer.writerow(row)

    logger.info(
        "Wrote %d boundary cuts to %s (%d state variables)",
        len(cuts),
        output_path,
        len(reservoir_names),
    )
    return output_path


def write_hot_start_cuts_csv(
    cuts: List[Dict[str, Any]],
    reservoir_names: List[str],
    output_path: Path | str,
    stage_to_phase: Optional[Dict[int, int]] = None,
) -> Path:
    """Write hot-start cuts (all stages) to a CSV with named state variables.

    Unlike :func:`write_boundary_cuts_csv`, this includes a ``phase`` column
    so the solver can load each cut into the correct phase.

    Parameters
    ----------
    cuts
        List of cut dicts, each with keys ``name``, ``iteration``, ``scene``,
        ``stage`` (1-based PLP IEtapa), ``rhs``, and ``coefficients``.
    reservoir_names
        Ordered list of state-variable names for column headers.
    output_path
        Path for the output CSV file.
    stage_to_phase
        Mapping from PLP stage number (1-based) to gtopt phase UID.
        If *None*, a default mapping ``stage → stage`` is used.

    Returns
    -------
    Path
        The path to the written CSV file.
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    header = ["name", "iteration", "scene", "phase", "rhs"] + list(
        reservoir_names
    )

    with open(output_path, "w", newline="", encoding="utf-8") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)

        for cut in cuts:
            plp_stage = cut.get("stage", 0)
            if stage_to_phase is not None:
                phase_uid = stage_to_phase.get(plp_stage, plp_stage)
            else:
                phase_uid = plp_stage

            row = [
                cut["name"],
                cut.get("iteration", 0),
                cut["scene"],
                phase_uid,
                f"{cut['rhs']:.10g}",
            ]
            coeffs = cut.get("coefficients", {})
            for rname in reservoir_names:
                row.append(f"{coeffs.get(rname, 0.0):.10g}")
            writer.writerow(row)

    logger.info(
        "Wrote %d hot-start cuts to %s (%d state variables)",
        len(cuts),
        output_path,
        len(reservoir_names),
    )
    return output_path
