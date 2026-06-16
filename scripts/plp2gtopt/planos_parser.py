"""Parser for PLP boundary-cuts files (plpplaem / plpplem).

These files define the "planos de embalse" — external optimality cuts that
approximate the expected future cost beyond the planning horizon.  In SDDP
literature these are known as the *future cost function* (FCF) or Benders
cuts at the last stage boundary.

PLP uses two files (referenced in ``leeplaem.f`` Fortran subroutine).
Two naming conventions are supported:

* ``plpplaem1.dat`` / ``plpplaem2.dat`` — original naming
* ``plpplem1.dat``  / ``plpplem2.dat``  — abbreviated naming

**File 1** (reservoir-name mapping) — two formats are accepted:

*Simple format* (original)::

    PLPNCenEmb                              ← count header
    PLPIEmb  'PLPCenNom'                    ← 1-based index, quoted name
    ...

*CSV format* (extended, used by newer PLP builds)::

    #Numero, Nombre, Tipo, Barra, ...       ← comment header (skipped)
    1,LMAULE                  ,A,  0, ...   ← index, name, extra fields…

Both formats are read the same way the Fortran ``READ(*, *) PLPIEmb, PLPCenNom``
would: the first field is the 1-based index, the second is the name, and any
remaining fields are ignored.

**File 2** (cut data)::

    NumEtaCF                                ← boundary stage number (1-based)
    IPDNumIte  IEtapa  ISimul  LDPhiPrv  GradX(1)...GradX(N)

Cuts where ``IEtapa == NumEtaCF`` are *boundary cuts* (future-cost function
for the last stage).  All other cuts are *hot-start cuts* that apply to
intermediate stages.

The gradient coefficients ``GradX`` correspond to the reservoir volumes
(state variables) identified by the name mapping in file 1.

The resulting ``varphi`` (φ) variable satisfies the PLP-canonical cut::

    φ ≥ +LDPhiPrv + Σ_i GradX_i · Vol_i

where ``LDPhiPrv = E[Z(v_trial)]`` is the **positive** expected future
operating cost from the next stage onwards.  Per PLP source
``plp-espercnd.f:43-54``, ``LDPhiPrv = PromedioZ / NApert`` — averaged
over **apertures** (intra-stage uncertainty branching), NOT over
scenarios.  So it is the per-scenario expectation given apertures,
which maps to gtopt's per-SCENE expected α-cost.
``GradX_i = ∂E[Z]/∂v_i`` is the **negative** marginal water value (more
water → less future cost).  The parsed CSV ``rhs`` column carries
``+LDPhiPrv`` directly, with no sign flip — matching PLP's
``plp-agrespd.f::AgrResPDi`` which builds the LP row as
``rowlb = +LDPhiPrv/ScalePhi`` for the constraint
``α + Σ(-GradX/ScalePhi)·v ≥ +LDPhiPrv/ScalePhi``.

**Bridge to gtopt** (handled at WRITE time by ``planos_writer``):
PLP's LP folds the probability factor into the α-col objective
coefficient as ``1/NVarPhi``; gtopt's per-scene LP uses an α-col
coefficient of ``1.0`` (see ``sddp_method_alpha.cpp:110``).  When 16
per-scene LP objectives are summed for the aggregate LB, each scene
contributes its full α floor → over-counts by N_scenarios.  The
writer therefore divides ``rhs`` and every gradient coefficient by
``N`` at export, so the verbatim "PLP-canonical cut" above is
rescaled into "gtopt-canonical α-space" exactly once.
"""

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

from plp2gtopt.base_parser import BaseParser

logger = logging.getLogger(__name__)

# FEscala field index (0-based) in the CSV-format plpplem1.dat / plpplaem1.dat.
# The CSV row layout is:
#   Numero, Nombre, Tipo, Barra, N/A, VolMin, VolMax, VolMinNECF, VolMaxNECF,
#   FEscala, FactRendim
# so FEscala is at index 9.
_FESCALA_IDX = 9


