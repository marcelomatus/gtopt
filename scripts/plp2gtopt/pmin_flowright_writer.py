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

from gtopt_shared.csv_io import write_csv

from .base_writer import BaseWriter
from ._water_value import WaterValueResolver

_logger = logging.getLogger(__name__)

#: Suffix appended to the central name for the FlowRight name and
#: parquet stem.  Must agree with
#: :data:`gtopt_expand.pmin_flowright_expand._FLOW_RIGHT_SUFFIX`.
_FLOW_RIGHT_SUFFIX: str = "_pmin_as_flow_right"

#: Suffix appended to the waterway name for the FlowRight name and
#: parquet stem produced by the waterway-fmin → FlowRight transform.
_WATERWAY_FLOW_RIGHT_SUFFIX: str = "_fmin_as_flow_right"

#: First UID assigned to FlowRights emitted by the waterway-fmin path.
#: Sits above :data:`_FLOW_RIGHT_UID_START` so the two paths do not
#: collide when both run in the same plp2gtopt invocation.
_WATERWAY_FLOW_RIGHT_UID_START: int = 5500

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
        vrebemb_parser: Optional[Any] = None,
        plpmat_parser: Optional[Any] = None,
        cenre_parser: Optional[Any] = None,
        options: Optional[Dict[str, Any]] = None,
        water_value_resolver: Optional[WaterValueResolver] = None,
    ) -> None:
        super().__init__(parser=None, options=options)
        self.whitelist = list(whitelist)
        self.central_parser = central_parser
        self.mance_parser = mance_parser
        self.block_parser = block_parser
        self.stage_parser = stage_parser
        self.vrebemb_parser = vrebemb_parser
        self.plpmat_parser = plpmat_parser
        self.cenre_parser = cenre_parser
        # Auto water-shortfall pricing helper.  When the resolver is
        # active (``is_active`` True — set by either ``--auto-water-
        # fail-cost`` or the explicit ``--water-fail-cost`` override),
        # ``fail_cost`` is resolved per-central via
        # :meth:`junction_lost_pf`; otherwise the legacy uniform
        # ``_resolve_fail_cost`` cascade is used.
        _opts = self.options or {}
        _wvr_gate = bool(_opts.get("auto_water_fail_cost")) or (
            _opts.get("water_fail_cost") is not None
        )
        self._water_value_resolver: Optional[WaterValueResolver] = (
            water_value_resolver
            if water_value_resolver is not None or not _wvr_gate
            else WaterValueResolver(
                central_parser=central_parser,
                cenre_parser=cenre_parser,
                options=self.options,
            )
        )
        # Generator UIDs whose pmin was zeroed in the JSON by this
        # transform.  Used by :meth:`_drop_generator_pmin_columns` to
        # also strip the corresponding columns from
        # ``Generator/pmin.parquet`` so the per-stage trajectory does
        # not silently re-enforce a hard pmin alongside the new soft
        # FlowRight.
        self._converted_generator_uids: set[int] = set()

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
        # Uniform fallback fail_cost — used when the auto-water-fail-cost
        # resolver is inactive, AND as the floor for the waterway-fmin
        # transform which has no per-central anchor.  When the resolver
        # is active each gen-pmin FR receives its own per-central cost
        # via :meth:`_resolve_fail_cost_for`.
        fail_cost = self._resolve_fail_cost()

        gen_junctions: Dict[str, str] = {}
        if self.whitelist:
            gen_junctions = self._build_gen_junction_map(planning_system)
            if not gen_junctions:
                _logger.warning(
                    "pmin_as_flowright: no gen waterways found in planning;"
                    " whitelist transform will be skipped."
                )
        generators: List[Dict[str, Any]] = list(
            planning_system.get("generator_array", []) or []
        )
        flow_rights: List[Dict[str, Any]] = []
        rows_by_uid: Dict[int, np.ndarray] = {}
        block_index, stage_index = self._block_stage_indices()

        next_uid = _FLOW_RIGHT_UID_START
        for name in self.whitelist:
            fc = self._resolve_fail_cost_for(name, default=fail_cost)
            entry, flow_values = self._build_flow_right(
                central_name=name,
                uid=next_uid,
                gen_junctions=gen_junctions,
                generators=generators,
                fail_cost=fc,
                block_index=block_index,
            )
            if entry is None:
                continue
            flow_rights.append(entry)
            rows_by_uid[next_uid] = flow_values
            next_uid += 1

        if flow_rights:
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

            # Drop the converted generators' columns from
            # Generator/pmin.parquet so the original per-stage pmin
            # trajectory does not silently re-enforce a hard pmin
            # alongside the new soft FlowRight.
            self._drop_generator_pmin_columns()

            # ``fail_cost`` is the gtopt-internal $/(m³/s·h) coefficient;
            # the user-facing PLP-native equivalent is /3.6 ($/Hm³).
            plp_to_gtopt_units: float = 3.6
            fail_cost_plp = fail_cost / plp_to_gtopt_units
            _logger.info(
                "pmin_as_flowright: emitted %d FlowRight(s)"
                " (fail_cost = %g $/Hm³ ≡ %g $/(m³/s·h) internal)",
                len(flow_rights),
                fail_cost_plp,
                fail_cost,
            )

        # Second transform: waterway fmin → FlowRight.  Auto-detected
        # from ``Waterway/fmin.parquet`` (no whitelist; every non-zero
        # column is converted).  This catches transit centrals
        # (``bus = 0``, e.g. LMAULE, B_LaMina) whose minimum-flow
        # obligation lives on the gen waterway directly because they
        # have no Generator entity to carry it.  Without this transform
        # the hard ``waterway_flow >= fmin`` row can drive an SDDP
        # phase infeasible when the upstream reservoir is empty.
        waterway_rights = self._process_waterway_fmin(
            planning_system, fail_cost=fail_cost
        )

        return flow_rights + waterway_rights

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

    def _resolve_fail_cost_for(self, central_name: str, *, default: float) -> float:
        """Per-central FlowRight ``fail_cost`` (gtopt-internal $/(m³/s·h)).

        When the auto helper is inactive (no resolver, or resolver's
        ``is_active`` is False) we return *default* — the uniform
        legacy value resolved by :meth:`_resolve_fail_cost`.

        When the helper is active:

        * Resolve the central's number from ``central_parser``.
        * If unknown → fall back to *default* (defensive; preserves
          historical behaviour for centrals not in the parser).
        * Else use ``junction_lost_pf(number)``: the central's own
          ``max_rendi`` if it has a generator (``bus > 0``), else 0.
        * If ``lost_pf == 0`` (transit-only / bus=0 central) the FR
          has no energy-equivalent cost — return ``0.0``.
        * Otherwise scale by the anchor: ``anchor × lost_pf``.
        """
        resolver = self._water_value_resolver
        if resolver is None or not resolver.is_active:
            return default
        central_number: Optional[int] = None
        if self.central_parser is not None:
            central_number = next(
                (
                    c.get("number")
                    for c in getattr(self.central_parser, "centrals", []) or []
                    if c.get("name") == central_name
                ),
                None,
            )
        if central_number is None:
            return default
        lost_pf = resolver.junction_lost_pf(int(central_number))
        if lost_pf <= 0.0:
            # Transit-only central (bus=0) — FR has no energy-equivalent cost.
            return 0.0
        return resolver.fail_cost(lost_pf)

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
        # name (matching central_name).  Also record the generator's
        # uid so we can later drop its column from Generator/pmin.parquet
        # (otherwise the per-stage trajectory persists as dead data
        # alongside the new FlowRight obligation).
        for gen in generators:
            if isinstance(gen, dict) and gen.get("name") == central_name:
                gen["pmin"] = 0.0
                gen_uid = gen.get("uid")
                if gen_uid is not None:
                    self._converted_generator_uids.add(int(gen_uid))
                break

        column_name = f"{central_name}{_FLOW_RIGHT_SUFFIX}"
        # The gtopt C++ JSON binding uses `target` (with `discharge`
        # accepted as legacy alias) and `fcost` (no legacy alias —
        # `fail_cost` is the pre-2026-05 name and no longer parses).
        # Keep `discharge` here for compatibility with older gtopt
        # builds that haven't picked up the unified-mode refactor;
        # the alias resolves to `target` at parse time.
        entry: Dict[str, Any] = {
            "uid": uid,
            "name": column_name,
            "purpose": _FLOW_RIGHT_PURPOSE,
            "junction": junction_b,
            "direction": _FLOW_RIGHT_DIRECTION,
            "discharge": column_name,
            "fcost": fail_cost,
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

    def _drop_generator_pmin_columns(self) -> None:
        """Strip converted generators' columns from ``Generator/pmin.parquet``.

        The pmin-as-flowright transform sets ``Generator.pmin = 0.0`` in
        the JSON for each converted central, but ``Generator/pmin.parquet``
        is written by ``mance_writer`` BEFORE this transform runs.  If the
        gtopt parser ever consults the parquet for a generator whose JSON
        scalar pmin is 0, the original mance trajectory would silently
        re-enforce the hard must-run alongside the new soft FlowRight —
        defeating the entire transform.

        This cleanup pass removes the ``uid:N`` column for each converted
        generator from the parquet so the conversion is unambiguous: the
        only pmin obligation surviving is the soft FlowRight.
        """
        if not self._converted_generator_uids:
            return
        if "output_dir" not in self.options:
            return
        gen_dir = Path(self.options["output_dir"]) / "Generator"
        # File extension follows the case's output_format (default parquet
        # → ``pmin.parquet``; CSV mode → ``pmin.csv``).
        candidates = [gen_dir / "pmin.parquet", gen_dir / "pmin.csv"]
        path = next((p for p in candidates if p.exists()), None)
        if path is None:
            return

        is_parquet = path.suffix == ".parquet"
        try:
            df = pd.read_parquet(path) if is_parquet else pd.read_csv(path)
        except (OSError, ValueError) as exc:
            _logger.warning(
                "pmin_as_flowright: could not open %s for cleanup (%s); "
                "the converted generators' pmin trajectories may persist "
                "as dead data alongside the new FlowRights.",
                path,
                exc,
            )
            return

        drop_cols = [f"uid:{uid}" for uid in self._converted_generator_uids]
        present = [c for c in drop_cols if c in df.columns]
        if not present:
            return  # nothing to drop — not all converted gens had a mance schedule

        df = df.drop(columns=present)
        try:
            if is_parquet:
                df.to_parquet(path, index=False)
            else:
                write_csv(df, path)
        except (OSError, ValueError) as exc:
            _logger.warning(
                "pmin_as_flowright: could not rewrite %s after column "
                "drop (%s); the original trajectories remain on disk.",
                path,
                exc,
            )
            return
        _logger.info(
            "pmin_as_flowright: dropped %d converted-generator pmin column(s) "
            "from %s (%s)",
            len(present),
            path.name,
            ", ".join(present),
        )

    # ------------------------------------------------------------------
    # Waterway fmin → FlowRight transform
    # ------------------------------------------------------------------
    def _process_waterway_fmin(
        self,
        planning_system: Dict[str, Any],
        *,
        fail_cost: float,
    ) -> List[Dict[str, Any]]:
        """Convert hard waterway ``fmin`` floors into soft FlowRights.

        Scans the planning system's ``waterway_array`` for waterways
        whose ``fmin`` is either:

        * a parquet schedule reference (``"fmin"``) — the per-stage
          envelope lives in ``Waterway/fmin.parquet`` keyed by waterway
          uid, OR
        * a positive scalar value.

        For each such waterway:

        1. Build a per-block flow vector (m³/s).  No Rendi conversion
           — fmin is already in flow units.
        2. Append a FlowRight to ``flow_right_array`` with
           ``junction = waterway.junction_b``,
           ``direction = -1`` (consumptive — same convention as the
           pmin-as-flowright transform).
        3. Write a single-column parquet under
           ``FlowRight/<waterway_name>_fmin_as_flow_right.parquet``.
        4. Zero the waterway's ``fmin`` so the LP no longer enforces
           the hard floor.
        5. Drop the column from ``Waterway/fmin.parquet`` so the
           per-stage trajectory does not silently re-enforce a hard
           fmin alongside the new soft FlowRight.

        Returns the list of emitted FlowRight entries.  When no
        waterway carries a non-zero fmin, returns ``[]`` and the
        parquet file is left untouched.
        """
        waterways = planning_system.get("waterway_array", []) or []
        if not waterways:
            return []
        block_index, stage_index = self._block_stage_indices()
        if block_index.size == 0:
            return []

        # Read the parquet column for waterway uids whose ``fmin`` is
        # a string ref.  Centrals with scalar fmin are handled inline.
        fmin_parquet_path: Optional[Path] = None
        fmin_df: Optional[pd.DataFrame] = None
        if "output_dir" in self.options:
            cand = Path(self.options["output_dir"]) / "Waterway" / "fmin.parquet"
            if cand.exists():
                fmin_parquet_path = cand
                try:
                    fmin_df = pd.read_parquet(cand)
                except (OSError, ValueError) as exc:
                    _logger.warning(
                        "soft_min_flows (waterway): could not read %s (%s); "
                        "waterway-fmin → FlowRight conversion skipped.",
                        cand,
                        exc,
                    )
                    return []

        flow_rights: List[Dict[str, Any]] = []
        rows_by_uid: Dict[int, np.ndarray] = {}
        next_uid = _WATERWAY_FLOW_RIGHT_UID_START
        dropped_uid_cols: List[str] = []
        zeroed_waterways: List[str] = []

        for ww in waterways:
            if not isinstance(ww, dict):
                continue
            ww_uid = ww.get("uid")
            ww_name = ww.get("name")
            junction_b = ww.get("junction_b")
            fmin = ww.get("fmin")
            if ww_uid is None or not isinstance(ww_name, str):
                continue
            if not isinstance(junction_b, str) or not junction_b:
                continue

            flow_values: Optional[np.ndarray] = None
            uid_col = f"uid:{ww_uid}"

            if isinstance(fmin, str):
                # Parquet schedule ref — pull the column matching this
                # waterway uid.
                if fmin_df is None or uid_col not in fmin_df.columns:
                    continue
                col = fmin_df[uid_col].to_numpy(dtype=np.float64, copy=True)
                if col.size != block_index.size:
                    # Unexpected shape — skip this waterway rather than
                    # mis-broadcast.
                    _logger.warning(
                        "soft_min_flows (waterway): %s column '%s' has %d "
                        "rows but the case has %d block-rows; skipping.",
                        fmin_parquet_path,
                        uid_col,
                        col.size,
                        block_index.size,
                    )
                    continue
                if not (col > 0).any():
                    # All-zero column: nothing to convert; just drop the
                    # parquet ref so the JSON does not carry a dangling
                    # filename reference.
                    ww["fmin"] = 0.0
                    dropped_uid_cols.append(uid_col)
                    continue
                flow_values = col
            elif isinstance(fmin, (int, float)) and float(fmin) > 0.0:
                flow_values = np.full(block_index.size, float(fmin), dtype=np.float64)
            else:
                continue

            entry: Dict[str, Any] = {
                "uid": next_uid,
                "name": f"{ww_name}{_WATERWAY_FLOW_RIGHT_SUFFIX}",
                "purpose": _FLOW_RIGHT_PURPOSE,
                "junction": junction_b,
                "direction": _FLOW_RIGHT_DIRECTION,
                "discharge": f"{ww_name}{_WATERWAY_FLOW_RIGHT_SUFFIX}",
                "fcost": fail_cost,
            }
            flow_rights.append(entry)
            rows_by_uid[next_uid] = flow_values
            next_uid += 1

            # Disable the hard floor — the FlowRight slack now carries
            # the obligation.
            ww["fmin"] = 0.0
            zeroed_waterways.append(ww_name)
            if isinstance(fmin, str):
                dropped_uid_cols.append(uid_col)

        if not flow_rights:
            return []

        fr_array = planning_system.setdefault("flow_right_array", [])
        if not isinstance(fr_array, list):
            raise ValueError("planning system 'flow_right_array' must be a list")
        fr_array.extend(flow_rights)

        self._write_parquets(flow_rights, rows_by_uid, block_index, stage_index)

        # Drop converted columns from Waterway/fmin.parquet so the
        # original trajectory cannot silently re-enforce the hard
        # floor alongside the new soft FlowRight.
        if fmin_parquet_path is not None and fmin_df is not None and dropped_uid_cols:
            present = [c for c in dropped_uid_cols if c in fmin_df.columns]
            if present:
                survivors = fmin_df.drop(columns=present)
                # If only schema columns (block, stage) remain, remove
                # the file entirely so the JSON ref points to nothing.
                payload_cols = [c for c in survivors.columns if c.startswith("uid:")]
                try:
                    if not payload_cols:
                        fmin_parquet_path.unlink()
                    else:
                        survivors.to_parquet(fmin_parquet_path, index=False)
                except (OSError, ValueError) as exc:
                    _logger.warning(
                        "soft_min_flows (waterway): could not rewrite %s "
                        "after column drop (%s); the original trajectories "
                        "remain on disk.",
                        fmin_parquet_path,
                        exc,
                    )

        _logger.info(
            "soft_min_flows (waterway): emitted %d FlowRight(s) for "
            "waterway-fmin floors on %s (fail_cost = %g $/(m³/s·h))",
            len(flow_rights),
            ", ".join(zeroed_waterways),
            fail_cost,
        )
        return flow_rights

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
            # FlowRight.{fmin,fmax,target,discharge} are now
            # OptTBRealFieldSched (Stage × Block — scenario-independent).
            # Emit one row per (stage, block); do NOT replicate per
            # scenario.  Replicating would create duplicate (stage,
            # block) keys for the 2D reader and the C++ side now
            # throws on duplicate UID keys.
            df = pd.DataFrame(
                {
                    "block": block_index,
                    f"uid:{uid}": values,
                    "stage": stage_index,
                }
            )
            self.write_dataframe(df, output_dir, stem)
