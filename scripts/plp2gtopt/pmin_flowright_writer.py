# -*- coding: utf-8 -*-

"""Soft hydro ``pmin`` + waterway-``fmin`` → FlowRight transforms.

This module owns two related transforms applied late in the
``plp2gtopt`` pipeline, both aimed at preventing a hard minimum-flow /
must-run floor from driving an SDDP phase infeasible when the upstream
reservoir is empty:

1. **Soft hydro pmin** (see :meth:`PminFlowRightWriter.process`).  Every
   hydro generator (turbine-linked) carrying ``pmin > 0`` KEEPS its
   ``pmin`` but gains a ``pmin_fcost`` equal to the water-value anchor
   [$/MWh].  The floor becomes a *soft* obligation: a water-short river
   pays the anchor for the shortfall instead of being hard-forced.  No
   FlowRight is emitted for generator pmin — this replaces the legacy
   pmin → FlowRight conversion.

2. **Waterway fmin → FlowRight** (see
   :meth:`PminFlowRightWriter._process_waterway_fmin`).  Transit
   centrals (``bus = 0``) carry their minimum-flow obligation directly
   on the gen waterway's ``fmin``.  Each non-zero ``fmin`` is converted
   into a soft FlowRight (slack-backed) and the hard floor is zeroed.
   The per-FlowRight discharge parquet is written under
   ``<output_dir>/FlowRight/`` so PLP-derived cases stay self-consistent.

FlowRight is therefore reserved for irrigation / transit water rights.
Waterway-fmin FlowRight UIDs start at
:data:`_WATERWAY_FLOW_RIGHT_UID_START` (5500).
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
import pandas as pd

from gtopt_shared.water_values import WaterValueResolver

from .base_writer import BaseWriter

_logger = logging.getLogger(__name__)

#: Suffix appended to the waterway name for the FlowRight name and
#: parquet stem produced by the waterway-fmin → FlowRight transform.
_WATERWAY_FLOW_RIGHT_SUFFIX: str = "_fmin_as_flow_right"

#: First UID assigned to FlowRights emitted by the waterway-fmin path.
_WATERWAY_FLOW_RIGHT_UID_START: int = 5500

#: ``purpose`` field used on the emitted FlowRight (metadata only).
_FLOW_RIGHT_PURPOSE: str = "environmental"

#: ``direction`` for a consumptive (downstream) flow obligation.
_FLOW_RIGHT_DIRECTION: int = -1

#: Fallback ``fail_cost`` [$/m³/s·h] used when no rebalse cost is
#: available from ``plpvrebemb.dat`` and ``plpmat.dat`` ``CVert`` is
#: zero/missing.  Chosen high so the slack is the path of last resort.
_DEFAULT_FAIL_COST: float = 10000.0

#: Truthy values for the ``enabled`` column of the whitelist CSV.
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})

#: PLP-to-gtopt fail-cost unit conversion factor.  PLP's ``CCaudFalla`` is
#: scaled internally by ``FactTiempoH = 3.6`` (CEN65/src ``leemat.f:118``)
#: before the objective coefficient is built in ``genpdago.f:101`` as
#: ``CCaudFalla_scaled × BloDur / FPhi``.  gtopt's ``FlowRight.fail_cost`` is
#: $/(m³/s·h), so the equivalent gtopt value for any PLP-native $/Hm³
#: quantity is ``value × 3.6``.
_PLP_TO_GTOPT_FAIL_COST: float = 3.6


def resolve_flow_right_fail_cost(
    options: Optional[Dict[str, Any]],
    plpmat_parser: Optional[Any],
    vrebemb_parser: Optional[Any],
) -> float:
    """Resolve the FlowRight ``fail_cost`` in gtopt's $/(m³/s·h).

    Shared by the waterway-fmin transform
    (:meth:`PminFlowRightWriter._resolve_fail_cost`) and the
    irrigation-diversion FlowRights emitted by ``JunctionWriter`` so both
    pick the SAME number — no new value is invented.  Resolution order
    (every PLP-native $/Hm³ value is multiplied by
    :data:`_PLP_TO_GTOPT_FAIL_COST` before being returned):

    0. CLI override ``--flow-right-fail-cost`` ($/Hm³).
    1. ``plpmat.dat`` ``CCauFal`` flow-failure cost ($/Hm³).
    2. ``2 × max(rebalse_cost)`` over plpvrebemb.dat entries ($/Hm³).
    3. ``2 × CVert`` plpmat default vert cost ($/Hm³).
    4. :data:`_DEFAULT_FAIL_COST` (already in gtopt units — no conversion).
    """
    opts = options or {}

    override = opts.get("flow_right_fail_cost")
    if override is not None:
        return float(override) * _PLP_TO_GTOPT_FAIL_COST

    ccaufal = 0.0
    if plpmat_parser is not None:
        ccaufal = float(getattr(plpmat_parser, "flow_fail_cost", 0.0) or 0.0)
    if ccaufal > 0.0:
        return ccaufal * _PLP_TO_GTOPT_FAIL_COST

    max_rebalse = 0.0
    if vrebemb_parser is not None:
        for entry in vrebemb_parser.get_all() or []:
            cost = float(entry.get("cost", 0.0) or 0.0)
            max_rebalse = max(max_rebalse, cost)
    if max_rebalse > 0.0:
        return 2.0 * max_rebalse * _PLP_TO_GTOPT_FAIL_COST

    cvert = 0.0
    if plpmat_parser is not None:
        cvert = float(getattr(plpmat_parser, "vert_cost", 0.0) or 0.0)
    if cvert > 0.0:
        return 2.0 * cvert * _PLP_TO_GTOPT_FAIL_COST

    return _DEFAULT_FAIL_COST


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

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def process(
        self,
        planning_system: Dict[str, Any],
    ) -> List[Dict[str, Any]]:
        """Mutate *planning_system* in place; return the FlowRight entries.

        Steps:

        1. **Soft hydro pmin**: for every hydro generator (turbine-linked)
           carrying ``pmin > 0``, set ``pmin_fcost`` to the water-value
           anchor [$/MWh] and KEEP ``pmin`` as a soft floor.  No FlowRight
           is emitted for generator pmin.
        2. **Waterway fmin → FlowRight**: convert each non-zero waterway
           ``fmin`` floor into a soft FlowRight and zero the hard floor
           (see :meth:`_process_waterway_fmin`).

        Returns the waterway-fmin FlowRight entries (the soft-pmin step
        emits no FlowRights).
        """
        # Uniform fallback fail_cost — used when the auto-water-fail-cost
        # resolver is inactive, AND as the floor for the waterway-fmin
        # transform which has no per-central anchor.
        fail_cost = self._resolve_fail_cost()

        generators: List[Dict[str, Any]] = list(
            planning_system.get("generator_array", []) or []
        )

        # ---- Soft hydro pmin -------------------------------------------------
        # Every hydro generator (turbine-linked) that carries a pmin keeps it
        # as a SOFT floor: when the river is water-short the shortfall is
        # penalised at the hydro water value rather than hard-forced (which
        # would drive the SDDP phase infeasible).  This REPLACES the legacy
        # pmin->FlowRight conversion for generator units — FlowRight is now
        # reserved for irrigation / water rights.  The penalty is the
        # water-value anchor [$/MWh]: the flow-based FlowRight ``fcost`` was
        # ``anchor x PF``; expressed per-MWh on the generator the PF cancels,
        # leaving the anchor.  ``pmin`` itself is kept untouched (soft floor).
        hydro_gen_names = {
            str(t.get("generator") or t.get("name"))
            for t in (planning_system.get("turbine_array", []) or [])
            if isinstance(t, dict)
        }
        anchor = (
            float(self._water_value_resolver.anchor)
            if self._water_value_resolver is not None
            else 0.0
        )
        soft_count = 0
        if anchor > 0.0:
            for gen in generators:
                if not isinstance(gen, dict) or gen.get("name") not in hydro_gen_names:
                    continue
                pmin = gen.get("pmin")
                has_pmin = (
                    isinstance(pmin, (int, float)) and pmin > 0.0
                ) or isinstance(pmin, str)
                if not has_pmin:
                    continue
                gen["pmin_fcost"] = anchor
                soft_count += 1
            if soft_count:
                planning_system["generator_array"] = generators
                _logger.info(
                    "soft hydro pmin: pmin_fcost=%g $/MWh on %d hydro"
                    " generator(s) (water-value anchor; pmin kept as soft"
                    " floor, no FlowRight)",
                    anchor,
                    soft_count,
                )

        # Second transform: waterway fmin → FlowRight (see module docstring).
        # Auto-detected from ``Waterway/fmin.parquet`` — catches transit
        # centrals (``bus = 0``, e.g. LMAULE, B_LaMina) whose minimum-flow
        # obligation lives on the gen waterway because they have no
        # Generator entity to carry it.
        waterway_rights = self._process_waterway_fmin(
            planning_system, fail_cost=fail_cost
        )

        return waterway_rights

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
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

        Delegates to :func:`resolve_flow_right_fail_cost` so the
        irrigation-diversion FlowRights emitted by ``JunctionWriter`` pick
        the identical number.
        """
        return resolve_flow_right_fail_cost(
            self.options, self.plpmat_parser, self.vrebemb_parser
        )

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
                "junction_a": junction_b,
                "direction": _FLOW_RIGHT_DIRECTION,
                # Canonical `target` key (parquet column reference); the
                # binding still accepts the legacy `discharge` alias.
                "target": f"{ww_name}{_WATERWAY_FLOW_RIGHT_SUFFIX}",
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
            stem = str(entry["target"])
            values = rows_by_uid.get(uid)
            if values is None or values.size == 0:
                continue
            # FlowRight.{fmin,fmax,target} are now
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
