# -*- coding: utf-8 -*-

"""Convert specific hydro generators' must-run ``pmin`` into FlowRights.

Companion to :mod:`gtopt_expand.pmin_flowright_expand`.  Where the
gtopt_expand subcommand performs the JSON-level transform but cannot
write parquet (it has no access to PLP source data), this module owns
the parquet emission so PLP-derived cases produced by ``plp2gtopt``
are self-consistent: the JSON references a parquet column, and that
column is actually written under ``<output_dir>/FlowRight/``.

Pipeline
--------
1. Load the whitelist (CSV path / comma-separated names / bundled
   default — see :func:`resolve_whitelist`).
2. For each whitelisted central that exists in the case:

   * Look up the gen waterway and its downstream junction
     (``junction_b``) via ``waterway_array``/``turbine_array``.
   * Read the central's ``Rendi`` (= ``efficiency``) from the central
     parser.
   * Build per-stage-per-block ``flow_min = pmin / Rendi`` from
     ``mance_parser`` if available, otherwise fall back to the
     central's static ``pmin``.
   * Write a one-column parquet file
     ``<output_dir>/FlowRight/<central>_pmin_as_flow_right.parquet``
     with columns ``[block, uid:<flow_right_uid>, stage]``.
   * Append a FlowRight JSON entry (``uid``, ``name``, ``purpose``,
     ``junction``, ``direction``, ``discharge``, ``fail_cost``).
3. Zero out the generator's ``pmin`` so the LP no longer enforces
   ``g >= pmin`` on every block (the FlowRight now expresses the
   obligation, with a soft slack).

UIDs start at :data:`_FLOW_RIGHT_UID_START` (5000) to match the
``gtopt_expand pmin_as_flowright`` ``--uid-start`` default.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional

import numpy as np
import pandas as pd

from .base_writer import BaseWriter

_logger = logging.getLogger(__name__)

#: Suffix appended to the central name for the FlowRight name and
#: parquet stem.  Must agree with
#: :data:`gtopt_expand.pmin_flowright_expand._FLOW_RIGHT_SUFFIX`.
_FLOW_RIGHT_SUFFIX: str = "_pmin_as_flow_right"

#: ``purpose`` field used on the emitted FlowRight (metadata only).
_FLOW_RIGHT_PURPOSE: str = "environmental"

#: ``direction`` for a consumptive (downstream) flow obligation.
_FLOW_RIGHT_DIRECTION: int = -1

#: First UID assigned to FlowRights emitted by this writer.  Matches
#: :data:`gtopt_expand.pmin_flowright_expand.DEFAULT_UID_START` so the
#: two paths agree when used standalone.
_FLOW_RIGHT_UID_START: int = 5000

#: Fallback ``fail_cost`` [$/m³/s·h] used when no rebalse cost is
#: available from ``plpvrebemb.dat`` and ``plpmat.dat`` ``CVert`` is
#: zero/missing.  Chosen high so the slack is the path of last resort.
_DEFAULT_FAIL_COST: float = 10000.0

#: Truthy values for the ``enabled`` column of the whitelist CSV.
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})


# ---------------------------------------------------------------------------
# Whitelist resolution
# ---------------------------------------------------------------------------
def _parse_csv(path: Path) -> List[str]:
    """Return the enabled central names from a pmin_as_flowright CSV.

    Mirrors the schema accepted by ``gtopt_expand`` — required columns
    are ``name`` and ``enabled``; optional ``description`` is ignored.
    """
    import csv  # noqa: PLC0415  (local import keeps module-level surface tiny)

    if not path.is_file():
        raise FileNotFoundError(f"pmin-as-flowright CSV not found: {path}")

    names: List[str] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        fieldnames = reader.fieldnames or []
        if "name" not in fieldnames or "enabled" not in fieldnames:
            raise ValueError(
                f"{path}: missing required column(s); got {list(fieldnames)}"
            )
        for row in reader:
            name = (row.get("name") or "").strip()
            if not name:
                continue
            enabled = (row.get("enabled") or "").strip().lower()
            if enabled in _TRUTHY:
                names.append(name)
    return names


def resolve_whitelist(
    spec: str,
    *,
    default_csv: Path,
) -> List[str]:
    """Resolve ``--pmin-as-flowright`` argument into an ordered list of names.

    The argument may be:

    * The empty string — use ``default_csv``.
    * A path to a CSV file — parse it.
    * A comma-separated list of central names — use directly.
    """
    if not spec:
        return _parse_csv(default_csv)

    candidate = Path(spec)
    if candidate.is_file():
        return _parse_csv(candidate)

    # Treat as a name list.  Empty tokens are dropped.
    return [tok.strip() for tok in spec.split(",") if tok.strip()]


# ---------------------------------------------------------------------------
# Writer
# ---------------------------------------------------------------------------
class PminFlowRightWriter(BaseWriter):
    """Emit FlowRight JSON entries + per-FlowRight discharge parquets.

    The writer is stateless across cases; one instance per ``plp2gtopt``
    run is enough.  ``options`` carries the standard plp2gtopt option
    bag (``output_dir``, ``output_format``, ``compression``, …).
    """

    def __init__(
        self,
        *,
        whitelist: List[str],
        central_parser: Any,
        mance_parser: Any,
        block_parser: Any,
        stage_parser: Any,
        scenarios: Optional[List[Dict[str, Any]]] = None,
        vrebemb_parser: Optional[Any] = None,
        plpmat_parser: Optional[Any] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        super().__init__(parser=None, options=options)
        self.whitelist = list(whitelist)
        self.central_parser = central_parser
        self.mance_parser = mance_parser
        self.block_parser = block_parser
        self.stage_parser = stage_parser
        self.scenarios = list(scenarios) if scenarios else []
        self.vrebemb_parser = vrebemb_parser
        self.plpmat_parser = plpmat_parser

    def _scenario_uids(self) -> List[int]:
        """Return the list of scenario UIDs used for the parquet rows.

        Falls back to ``[1]`` if no scenarios are configured.  The
        FlowRight discharge is currently scenario-independent (=
        ``pmin / Rendi``), so we replicate the same row-set across
        every scenario; gtopt's ``STBRealFieldSched`` reader expects a
        ``scenario`` column whose values match the case's scenario UIDs
        (set in ``simulation.scenario_array``).
        """
        if not self.scenarios:
            return [1]
        return [int(s["uid"]) for s in self.scenarios if "uid" in s]

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def process(
        self,
        planning_system: Dict[str, Any],
    ) -> List[Dict[str, Any]]:
        """Mutate *planning_system* in place; return the FlowRight entries.

        Steps:

        1. Build a ``{central: junction_b}`` map by scanning the
           ``turbine_array`` + ``waterway_array`` already present in
           ``planning_system``.
        2. For each whitelisted central:

           * Skip if not in ``central_parser`` (unknown / out of run).
           * Skip if the central has no gen waterway in the system
             (e.g. transit-only or skipped during junction processing).
           * Compute the per-stage-per-block flow schedule.
           * Write the parquet column.
           * Zero the matching generator's ``pmin``.
           * Append the FlowRight JSON.
        """
        if not self.whitelist:
            return []

        gen_junctions = self._build_gen_junction_map(planning_system)
        if not gen_junctions:
            _logger.warning(
                "pmin_as_flowright: no gen waterways found in planning;"
                " nothing to convert."
            )
            return []

        fail_cost = self._resolve_fail_cost()
        generators: List[Dict[str, Any]] = list(
            planning_system.get("generator_array", []) or []
        )
        flow_rights: List[Dict[str, Any]] = []
        rows_by_uid: Dict[int, np.ndarray] = {}
        block_index, stage_index = self._block_stage_indices()

        next_uid = _FLOW_RIGHT_UID_START
        for name in self.whitelist:
            entry, flow_values = self._build_flow_right(
                central_name=name,
                uid=next_uid,
                gen_junctions=gen_junctions,
                generators=generators,
                fail_cost=fail_cost,
                block_index=block_index,
            )
            if entry is None:
                continue
            flow_rights.append(entry)
            rows_by_uid[next_uid] = flow_values
            next_uid += 1

        if not flow_rights:
            return []

        # Persist generator pmin overrides.
        planning_system["generator_array"] = generators

        # Append FlowRights to planning system.
        fr_array = planning_system.setdefault("flow_right_array", [])
        if not isinstance(fr_array, list):
            raise ValueError("planning system 'flow_right_array' must be a list")
        fr_array.extend(flow_rights)

        # Write the parquet file(s) — one per FlowRight to match the
        # JSON ``discharge`` string ref format expected by gtopt.
        self._write_parquets(flow_rights, rows_by_uid, block_index, stage_index)

        # ``fail_cost`` is the gtopt-internal $/(m³/s·h) coefficient; the
        # user-facing PLP-native equivalent is /3.6 ($/Hm³).  Show both so
        # the operator can cross-check against plpmat.dat directly.
        plp_to_gtopt_units: float = 3.6
        fail_cost_plp = fail_cost / plp_to_gtopt_units
        _logger.info(
            "pmin_as_flowright: emitted %d FlowRight(s)"
            " (fail_cost = %g $/Hm³ ≡ %g $/(m³/s·h) internal)",
            len(flow_rights),
            fail_cost_plp,
            fail_cost,
        )
        return flow_rights

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _build_gen_junction_map(
        self, planning_system: Mapping[str, Any]
    ) -> Dict[str, str]:
        """Return ``{central_name: junction_b}`` for each gen waterway.

        The convention is that JunctionWriter names the gen waterway
        ``"<central_name>_<central_id>_<target_id>"`` and binds a
        Turbine entry whose ``name == central_name`` to that waterway
        via the turbine's ``waterway`` field.  We resolve ``junction_b``
        by joining ``turbine_array`` → ``waterway_array``.
        """
        waterways = {
            ww.get("name"): ww
            for ww in planning_system.get("waterway_array", []) or []
            if isinstance(ww, dict) and ww.get("name")
        }
        result: Dict[str, str] = {}
        for tur in planning_system.get("turbine_array", []) or []:
            if not isinstance(tur, dict):
                continue
            cname = tur.get("name")
            wname = tur.get("waterway")
            if not isinstance(cname, str) or not isinstance(wname, str):
                continue
            ww = waterways.get(wname)
            if not ww:
                continue
            jb = ww.get("junction_b")
            if isinstance(jb, str) and jb:
                result[cname] = jb
        return result

    def _resolve_fail_cost(self) -> float:
        """Return the internal ``fail_cost`` (in gtopt's $/(m³/s·h)).

        User-facing convention is **PLP-native ``$/Hm³``** — the same
        units an engineer reads in ``plpmat.dat`` ``CCauFal``.  Internally
        gtopt's ``FlowRight.fail_cost`` is per-block ``$/(m³/s·h)``, so
        every PLP-native value is multiplied by ``FactTiempoH = 3.6`` to
        produce the value emitted into JSON.

        Resolution order (all values *returned* are gtopt-internal):

        0. CLI override ``--flow-right-fail-cost VALUE`` (read from
           ``self.options['flow_right_fail_cost']``) wins over every
           data-derived value.  **Interpreted as $/Hm³** (PLP convention)
           and converted via ``× 3.6`` before being emitted.  Example:
           ``--flow-right-fail-cost 5000`` → ``18000`` in JSON.

        1. ``plpmat.dat`` ``CCauFal`` (= flow-failure cost, parsed as
           ``plpmat_parser.flow_fail_cost``, native unit $/Hm³).
           Verified against PLP source (``CEN65/src/genpdago.f:101``):
              ``FO(qaf) = CCaudFalla_scaled × BloDur / FPhi``
           where ``CCaudFalla_scaled = CCauFal × FactTiempoH`` (set in
           ``leemat.f:118``).  Returned as ``CCauFal × 3.6``.  E.g.
           juan/gtopt_iplp's ``CCauFal = 7000 $/Hm³`` →
           ``25,200 $/(m³/s·h)``.
        2. ``2 × max(rebalse_cost)`` over plpvrebemb.dat entries (also
           PLP-native $/Hm³) — same ``× 3.6`` conversion.  Used when
           ``CCauFal`` unavailable.
        3. ``2 × CVert`` (plpmat.dat default vert cost, $/Hm³) — same
           conversion.
        4. :data:`_DEFAULT_FAIL_COST` final fallback (already in gtopt
           ``$/(m³/s·h)`` units — no further conversion).
        """
        # PLP-to-gtopt unit conversion factor.  PLP's CCaudFalla is
        # multiplied internally by ``FactTiempoH = 3.6`` (CEN65/src/pxp.fpp,
        # leemat.f:118) before the obj coefficient is built in
        # genpdago.f:101 as ``CCaudFalla_scaled × BloDur / FPhi``.  gtopt's
        # ``fail_cost`` is $/(m³/s·h), so the equivalent gtopt value for
        # any PLP-native $/Hm³ quantity is **value × 3.6**.
        plp_to_gtopt_units: float = 3.6

        # 0. CLI override — PLP-native $/Hm³, converted to gtopt units.
        override = self.options.get("flow_right_fail_cost") if self.options else None
        if override is not None:
            return float(override) * plp_to_gtopt_units

        ccaufal = 0.0
        if self.plpmat_parser is not None:
            ccaufal = float(getattr(self.plpmat_parser, "flow_fail_cost", 0.0) or 0.0)
        if ccaufal > 0.0:
            return ccaufal * plp_to_gtopt_units

        max_rebalse = 0.0
        if self.vrebemb_parser is not None:
            for entry in self.vrebemb_parser.get_all() or []:
                cost = float(entry.get("cost", 0.0) or 0.0)
                max_rebalse = max(max_rebalse, cost)
        if max_rebalse > 0.0:
            return 2.0 * max_rebalse * plp_to_gtopt_units

        cvert = 0.0
        if self.plpmat_parser is not None:
            cvert = float(getattr(self.plpmat_parser, "vert_cost", 0.0) or 0.0)
        if cvert > 0.0:
            return 2.0 * cvert * plp_to_gtopt_units

        return _DEFAULT_FAIL_COST

    def _block_stage_indices(self) -> tuple[np.ndarray, np.ndarray]:
        """Return parallel ``(block, stage)`` int32 arrays for the parquet.

        Mirrors the ``Generator/pmin.parquet`` layout — one row per
        block, with the corresponding stage number duplicated across
        all blocks belonging to that stage.
        """
        if self.block_parser is None or not self.block_parser.items:
            return np.empty(0, dtype=np.int32), np.empty(0, dtype=np.int32)
        block_index = np.array(
            [b["number"] for b in self.block_parser.items], dtype=np.int32
        )
        stage_index = np.array(
            [self.block_parser.get_stage_number(int(b)) for b in block_index],
            dtype=np.int32,
        )
        return block_index, stage_index

    def _build_flow_right(
        self,
        *,
        central_name: str,
        uid: int,
        gen_junctions: Mapping[str, str],
        generators: List[Dict[str, Any]],
        fail_cost: float,
        block_index: np.ndarray,
    ) -> tuple[Optional[Dict[str, Any]], np.ndarray]:
        """Build one FlowRight entry + its per-block flow vector.

        Returns ``(None, empty_array)`` when the central is not eligible
        (missing in central_parser, no gen waterway, non-positive
        Rendi, etc.) — caller should ``continue``.
        """
        empty = np.empty(0, dtype=np.float64)

        junction_b = gen_junctions.get(central_name)
        if junction_b is None:
            _logger.warning(
                "pmin_as_flowright: central '%s' has no gen waterway in"
                " planning; skipping.",
                central_name,
            )
            return None, empty

        central = (
            self.central_parser.get_central_by_name(central_name)
            if self.central_parser is not None
            else None
        )
        if central is None:
            _logger.warning(
                "pmin_as_flowright: central '%s' not found in"
                " central_parser; skipping.",
                central_name,
            )
            return None, empty

        rendi = float(central.get("efficiency", 0.0) or 0.0)
        if rendi <= 0.0:
            _logger.warning(
                "pmin_as_flowright: central '%s' has non-positive"
                " efficiency (%g); skipping.",
                central_name,
                rendi,
            )
            return None, empty

        flow_values = self._build_flow_values(
            central_name=central_name,
            central=central,
            rendi=rendi,
            block_index=block_index,
        )
        if flow_values.size == 0 or float(flow_values.max()) == 0.0:
            _logger.info(
                "pmin_as_flowright: central '%s' has no positive pmin;"
                " skipping (nothing to convert).",
                central_name,
            )
            return None, empty

        # Zero the matching generator's pmin so the LP doesn't double-
        # count the obligation.  Generators in plp2gtopt are keyed by
        # name (matching central_name).
        for gen in generators:
            if isinstance(gen, dict) and gen.get("name") == central_name:
                gen["pmin"] = 0.0
                break

        column_name = f"{central_name}{_FLOW_RIGHT_SUFFIX}"
        entry: Dict[str, Any] = {
            "uid": uid,
            "name": column_name,
            "purpose": _FLOW_RIGHT_PURPOSE,
            "junction": junction_b,
            "direction": _FLOW_RIGHT_DIRECTION,
            "discharge": column_name,
            "fail_cost": fail_cost,
        }
        return entry, flow_values

    def _build_flow_values(
        self,
        *,
        central_name: str,
        central: Mapping[str, Any],
        rendi: float,
        block_index: np.ndarray,
    ) -> np.ndarray:
        """Compute ``pmin / rendi`` per block.

        Prefers per-stage-per-block plpmance values when available;
        otherwise broadcasts the central's static ``pmin``.
        """
        static_pmin = float(central.get("pmin", 0.0) or 0.0)

        mance_entry = (
            self.mance_parser.get_mance_by_name(central_name)
            if self.mance_parser is not None
            else None
        )
        if mance_entry is None:
            if static_pmin == 0.0 or block_index.size == 0:
                return np.zeros(block_index.size, dtype=np.float64)
            return np.full(block_index.size, static_pmin / rendi, dtype=np.float64)

        # mance: ``block`` (np.array of ints) and ``pmin`` (np.array of floats).
        mance_block = np.asarray(mance_entry.get("block"), dtype=np.int64)
        mance_pmin = np.asarray(mance_entry.get("pmin"), dtype=np.float64)
        if mance_block.size == 0 or mance_pmin.size == 0:
            if static_pmin == 0.0 or block_index.size == 0:
                return np.zeros(block_index.size, dtype=np.float64)
            return np.full(block_index.size, static_pmin / rendi, dtype=np.float64)

        # Build a {block: pmin} map (last-wins for duplicates) so we can
        # broadcast onto the canonical block_index of the case.
        pmin_by_block: Dict[int, float] = {}
        for blk, val in zip(mance_block.tolist(), mance_pmin.tolist()):
            pmin_by_block[int(blk)] = float(val)

        out = np.empty(block_index.size, dtype=np.float64)
        for i, blk in enumerate(block_index.tolist()):
            out[i] = pmin_by_block.get(int(blk), static_pmin) / rendi
        return out

    def _write_parquets(
        self,
        flow_rights: List[Dict[str, Any]],
        rows_by_uid: Dict[int, np.ndarray],
        block_index: np.ndarray,
        stage_index: np.ndarray,
    ) -> None:
        """Write one ``FlowRight/<name>.parquet`` per emitted FlowRight."""
        if not flow_rights or block_index.size == 0:
            return
        output_dir = (
            Path(self.options["output_dir"]) / "FlowRight"
            if "output_dir" in self.options
            else Path("FlowRight")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        for entry in flow_rights:
            uid = int(entry["uid"])
            stem = str(entry["discharge"])
            values = rows_by_uid.get(uid)
            if values is None or values.size == 0:
                continue
            # FlowRight.discharge is STBRealFieldSched (Scenario ×
            # sTage × Block), so the parquet MUST carry a `scenario`
            # column populated with the actual scenario UID(s) the case
            # uses (NOT a hardcoded 1).  Mirror the convention of
            # `Flow/discharge.parquet`: one row per (scenario, stage,
            # block).  When more than one scenario is in the case, the
            # frame is replicated per scenario (the discharge value is
            # currently scenario-independent for pmin-as-flowright).
            scen_uids = self._scenario_uids()
            frames: list[pd.DataFrame] = []
            for s_uid in scen_uids:
                frames.append(
                    pd.DataFrame(
                        {
                            "scenario": np.full(
                                block_index.size, s_uid, dtype=np.int32
                            ),
                            "block": block_index,
                            f"uid:{uid}": values,
                            "stage": stage_index,
                        }
                    )
                )
            df = pd.concat(frames, ignore_index=True) if len(frames) > 1 else frames[0]
            self.write_dataframe(df, output_dir, stem)
