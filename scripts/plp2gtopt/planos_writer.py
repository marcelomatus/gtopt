"""Writer for gtopt boundary-cuts CSV from parsed PLP planos data.

Converts the output of :class:`PlanosParser` into the gtopt boundary-cuts
CSV format understood by the SDDP solver's ``load_boundary_cuts()`` method.

The CSV format is::

    name,iteration,scene,rhs,Reservoir1,Reservoir2,...
    bc_1_1,1,1,-5000.0,0.25,0.75,...
    bc_1_2,1,2,-4800.0,0.30,0.60,...

Column headers after ``rhs`` are the state-variable names (reservoirs or
junctions) that the solver maps to LP columns.  The ``scene`` column
contains the **scene UID** (matching the ``uid`` field in gtopt's
``scene_array``).  For PLP cases, ISimul maps directly to the scene UID
because plp2gtopt assigns ``uid = i + 1`` for scene *i*.  Coefficients
represent the gradient of the future-cost function with respect to each
state variable (reservoir volume).
"""

import csv
import logging
from pathlib import Path
from typing import Any, Dict, List

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
