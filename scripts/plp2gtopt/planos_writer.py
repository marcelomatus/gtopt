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

Mode-aware emission (``phi_expectation``, 2026-07)
--------------------------------------------------

The above ``1/NVarPhi`` pre-division applies ONLY to the legacy
``combined`` / ``separated`` load modes.  Under
``boundary_cuts_mode = phi_expectation`` (the plp2gtopt default) the
caller passes ``num_scenarios=None`` and the CSV is written **RAW**:
gtopt registers NVarPhi per-plane-hydrology ``φ_j`` terminal columns
priced ``p_s/NVarPhi`` and keys each cut by the ``scene`` column as
PLP's ``ISimul`` plane index, so the probability composition lives in
the LP column price, not in the file.  The C++ loader reads every
mode's CSV verbatim — the emission carries the normalization — which
keeps existing combined-mode files and results byte-identical.  The
uniform ``grad_scale`` gradient unit conversion (``$/dam³ → $/hm³``,
×1000 for PLP planos input — see the parameter docstring for the
2026-07-12 FEscala correction) applies in every mode (it is a
physical-unit conversion, not a probability weight).
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
    grad_scale: float = 1.0,
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
        disables scaling (back-compat).  Not used under
        ``boundary_cuts_mode = phi_expectation`` (raw emission).
    grad_scale
        UNIFORM physical-unit conversion applied to every gradient
        coefficient (never to ``rhs``).  For PLP planos input pass
        ``1000.0``: ``plpplem2.dat`` gradients are ``$/(10³ m³)``
        (= $/dam³, PLP's native "miles de m³" volume unit) for EVERY
        reservoir, and gtopt volumes are hm³.

        History (2026-07-12): this replaces the former per-reservoir
        ``fescala_map`` scaling ``10^(FEscala-6)``, which mis-read
        plem1's ``FEscala`` as a per-file unit exponent.  ``FEscala``
        is a pure LP-numerics column scale that CANCELS in the planos
        round trip — PLP's writer divides the internal gradient by
        ``ScaleVol`` (``plp-gdbdple.f:45``) and its reader multiplies
        it back at LP assembly (``defprbpd.f:775``,
        ``coef = PlaCFRho*(ScaleVol/ScalePhi)``) — so the on-disk
        gradient is $/dam³ regardless of FEscala.  Pinned empirically:
        the uniform-$/dam³ plane evaluation at plpemb.csv terminal
        volumes reproduces PLP's pdlin UB terminal component
        (2.0210e9 on the 2-year no-convenios case, per-sim range
        1.87–2.10e9) exactly, while the FEscala-scaled evaluation
        leaves a 0.3e9 hole; plem1's own VolMax column (dam³ for all
        FEscala values: ELTORO 5585888 dam³ = 5586 hm³ = Lago Laja)
        confirms the file-unit reading.

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
        # rhs gets the per-scene `scale` (1/NVarPhi) ONLY — never `grad_scale`.
        # grad_scale is a unit conversion for the gradient coefficients (∂/∂vol);
        # the rhs is already in objective units, so applying grad_scale here
        # would double-convert it.  See the `grad_scale` docstring.
        rhs_values.append(float(cut["rhs"]) * scale)
        coeffs = cut.get("coefficients", {})
        for rname, aliased in zip(reservoir_names, aliased_names):
            # coefficients get both: per-scene `scale` AND the unit `grad_scale`.
            state_columns[aliased].append(
                float(coeffs.get(rname, 0.0)) * scale * grad_scale
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

    # ``include_header=True`` (the default) writes a header row keyed
    # by the Arrow schema's field names — matches what the gtopt-side
    # reader expects to detect ``has_iteration_col`` and to resolve
    # state-variable column names.  ``quoting_style="needed"`` keeps
    # data cells unquoted unless they contain ``,`` / ``"`` / newline.
    # ``quoting_header="none"`` does the same for the header row;
    # PyArrow's default treats ``"needed"`` AND ``"all_valid"`` as
    # "quote every header" for the header row (per ``WriteOptions``
    # docs), so we force ``"none"`` to keep the field names readable
    # for downstream tools that grep / sed the header.
    write_options = pacsv.WriteOptions(
        quoting_style="needed",
        quoting_header="none",
    )
    pacsv.write_csv(table, str(output_path), write_options=write_options)

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
