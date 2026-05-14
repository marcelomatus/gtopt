"""Writer for gtopt boundary-cuts CSV files.

Converts the output of :class:`PlanosParser` into the CSV format understood
by the SDDP solver's boundary-cut loader:

**Boundary-cuts CSV** (``load_boundary_cuts()``)::

    iteration,scene,rhs,Reservoir1,Reservoir2,...
    1,1,-5000.0,0.25,0.75,...

Column headers after ``rhs`` are the state-variable names (reservoirs or
junctions) that the solver maps to LP columns.  The ``scene`` column
contains the **scene UID** (matching the ``uid`` field in gtopt's
``scene_array``).

The legacy leading ``name`` column was retired in 2026-05 (PLP itself
never emitted one); cut identity now lives in the structured
``(iteration, scene, rhs)`` tuple and log diagnostics format from those
fields directly.

The legacy ``write_hot_start_cuts_csv`` ("hot-start planos") path was
also retired in 2026-05 — those cuts are gtopt's own format and now
travel via the typed Parquet writer / loader (``cuts_input_file``).

Probability-factor scaling (NVarPhi)
------------------------------------

PLP and gtopt put the scenario probability in *different* places inside the
LP:

* **PLP** solves ONE LP containing all scenarios at once.  The α-column
  (called ``varphi``) carries an LP objective coefficient
  ``(ScalePhi/ScaleObj) / NVarPhi`` — i.e. the ``1/NVarPhi`` factor IS the
  per-scenario probability weight.  The cut RHS ``LDPhiPrv`` is the **total**
  expected future cost (raw ``$``, NOT divided by ``NVarPhi``) and the
  gradient ``GradX_i`` is the total marginal water value across scenarios.

* **gtopt** solves ONE LP **per scene**.  The α-column there has objective
  coefficient ``1.0`` (raw ``$``).  Expected cost across scenes =
  ``Σ_s α_s``, so each per-scene LP must contribute its OWN per-scene share
  of the future cost.

If we wrote ``LDPhiPrv`` and ``GradX_i`` verbatim, every per-scene LP would
load the *total* expected future cost as its α floor — inflating the LB by
``NVarPhi×`` (observed 16× on juan/gtopt_iplp_plain, 2026-05-13).

**Fix**: at write time, divide both ``rhs`` and every gradient coefficient
by ``num_scenarios``.  Pass ``num_scenarios`` = ``len(scenario_array)`` from
the caller.  When ``num_scenarios`` is ``None`` or ``1`` no scaling is
applied (back-compat / single-scenario cases).

**Equal-probability assumption**: PLP's ``1/NVarPhi`` weighting assumes equal
scenario probabilities.  ``plp2gtopt`` builds ``scenario_array`` with
``probability_factor = 1/NVarPhi`` by default (see
``_writer_time.py::process_scenarios``), so this matches PLP's convention.
If a future caller overrides ``--probability-factors`` to unequal values,
this fix becomes approximate — flagged as a follow-up.
"""

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

import pyarrow as pa
import pyarrow.csv as pacsv

logger = logging.getLogger(__name__)


def _apply_alias(name: str, alias: Optional[Dict[str, str]]) -> str:
    """Return ``alias[name]`` if ``alias`` is set and contains ``name``."""
    if alias is None:
        return name
    return alias.get(name, name)


def _scale_factor(num_scenarios: Optional[int]) -> float:
    """Return ``1/num_scenarios`` (``NVarPhi`` factor) or ``1.0`` if N/A.

    See the module docstring for the rationale (PLP α-column carries
    ``1/NVarPhi`` as its probability weight; gtopt's α-column carries
    ``1.0``, so the per-scene contribution must be pre-divided).

    ``num_scenarios`` of ``None``, ``0``, or ``1`` disables scaling.
    """
    if num_scenarios is None or num_scenarios <= 1:
        return 1.0
    return 1.0 / float(num_scenarios)


