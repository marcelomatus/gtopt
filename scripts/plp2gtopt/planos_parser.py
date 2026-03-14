"""Parser for PLP boundary-cuts files (plpplaem1.dat / plpplaem2.dat).

These files define the "planos de embalse" — external optimality cuts that
approximate the expected future cost beyond the planning horizon.  In SDDP
literature these are known as the *future cost function* (FCF) or Benders
cuts at the last stage boundary.

PLP uses two files (referenced in ``leeplaem.f`` Fortran subroutine):

``plpplaem1.dat`` (reservoir-name mapping)
    Header: ``PLPNCenEmb`` (number of reservoirs in the boundary cuts)
    Data lines: ``PLPIEmb  'PLPCenNom'``  (1-based index, quoted name)

``plpplaem2.dat`` (cut data)
    Header: ``NumEtaCF`` (boundary stage number, 1-based)
    Data lines: ``IPDNumIte  IEtapa  ISimul  LDPhiPrv  GradX(1)...GradX(N)``
    Only cuts for ``IEtapa == NumEtaCF`` are retained.

The gradient coefficients ``GradX`` correspond to the reservoir volumes
(state variables) identified by the name mapping in ``plpplaem1.dat``.

The resulting ``varphi`` (φ) variable in PLP satisfies:
    φ ≥ -LDPhiPrv + Σ_i GradX_i · Vol_i

This parser produces a list of dicts suitable for writing a gtopt-compatible
boundary-cuts CSV file via :class:`PlanosWriter`.
"""

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

from plp2gtopt.base_parser import BaseParser

logger = logging.getLogger(__name__)


class PlanosParser(BaseParser):
    """Parse PLP plpplaem1.dat + plpplaem2.dat boundary-cut files.

    Attributes
    ----------
    reservoir_names : list[str]
        Ordered reservoir names from plpplaem1.dat.
    boundary_stage : int
        The PLP stage number to which the cuts apply (1-based).
    cuts : list[dict]
        Parsed cuts, each with keys ``name``, ``scenario``, ``rhs``,
        and a ``coefficients`` dict mapping reservoir names to floats.
    """

    def __init__(
        self,
        file_path1: str | Path,
        file_path2: str | Path,
    ) -> None:
        """Initialise with paths to both plpplaem files."""
        super().__init__(file_path1)
        self.file_path1 = Path(file_path1)
        self.file_path2 = Path(file_path2)
        self.reservoir_names: List[str] = []
        self.boundary_stage: int = 0
        self.cuts: List[Dict[str, Any]] = []

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:  # noqa: ARG002
        """Parse both files and populate :attr:`cuts`."""
        self._parse_reservoir_map()
        self._parse_cut_data()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _parse_reservoir_map(self) -> None:
        """Parse ``plpplaem1.dat`` for reservoir-name mapping."""
        lines = self._read_lines(self.file_path1)
        if not lines:
            logger.warning("plpplaem1.dat is empty — no boundary cuts")
            return

        idx = 0
        # First line: number of reservoirs in the boundary cuts
        num_reservoirs = int(lines[idx].split()[0])
        idx += 1

        for _ in range(num_reservoirs):
            if idx >= len(lines):
                break
            parts = lines[idx].split(maxsplit=1)
            # Format: PLPIEmb  'PLPCenNom'
            # Index is 1-based but we only care about the name order
            name = parts[1].strip().strip("'") if len(parts) > 1 else parts[0]
            self.reservoir_names.append(name)
            idx += 1

        logger.info(
            "plpplaem1.dat: %d reservoir(s) in boundary cuts: %s",
            len(self.reservoir_names),
            ", ".join(self.reservoir_names),
        )

    def _parse_cut_data(self) -> None:
        """Parse ``plpplaem2.dat`` for cut coefficients."""
        lines = self._read_lines(self.file_path2)
        if not lines:
            logger.warning("plpplaem2.dat is empty — no boundary cuts")
            return

        idx = 0
        # First non-empty line: boundary stage number (1-based)
        self.boundary_stage = int(lines[idx].split()[0])
        idx += 1

        num_reservoirs = len(self.reservoir_names)
        cut_count = 0

        while idx < len(lines):
            parts = lines[idx].split()
            if len(parts) < 4 + num_reservoirs:
                logger.warning(
                    "plpplaem2.dat line %d: expected %d fields, got %d — skipping",
                    idx,
                    4 + num_reservoirs,
                    len(parts),
                )
                idx += 1
                continue

            iter_num = int(parts[0])  # IPDNumIte
            stage = int(parts[1])  # IEtapa (1-based)
            scenario = int(parts[2])  # ISimul (1-based)
            ld_phi_prv = float(parts[3])  # LDPhiPrv (intercept, negated)

            # Only keep cuts for the boundary stage
            if stage == self.boundary_stage:
                coefficients: Dict[str, float] = {}
                for ri, rname in enumerate(self.reservoir_names):
                    coeff = float(parts[4 + ri])
                    if coeff != 0.0:
                        coefficients[rname] = coeff

                self.cuts.append(
                    {
                        "name": f"bc_{iter_num}_{scenario}",
                        "iteration": iter_num,
                        "scene": scenario,  # PLP ISimul maps to scene UID
                        "rhs": -ld_phi_prv,  # PLP stores negative intercept
                        "coefficients": coefficients,
                    }
                )
                cut_count += 1

            idx += 1

        logger.info(
            "plpplaem2.dat: loaded %d boundary cuts for stage %d",
            cut_count,
            self.boundary_stage,
        )

    @staticmethod
    def _read_lines(filepath: Path) -> List[str]:
        """Read non-empty, non-comment lines from a file."""
        with open(filepath, "r", encoding="ascii", errors="ignore") as fobj:
            result = []
            for line in fobj:
                stripped = line.strip()
                if stripped and not stripped.startswith("#"):
                    result.append(stripped)
            return result