# -- File-discovery helper ---------------------------------------------------


def find_planos_files(
    input_path: Path,
) -> Optional[tuple[Path, Path]]:
    """Locate the two planos files under *input_path*.

    Returns ``(file1, file2)`` or ``None`` if they cannot be found.
    Both the original (``plpplaem*``) and abbreviated (``plpplem*``)
    naming conventions are tried, including compressed variants.
    """
    from .compressed_open import find_compressed_path

    for prefix in ("plpplaem", "plpplem"):
        f1 = find_compressed_path(input_path / f"{prefix}1.dat")
        f2 = find_compressed_path(input_path / f"{prefix}2.dat")
        if f1 is not None and f2 is not None:
            return f1, f2
    return None


class PlanosParser(BaseParser):
    """Parse PLP plpplaem/plpplem boundary-cut files.

    Attributes
    ----------
    reservoir_names : list[str]
        Ordered reservoir names from plpplaem1/plpplem1.
    reservoir_fescala : dict[str, int]
        Mapping from reservoir name to FEscala exponent (from the CSV
        format of plpplem1.dat, field index 9).  Empty when the simple
        format is used or the FEscala column is absent.
    boundary_stage : int
        The PLP stage number to which boundary cuts apply (1-based).
    cuts : list[dict]
        Boundary cuts (``IEtapa == boundary_stage``), each with keys
        ``name``, ``iteration``, ``scene``, ``rhs``, and
        ``coefficients`` (dict of reservoir name → float).
    all_cuts : list[dict]
        *All* parsed cuts (all stages), each with the same keys as
        :attr:`cuts` plus ``stage`` (1-based PLP IEtapa).  This is
        used to generate hot-start cuts for intermediate stages.
    """

    def __init__(
        self,
        file_path1: str | Path,
        file_path2: str | Path,
    ) -> None:
        """Initialise with paths to both plpplaem/plpplem files."""
        super().__init__(file_path1)
        self.file_path1 = Path(file_path1)
        self.file_path2 = Path(file_path2)
        self.reservoir_names: List[str] = []
        self.reservoir_fescala: Dict[str, int] = {}
        self.boundary_stage: int = 0
        self.cuts: List[Dict[str, Any]] = []
        self.all_cuts: List[Dict[str, Any]] = []

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:  # noqa: ARG002
        """Parse both files and populate :attr:`cuts` and :attr:`all_cuts`."""
        self._parse_reservoir_map()
        self._parse_cut_data()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _parse_reservoir_map(self) -> None:
        """Parse file 1 for reservoir-name mapping and optional FEscala.

        Supports both the simple format (count header + index/name pairs)
        and the CSV format (comment header + ``index,name,...`` rows).
        The Fortran reader ``READ(*, *) PLPIEmb, PLPCenNom`` treats both
        formats identically because free-format READ splits on whitespace
        or commas and ignores trailing fields.

        In the CSV format, field index 9 (0-based) contains ``FEscala``
        — a logarithmic volume-scale exponent.  The LP volume scale is
        ``10^(FEscala - 6)`` (e.g. FEscala=9 → 1000, FEscala=8 → 100).
        When present, these values are stored in :attr:`reservoir_fescala`.
        """
        lines = self._read_lines(self.file_path1)
        if not lines:
            logger.warning("%s is empty — no boundary cuts", self.file_path1.name)
            return

        # Detect format: if the first line is a single integer it is the
        # "count header" of the simple format.  Otherwise every line is a
        # data row (CSV / extended format).
        first_fields = self._split_fields(lines[0])
        is_simple = len(first_fields) == 1 and first_fields[0].isdigit()

        if is_simple:
            num_reservoirs = int(first_fields[0])
            data_lines = lines[1 : 1 + num_reservoirs]
        else:
            # CSV / extended format — every line is a data row
            data_lines = lines
            num_reservoirs = len(data_lines)

        for dline in data_lines:
            fields = self._split_fields(dline)
            if len(fields) < 2:
                continue
            # Field 0: 1-based index (ignored for ordering)
            # Field 1: reservoir name (may have trailing spaces / quotes)
            name = fields[1].strip().strip("'\"")
            self.reservoir_names.append(name)

            # Extract FEscala when present (CSV extended format only)
            # FEscala field index in the CSV format (0-based): field 9.
            if len(fields) > _FESCALA_IDX:
                try:
                    fescala = int(fields[_FESCALA_IDX].strip())
                    self.reservoir_fescala[name] = fescala
                except (ValueError, IndexError):
                    pass  # Not a valid integer — skip silently

        logger.debug(
            "%s: %d reservoir(s) in boundary cuts: %s",
            self.file_path1.name,
            len(self.reservoir_names),
            ", ".join(self.reservoir_names),
        )

    def _parse_cut_data(self) -> None:
        """Parse file 2 for cut coefficients.

        Populates both :attr:`cuts` (boundary only) and :attr:`all_cuts`
        (all stages).
        """
        lines = self._read_lines(self.file_path2)
        if not lines:
            logger.warning("%s is empty — no boundary cuts", self.file_path2.name)
            return

        idx = 0
        # First non-empty line: boundary stage number (1-based)
        self.boundary_stage = int(self._split_fields(lines[idx])[0])
        idx += 1

        num_reservoirs = len(self.reservoir_names)
        boundary_count = 0
        total_count = 0

        while idx < len(lines):
            fields = self._split_fields(lines[idx])
            if len(fields) < 4 + num_reservoirs:
                logger.warning(
                    "%s line %d: expected %d fields, got %d — skipping",
                    self.file_path2.name,
                    idx,
                    4 + num_reservoirs,
                    len(fields),
                )
                idx += 1
                continue

            iter_num = int(fields[0])  # IPDNumIte
            stage = int(fields[1])  # IEtapa (1-based)
            scenario = int(fields[2])  # ISimul (1-based)
            # LDPhiPrv = E[Z(v_trial)] from PLP backward pass — positive
            # expected future operating cost (set by `plp-espercnd.f:54`
            # as `LDPhiPrv = PromedioZ`).  Written verbatim by
            # `plp-gdbdple.f:42` and `plp-gdbdple2.f:93`.
            ld_phi_prv = float(fields[3])

            coefficients: Dict[str, float] = {}
            for ri, rname in enumerate(self.reservoir_names):
                coeff = float(fields[4 + ri])
                if coeff != 0.0:
                    coefficients[rname] = coeff

            cut = {
                "name": f"bc_{iter_num}_{scenario}",
                "iteration": iter_num,
                "stage": stage,
                "scene": scenario,  # PLP ISimul maps to scene UID
                # rhs = +LDPhiPrv (no sign flip).  PLP's LP cut at row
                # construction time (`plp-agrespd.f:194,203`) sets
                # rowlb = +LDPhiPrv/ScalePhi for the row
                # `α + Σ(-GradX/ScalePhi)·v ≥ rowlb`.  gtopt's loader at
                # `source/sddp_boundary_cuts.cpp:418` consumes
                # `rc.rhs` directly as `lowb`, so the CSV must carry
                # the positive intercept.  An earlier version negated
                # this on emit, producing α ≈ -LDPhiPrv at the last
                # phase — verified against PLP source 2026-05-05 and
                # corrected.
                "rhs": ld_phi_prv,
                "coefficients": coefficients,
            }
            self.all_cuts.append(cut)
            total_count += 1

            if stage == self.boundary_stage:
                self.cuts.append(cut)
                boundary_count += 1

            idx += 1

        logger.debug(
            "%s: loaded %d boundary cuts (stage %d) and %d total cuts",
            self.file_path2.name,
            boundary_count,
            self.boundary_stage,
            total_count,
        )

    # ------------------------------------------------------------------
    # Cap helpers — boundary-cut-derived marginal water values
    # ------------------------------------------------------------------

    def lower_bound_water_value_by_reservoir(
        self,
        *,
        num_scenarios: Optional[int] = None,
        apply_fescala: bool = True,
    ) -> Dict[str, float]:
        """Per-reservoir cut **lower-bound** water value across all cuts.

        Returns ``{reservoir_name: lower_bound}`` in ``$/hm³``.  This is
        the boundary-cut LOWER BOUND of the (positive) water-value cost,
        replicating the gtopt C++ ``cut_soft_cost(min)`` rule that the
        Python converters now own.

        Sign convention
        ---------------
        PLP's plpplem2.dat ships ``GradX_i = ∂E[Z]/∂v_i < 0`` on each
        reservoir state variable — more stored water lowers the future
        cost, so the coefficient is **negative**.  The corresponding
        positive water value is ``-GradX_i``.  The *lower bound* of that
        positive cost is therefore obtained from the **maximum** (least
        negative) coefficient: ``-max(coeff)`` — see
        :func:`gtopt_shared.water_values.cut_lower_bound`, which also
        positive-floors the result when a cut prices water as a liability.

        The coefficients here are collected **SIGNED** (no ``abs``),
        because the lower-bound rule depends on the sign (``-max`` vs
        ``-min``).  The per-reservoir ``scale`` (``1/num_scenarios`` when
        ``num_scenarios >= 2``) and FEscala ``_vscale`` adjustment land
        the result in the same per-scene ``$/hm³`` space as
        ``Reservoir.efin_cost``.

        Args:
            num_scenarios: PLP scenario count (``NVarPhi``).  When ``>= 2``
                the raw PLP gradients are divided by ``num_scenarios`` to
                land in gtopt per-scene ``$/hm³``.  ``None``/``0``/``1``
                disables the divisor.
            apply_fescala: when True (default) multiply each coefficient by
                ``10^(FEscala - 6)`` to land in ``$/hm³`` (gtopt's volume
                basis), matching ``planos_writer._vol_scale``.

        Returns:
            Mapping from reservoir name to its cut lower-bound water value
            in ``$/hm³``.  Reservoirs whose cuts carry only zero
            coefficients (or no cut at all) are omitted.
        """
        from gtopt_shared.water_values import cut_lower_bound  # noqa: PLC0415

        if num_scenarios is not None and num_scenarios > 1:
            scale = 1.0 / float(num_scenarios)
        else:
            scale = 1.0

        def _vscale(rname: str) -> float:
            if not apply_fescala:
                return 1.0
            f = self.reservoir_fescala.get(rname)
            if f is None:
                return 1.0
            return 10.0 ** (f - 6)

        # Collect SIGNED scaled coefficients per reservoir (skip val==0 so
        # structural zeros do not pollute the lower-bound reduction).
        signed: Dict[str, List[float]] = {}
        for cut in self.all_cuts:
            coeffs = cut.get("coefficients") or {}
            for rname, raw in coeffs.items():
                try:
                    val = float(raw)
                except (TypeError, ValueError):
                    continue
                if val == 0.0:
                    continue
                signed.setdefault(rname, []).append(val * scale * _vscale(rname))

        result: Dict[str, float] = {}
        for rname, vals in signed.items():
            lb = cut_lower_bound(vals)
            if lb is not None:
                result[rname] = lb
        return result

    # ------------------------------------------------------------------
    # Utility
    # ------------------------------------------------------------------

    @staticmethod
    def _split_fields(line: str) -> List[str]:
        """Split a line on commas or whitespace, stripping each token."""
        if "," in line:
            return [f.strip() for f in line.split(",") if f.strip()]
        return line.split()

    @staticmethod
    def _read_lines(filepath: Path) -> List[str]:
        """Read non-empty, non-comment lines from a file."""
        from .compressed_open import compressed_open

        with compressed_open(filepath) as fobj:
            result = []
            for line in fobj:
                stripped = line.strip()
                if stripped and not stripped.startswith("#"):
                    result.append(stripped)
            return result