def write_boundary_cuts_csv(
    cuts: List[Dict[str, Any]],
    reservoir_names: List[str],
    output_path: Path | str,
    name_alias: Optional[Dict[str, str]] = None,
    num_scenarios: Optional[int] = None,
    fescala_map: Optional[Dict[str, int]] = None,
) -> Path:
    """Write boundary cuts to a CSV file in gtopt format.

    Parameters
    ----------
    cuts
        List of cut dicts, each with keys ``iteration``, ``scene``, ``rhs``,
        and ``coefficients`` (a dict mapping reservoir names to floats).
        The ``scene`` value is the scene UID.  A legacy ``name`` key, if
        present on the input dicts, is ignored (no longer emitted to disk).
    reservoir_names
        Ordered list of reservoir/junction names for column headers.
    output_path
        Path for the output CSV file.
    name_alias
        Optional ``{plp_name: gtopt_name}`` map applied to the header row so
        the solver can resolve state variables by the gtopt-side name.
        Cut coefficients remain keyed by the original PLP names; missing
        keys pass through unchanged.
    num_scenarios
        Number of PLP scenarios used to build the cuts (``NVarPhi``).
        When ``>= 2``, both the ``rhs`` and every gradient coefficient are
        divided by this count so the cut sits in gtopt's per-scene α-space
        instead of PLP's shared-α-column space.  See the module docstring.
        Pass ``len(scenario_array)`` from the caller.  ``None`` or ``1``
        disables scaling (back-compat).

    Returns
    -------
    Path
        The path to the written CSV file.
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Aliased reservoir header names — also the keys we'll use to build
    # the Arrow columns below.  Coefficient lookup still uses the
    # original PLP names (per the ``name_alias`` docstring contract).
    aliased_names = [_apply_alias(r, name_alias) for r in reservoir_names]

    scale = _scale_factor(num_scenarios)

    # Per-reservoir FEscala scaling.  PLP's plpplem2.dat stores gradients in
    # `$/(raw volume unit)`, where the unit depends on the reservoir's
    # `FEscala` column from plpplem1.dat — `raw_unit = hm³ / 10^(FEscala-6)`.
    # gtopt's reservoir volumes are in physical hm³, so to convert the cut
    # gradient to `$/hm³` we multiply by `10^(FEscala-6)` per reservoir.
    # Without this, LMAULE (FEscala=9) cuts are 1000× too weak; CIPRESES
    # (FEscala=8) is 100× too weak; ELTORO (FEscala=10) is 10000× too weak.
    # The RHS (LDPhiPrv) is in raw `$` (no per-reservoir scale), so it
    # gets only the `1/N` divisor.  Default `None` is a no-op (back-compat).
    def _vol_scale(rname: str) -> float:
        if fescala_map is None:
            return 1.0
        f = fescala_map.get(rname)
        if f is None:
            return 1.0
        return 10.0 ** (f - 6)

    # Build column-major Python lists once, then hand them to PyArrow.
    # This mirrors the typed Arrow schema the gtopt-side loader expects
    # (``arrow::csv::TableReader`` with explicit int32 / float64 column
    # types), so the round-trip stays bit-exact through float64 storage
    # without the ``f"{:.10g}"`` text-formatting dance.
    iterations: List[int] = []
    scenes: List[int] = []
    rhs_values: List[float] = []
    state_columns: Dict[str, List[float]] = {a: [] for a in aliased_names}

    for cut in cuts:
        iterations.append(int(cut.get("iteration", 0)))
        scenes.append(int(cut["scene"]))
        rhs_values.append(float(cut["rhs"]) * scale)
        coeffs = cut.get("coefficients", {})
        for rname, aliased in zip(reservoir_names, aliased_names):
            state_columns[aliased].append(
                float(coeffs.get(rname, 0.0)) * scale * _vol_scale(rname)
            )

    fields = [
        pa.field("iteration", pa.int32()),
        pa.field("scene", pa.int32()),
        pa.field("rhs", pa.float64()),
    ] + [pa.field(name, pa.float64()) for name in aliased_names]
    columns: List[pa.Array] = [
        pa.array(iterations, type=pa.int32()),
        pa.array(scenes, type=pa.int32()),
        pa.array(rhs_values, type=pa.float64()),
    ] + [pa.array(state_columns[name], type=pa.float64()) for name in aliased_names]
    table = pa.Table.from_arrays(columns, schema=pa.schema(fields))

    # ``include_header=True`` (the default) writes a header row keyed by
    # the Arrow schema's field names — matches what the gtopt-side
    # reader expects to detect ``has_iteration_col`` and to resolve
    # state-variable column names.
    pacsv.write_csv(table, str(output_path))

    logger.debug(
        "Wrote %d boundary cuts to %s (%d state variables, scale=1/%s)",
        len(cuts),
        output_path,
        len(reservoir_names),
        num_scenarios if num_scenarios and num_scenarios > 1 else "1",
    )
    return output_path


# ``write_hot_start_cuts_csv`` was retired in 2026-05.  The "hot-start
# planos" CSV format was gtopt's own internal cut format; internal cuts
# now travel via the typed Parquet writer / loader (driven by the
# gtopt-side ``cuts_input_file`` / ``cuts_output_file`` options).  Only
# the PLP-compatible *boundary* cuts above are still emitted as CSV.
