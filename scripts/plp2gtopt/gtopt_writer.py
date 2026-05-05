# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.

Domain methods are split across mixin modules to keep this file
focused on orchestration:

* :mod:`._writer_time` — stages, scenarios, apertures, indhor
* :mod:`._writer_generation` — central, generator-profile, falla,
  pasada classification
* :mod:`._writer_hydro` — RoR, afluents, junctions, water rights, LNG,
  pumped storage, flow-turbine, pmin-flowright
* :mod:`._writer_network` — buses, lines, demands, batteries
* :mod:`._writer_boundary` — boundary cuts and variable scales
"""

import json
import logging
from pathlib import Path
from typing import Any, Dict

from ._writer_boundary import BoundaryMixin
from ._writer_generation import GenerationMixin
from ._writer_hydro import HydroMixin
from ._writer_network import NetworkMixin
from ._writer_time import TimeMixin
from .line_parser import LineParser
from .plp_parser import PLPParser

_logger = logging.getLogger(__name__)


def _strip_internal_keys(planning: Dict) -> Dict:
    """Return a shallow copy of ``planning`` with internal-only keys removed.

    The gtopt C++ parser uses ``StrictParsePolicy`` (daw::json
    ``UseExactMappingsByDefault=yes``), so any field not declared in the
    corresponding struct causes a parse error.  Python-side metadata
    (pipeline annotations, Excel hints) is preserved on the writer
    instance but excluded from the emitted JSON.
    """
    return {k: v for k, v in planning.items() if not k.startswith("_")}


def _try_scalar(value: Any) -> float | None:
    """Extract a scalar float from a JSON value if possible.

    Mirrors the C++ side's ``try_scalar_value`` (validate_planning.cpp):
    a scalar Real / int collapses to ``float``; vector / file / string
    schedules return ``None`` (validation deferred to load time).
    """
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    return None


def _find_by_uid_or_name(arr: list[Dict], ref: Any) -> Dict | None:
    """Look up an element in ``arr`` by Uid (int) or Name (str)."""
    if isinstance(ref, int):
        for elem in arr:
            if elem.get("uid") == ref:
                return elem
        return None
    if isinstance(ref, str):
        for elem in arr:
            if elem.get("name") == ref:
                return elem
    return None


def _validate_piecewise_segments(planning: Dict) -> list[str]:
    """Per-segment range feasibility for seepage + discharge_limit.

    Mirrors the C++ ``check_piecewise_feasibility`` in
    ``source/validate_planning.cpp``.  For each piecewise segment k of a
    seepage / discharge_limit element, evaluates the linear function
    ``f(efin) = constant + slope · efin`` at the segment's active
    range ``[V_low, V_high]`` (clipped to the reservoir's
    ``[emin, emax]`` envelope) and returns a warning string when the
    resulting range violates the LP-row's flow bound:

    * ReservoirSeepage row is an equality
      ``qfilt = constant + slope · efin`` with qfilt bounded by the
      seepage's WATERWAY ``[fmin, fmax]``.  Warn when
      ``min(f(V_low), f(V_high)) < fmin``  OR
      ``max(f(V_low), f(V_high)) > fmax``.

      Also warn when the **first segment** evaluated at ``efin = emin``
      gives a non-zero qfilt (tolerance 1e-3 m³/s).  PLP filtration
      curves are physically expected to drop to zero at the lower
      operating volume; ``junction_writer._fix_first_seepage_segment``
      anchors q(vmin)=0 by default, so a violation here means the
      input bypassed that fix (``--plp-legacy``, single-segment
      curves, or hand-edited JSON) — the LP will be forced to
      discharge water that isn't physically in storage near vmin.

    * ReservoirDischargeLimit row is the inequality
      ``qeh ≤ intercept + slope · efin`` with ``qeh ≥ 0``.  Warn when
      ``min(f(V_low), f(V_high)) < 0``.

    Schedule-form ``emin`` / ``emax`` / ``fmin`` / ``fmax`` are
    skipped (they need per-stage resolution at LP-build time, not at
    static validation).  Caller logs the returned warnings — does not
    raise — to match the C++ side's "warn-only" semantics.
    """
    warnings: list[str] = []
    system = planning.get("system", {})
    reservoirs = system.get("reservoir_array", [])
    waterways = system.get("waterway_array", [])
    seepages = system.get("reservoir_seepage_array", [])
    discharge_limits = system.get("reservoir_discharge_limit_array", [])

    # ── Seepage ────────────────────────────────────────────────
    for seep in seepages:
        segments = seep.get("segments") or []
        if not segments:
            continue
        rsv = _find_by_uid_or_name(reservoirs, seep.get("reservoir"))
        if rsv is None:
            continue
        emin = _try_scalar(rsv.get("emin"))
        emax = _try_scalar(rsv.get("emax"))
        if emin is None or emax is None:
            continue  # schedule form — defer
        ww = _find_by_uid_or_name(waterways, seep.get("waterway"))
        if ww is None:
            continue
        fmin_val = _try_scalar(ww.get("fmin"))
        fmax_val = _try_scalar(ww.get("fmax"))
        fmin_val = 0.0 if fmin_val is None else fmin_val
        fmax_val = float("inf") if fmax_val is None else fmax_val

        # First-segment q(emin) physical-zero check + auto-fix.  Mirrors
        # `junction_writer._fix_first_seepage_segment` (which runs on
        # the way OUT of plp2gtopt for the standard path) so cases that
        # bypassed it — hand-edited JSON, --plp-legacy, single-segment
        # curves — still get anchored at q(emin)=0 with the same
        # 2-point algorithm: anchor (emin, 0) plus continuity at
        # segment-2's start volume.  A warning fires unconditionally
        # so the operator sees the (auto-)correction in the log.
        first = segments[0]
        first_slope = float(first.get("slope", 0.0))
        first_constant = float(first.get("constant", 0.0))
        q_at_emin = first_constant + first_slope * emin
        if abs(q_at_emin) > 1e-3:
            seep_name = seep.get("name")
            rsv_name = rsv.get("name")
            ww_name = ww.get("name")
            if len(segments) >= 2:
                seg2_vol = float(segments[1].get("volume", 0.0))
            else:
                seg2_vol = float("nan")
            if len(segments) >= 2 and seg2_vol > emin:
                # Anchor: q(emin)=0 and q(seg2_vol) preserved
                # (continuity with the rest of the curve).
                q_at_seg2 = first_constant + first_slope * seg2_vol
                new_slope = q_at_seg2 / (seg2_vol - emin)
                new_constant = -new_slope * emin
                # In-place mutation of the planning dict — caller's
                # reference sees the corrected coefficients.
                segments[0]["slope"] = new_slope
                segments[0]["constant"] = new_constant
                warnings.append(
                    f"ReservoirSeepage '{seep_name}' (reservoir "
                    f"'{rsv_name}', waterway '{ww_name}'): first "
                    f"segment qfilt(efin=emin={emin:.4g}) = "
                    f"{q_at_emin:.4g} → 0 (auto-anchored: slope "
                    f"{first_slope:.6g}→{new_slope:.6g}, constant "
                    f"{first_constant:.6g}→{new_constant:.6g}, "
                    f"continuity at vol={seg2_vol:.4g} with "
                    f"q={q_at_seg2:.4g})."
                )
            else:
                # Single segment, or seg2 starts at/below emin —
                # cannot anchor cleanly without losing physical
                # meaning; warn only.
                reason = (
                    "single segment"
                    if len(segments) < 2
                    else f"second segment vol={seg2_vol:.4g} ≤ emin={emin:.4g}"
                )
                warnings.append(
                    f"ReservoirSeepage '{seep_name}' (reservoir "
                    f"'{rsv_name}', waterway '{ww_name}'): first "
                    f"segment qfilt(efin=emin={emin:.4g}) = "
                    f"{q_at_emin:.4g} (slope={first_slope:.6g}, "
                    f"constant={first_constant:.6g}); cannot "
                    f"auto-anchor ({reason}) — fix the segment "
                    f"data manually so that constant + slope * emin "
                    f"= 0."
                )

        for k, seg in enumerate(segments):
            slope = float(seg.get("slope", 0.0))
            constant = float(seg.get("constant", 0.0))
            seg_lo = float(seg.get("volume", 0.0))
            v_low = max(seg_lo, emin)
            v_high = (
                float(segments[k + 1].get("volume", 0.0))
                if k + 1 < len(segments)
                else emax
            )
            if v_high < v_low:
                continue  # empty range
            f_low = constant + slope * v_low
            f_high = constant + slope * v_high
            f_min = min(f_low, f_high)
            f_max = max(f_low, f_high)
            if f_min < fmin_val:
                warnings.append(
                    f"ReservoirSeepage '{seep.get('name')}' "
                    f"(reservoir '{rsv.get('name')}', waterway "
                    f"'{ww.get('name')}'): segment {k} "
                    f"({v_low:.3g} ≤ efin ≤ {v_high:.3g}) produces qfilt "
                    f"below waterway fmin (slope={slope:.6g}, "
                    f"constant={constant:.6g} → qfilt range "
                    f"[{f_min:.6g}, {f_max:.6g}] vs fmin={fmin_val:.6g}); "
                    "the LP will go primal-infeasible whenever efin lands "
                    "in the segment's lower portion.  Adjust the segment "
                    "data so that `constant + slope * V >= fmin` for all "
                    "V in [V_low, V_high]."
                )
            if f_max > fmax_val:
                warnings.append(
                    f"ReservoirSeepage '{seep.get('name')}' "
                    f"(reservoir '{rsv.get('name')}', waterway "
                    f"'{ww.get('name')}'): segment {k} "
                    f"({v_low:.3g} ≤ efin ≤ {v_high:.3g}) produces qfilt "
                    f"above waterway fmax (slope={slope:.6g}, "
                    f"constant={constant:.6g} → qfilt range "
                    f"[{f_min:.6g}, {f_max:.6g}] vs fmax={fmax_val:.6g})."
                )

    # ── DischargeLimit ─────────────────────────────────────────
    for ddl in discharge_limits:
        segments = ddl.get("segments") or []
        if not segments:
            continue
        rsv = _find_by_uid_or_name(reservoirs, ddl.get("reservoir"))
        if rsv is None:
            continue
        emin = _try_scalar(rsv.get("emin"))
        emax = _try_scalar(rsv.get("emax"))
        if emin is None or emax is None:
            continue

        for k, seg in enumerate(segments):
            slope = float(seg.get("slope", 0.0))
            intercept = float(seg.get("intercept", 0.0))
            seg_lo = float(seg.get("volume", 0.0))
            v_low = max(seg_lo, emin)
            v_high = (
                float(segments[k + 1].get("volume", 0.0))
                if k + 1 < len(segments)
                else emax
            )
            if v_high < v_low:
                continue
            f_low = intercept + slope * v_low
            f_high = intercept + slope * v_high
            f_min = min(f_low, f_high)
            f_max = max(f_low, f_high)
            if f_min < 0.0:
                warnings.append(
                    f"ReservoirDischargeLimit '{ddl.get('name')}' "
                    f"(reservoir '{rsv.get('name')}'): segment {k} "
                    f"({v_low:.3g} ≤ efin ≤ {v_high:.3g}) produces a "
                    f"negative discharge upper bound "
                    f"(slope={slope:.6g}, intercept={intercept:.6g} → "
                    f"bound range [{f_min:.6g}, {f_max:.6g}]); the LP "
                    "will go primal-infeasible whenever efin lands in "
                    "the segment's lower portion.  Adjust the segment so "
                    "that `intercept + slope * V >= 0` for all V in "
                    "[V_low, V_high]."
                )

    return warnings


class GTOptWriter(
    TimeMixin,
    GenerationMixin,
    HydroMixin,
    NetworkMixin,
    BoundaryMixin,
):
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: PLPParser, options=None):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.options = options
        self.output_path = None

        self.planning: Dict[str, Dict[str, Any]] = {
            "options": {},
            "system": {},
            "simulation": {},
        }

    @staticmethod
    def _normalize_method(method: str) -> str:
        """Normalize solver type string.

        Accepts 'sddp', 'mono', 'monolithic', or 'cascade'; returns
        'sddp', 'monolithic', or 'cascade'.
        """
        if method in ("mono", "monolithic"):
            return "monolithic"
        if method == "cascade":
            return "cascade"
        return "sddp"

    @staticmethod
    def _build_default_cascade_options(
        model_opts: dict[str, Any],
        sddp_opts: dict[str, Any],
    ) -> dict[str, Any]:
        """Build a 3-level default cascade configuration.

        Iteration budget split:
          - Level 0 (uninodal):  full max_iterations — single-bus relaxation
            runs against the PLP-provided iteration budget unmodified
          - Level 1 (transport): 1/4 of max_iterations — lines enabled, no
            losses, no kirchhoff (pure transport model)
          - Level 2 (full):      1/4 of max_iterations — full network with the
            user's original model_options

        Each level inherits state-variable targets from the previous level
        via elastic constraints (``inherit_targets = -1``).
        """
        total_iter = sddp_opts.get("max_iterations", 100)
        convergence_tol = sddp_opts.get("convergence_tol", 0.01)

        l0_iter = max(total_iter, 1)
        l1_iter = max(total_iter // 4, 1)
        l2_iter = max(total_iter // 4, 1)

        # The cascade-level max_iterations is interpreted by
        # CascadePlanningMethod as a GLOBAL budget applied to the sum of
        # every level's training iterations.  Leaving it equal to
        # total_iter lets level 0 alone consume the whole budget (since
        # l0_iter == total_iter) and skip levels 1–2 — see
        # cascade_method.cpp: "global iteration budget exhausted".  Use
        # the sum of per-level budgets so each level still runs, while
        # keeping a conservative upper bound on total work.
        cascade_sddp_opts = {**sddp_opts, "max_iterations": l0_iter + l1_iter + l2_iter}

        transition = {
            "inherit_targets": -1,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500.0,
        }

        level_array = [
            {
                "uid": 1,
                "name": "uninodal",
                "model_options": {
                    "use_single_bus": True,
                },
                "sddp_options": {
                    "max_iterations": l0_iter,
                    "convergence_tol": convergence_tol,
                },
            },
            {
                "uid": 2,
                "name": "transport",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": False,
                    "use_line_losses": False,
                },
                "sddp_options": {
                    "max_iterations": l1_iter,
                    "convergence_tol": convergence_tol,
                },
                "transition": transition,
            },
            {
                "uid": 3,
                "name": "full_network",
                "model_options": {
                    k: v
                    for k, v in model_opts.items()
                    if k
                    in (
                        "use_single_bus",
                        "use_kirchhoff",
                        "use_line_losses",
                        "kirchhoff_threshold",
                        "loss_segments",
                        # PLP-faithful per-stage emin (PLP's `ve<u>` is Free
                        # mid-stage; only future-volume `vf<u>` carries the
                        # `vmin` lower bound).  Without this in the cascade
                        # filter, the C++ default (true since 3581a80e —
                        # strict-floor on `reservoir_sini` and last-block
                        # `efin`) kicks in and breaks SDDP convergence on
                        # cases with hard `efin >= eini` rows like
                        # plp_case_2y / juan/IPLP_uninodal.
                        "strict_storage_emin",
                    )
                },
                "sddp_options": {
                    "max_iterations": l2_iter,
                    "convergence_tol": convergence_tol,
                },
                "transition": transition,
            },
        ]

        return {
            "model_options": model_opts,
            "sddp_options": cascade_sddp_opts,
            "level_array": level_array,
        }

    def process_options(self, options):
        """Process options data to include input and output paths.

        The solver type is emitted at the top level as ``method`` so that
        the gtopt C++ JSON parser maps it directly to ``PlanningOptions::method``.
        All other SDDP-specific settings are still grouped under the nested
        ``sddp_options`` key.
        """
        if not options:
            options = {}
        discount_rate = options.get("discount_rate", 0.0)
        output_format = options.get("output_format", "parquet")
        input_format = options.get("input_format", output_format)
        compression = options.get("compression", "zstd")
        method = self._normalize_method(options.get("method", "sddp"))

        # Build the nested sddp_options block (all sddp_* fields except method).
        # NOTE: num_apertures is NOT emitted here — the C++ SddpOptions JSON
        # contract has no "num_apertures" field (only "apertures", an array of
        # UIDs).  Aperture configuration is fully handled by aperture_array and
        # per-phase apertures in the simulation section, plus
        # aperture_directory in sddp_options (all set by process_apertures).
        sddp_opts: dict = {}

        cut_sharing_mode = options.get("cut_sharing_mode")
        if cut_sharing_mode is not None:
            sddp_opts["cut_sharing_mode"] = cut_sharing_mode

        # elastic_mode controls how the forward-pass elastic filter emits
        # feasibility cuts when a subproblem is infeasible:
        #   - "single_cut" (gtopt C++ default): one aggregated π-weighted
        #     Benders cut touching every relaxed state variable.  Tight LP
        #     but any numeric drift at the box edge makes the cut
        #     hard-infeasible (observed on juan/gtopt_iplp p2).
        #   - "multi_cut" (PLP convention, plp2gtopt default): one per-link
        #     Birge-Louveaux cut per relaxed state, each clamped to its own
        #     box — matches PLP's `plp-agrespd.f::AgrElastici` +
        #     `osi_lp_get_feasible_cut` path.
        # plp2gtopt always emits multi_cut so gtopt runs produced from PLP
        # cases behave like PLP by default; users can still override via
        # `--set sddp_options.elastic_mode=single_cut` on the CLI.
        # Note: the JSON key is `elastic_mode` (mapped to internal
        # `SDDPOptions.elastic_filter_mode` in `planning_method.cpp`).
        sddp_opts["elastic_mode"] = options.get("elastic_mode", "multi_cut")

        max_iter = options.get("max_iterations")
        if max_iter is None:
            # Fall back to PDMaxIte from plpmat.dat if available.
            # PDMaxIte=1 means monolithic (single LP solve) in PLP, so only
            # use values > 1 as SDDP iteration limits.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "max_iterations", 0) > 1:
                    max_iter = plpmat.max_iterations
        if max_iter is not None:
            sddp_opts["max_iterations"] = max_iter

        convergence_tol = options.get("convergence_tol")
        if convergence_tol is None:
            # Fall back to PDError from plpmat.dat verbatim; use 0.01 if absent.
            # Emit the same numeric value PLP stores — no unit conversion —
            # so users can reason about a single "PDError / convergence_tol"
            # number rather than tracking a /100 translation.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "pd_error", 0.0) > 0.0:
                    convergence_tol = plpmat.pd_error
            if convergence_tol is None:
                convergence_tol = 0.01
        sddp_opts["convergence_tol"] = convergence_tol

        # Secondary convergence knobs (stationary_tol, stationary_window,
        # stationary_gap_ceiling, convergence_confidence, min_iterations)
        # are *not* emitted by default — gtopt now ships a coherent set of
        # defaults (1 % gap target, 0.5 % stationary tol, 5 % gap ceiling,
        # CI test disabled, 3-iter bootstrap) that match what plp2gtopt
        # used to override field-by-field.  Suppressing the default emits
        # keeps the JSON small and lets a future gtopt default change
        # propagate without re-running plp2gtopt.  Pass --stationary-tol /
        # --stationary-window / --stationary-gap-ceiling /
        # --convergence-confidence / --min-iterations on the CLI to
        # override on a case-by-case basis (each survives unchanged here).
        for key in (
            "stationary_tol",
            "stationary_window",
            "stationary_gap_ceiling",
            "convergence_confidence",
            "min_iterations",
        ):
            if key in options and options[key] is not None:
                sddp_opts[key] = options[key]

        # Cut coefficient tolerance (PLP OptiEPS equivalent).
        # cut_coeff_eps: drop coefficients with |value| < eps (default 1e-8).
        sddp_opts["cut_coeff_eps"] = options.get("cut_coeff_eps", 1e-8)

        # Pin the SDDP backward-pass solver to a single thread.  Per-stage
        # backward LPs are small (hundreds-to-thousands of vars) and the
        # parallel-simplex / parallel-barrier overhead in CPLEX/HiGHS hurts
        # more than it helps; one-thread-per-LP also avoids oversubscribing
        # the host when the SDDP scheduler already runs many backward LPs
        # concurrently across (scene × phase) cells.  C++ default is 2; this
        # block keeps the JSON intentional and CLI-overridable via
        # `--set sddp_options.backward_solver_options.threads=N`.
        sddp_opts["backward_solver_options"] = {
            "threads": options.get("backward_solver_threads", 1),
        }

        # When the JSON file lives inside the output directory (the default),
        # input_directory is "." so paths are relative to the JSON location.
        # When -f places the JSON elsewhere, use the full output_dir path.
        output_dir = Path(options.get("output_dir", ""))
        output_file = Path(options.get("output_file", ""))
        if output_file.parent == output_dir:
            input_dir_val = "."
        else:
            input_dir_val = str(output_dir)

        src_model = options.get("model_options", {})

        # PLP parity: the curtailment cost is applied PER-DEMAND via each
        # real demand's ``fcost`` field (set from falla_by_bus below), so
        # the global ``model_options.demand_fail_cost`` is essentially a
        # fallback for demands without an explicit fcost.  We default it
        # to 0 to preserve historical behaviour; the user may safely raise
        # it (``--demand-fail-cost N``) without distorting battery
        # dispatch, because gtopt's C++ ``System::expand_batteries`` now
        # pins fcost=0 explicitly on every synthetic battery-charge demand
        # (see ``source/system.cpp`` — the synthetic demand is no longer
        # sensitive to this global default).
        user_demand_fail = src_model.get("demand_fail_cost")
        effective_demand_fail = (
            user_demand_fail if user_demand_fail is not None else 0.0
        )

        # Auto-promote to single-bus when the parsed PLP case has 0
        # transmission lines.  Multi-bus mode with 0 lines makes every bus an
        # isolated island, and any bus carrying must-run thermal pmin > local
        # demand cap is structurally infeasible (no transmission to dispatch
        # the excess elsewhere).  Explicit ``-b`` / ``--use-single-bus`` still
        # forces True; setting ``use_single_bus`` in the conf or JSON
        # overrides the auto-detect either way.  When the parser is a mock or
        # the line count cannot be determined, fall back to False to preserve
        # historical behaviour.
        user_single_bus = src_model.get("use_single_bus")
        if user_single_bus is None:
            line_count = -1  # unknown
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                lp_obj = parsed.get("line_parser")
                if isinstance(lp_obj, LineParser):
                    line_count = lp_obj.num_lines
                elif isinstance(lp_obj, list):
                    line_count = len(lp_obj)
            if line_count == 0:
                _logger.info(
                    "auto-promoting model_options.use_single_bus=true: "
                    "0 transmission lines parsed (multi-bus mode would "
                    "create isolated islands)"
                )
                effective_single_bus = True
            else:
                effective_single_bus = False
        else:
            effective_single_bus = user_single_bus

        # PLP-faithful per-stage emin enforcement: emit strict_storage_emin=false
        # explicitly so the gtopt C++ default (true since 2026-04-26) does not
        # turn the per-stage emin floor into a HARD constraint on
        # reservoir_sini and the last-block efin column.  PLP's per-stage LP
        # treats `ve<u>` as Free mid-stage and only `vf<u>` (future volume)
        # carries the `vmin` lower bound; the strict-default would force the
        # SDDP iter-0 forward pass infeasible whenever a previous Benders cut
        # has clamped sini near 0 but the schedule still demands efin >= emin.
        # User can opt back into strict mode by setting strict_storage_emin in
        # the conf or via --set model_options.strict_storage_emin=true.
        model_opts = {
            "use_single_bus": effective_single_bus,
            "use_kirchhoff": src_model.get("use_kirchhoff", True),
            "demand_fail_cost": effective_demand_fail,
            "state_fail_cost": src_model.get("state_fail_cost", 1000),
            "strict_storage_emin": src_model.get("strict_storage_emin", False),
        }
        # Only emit scale_objective if explicitly set (C++ default is 1000).
        if "scale_objective" in src_model:
            model_opts["scale_objective"] = src_model["scale_objective"]
        # Only emit scale_theta if explicitly set (C++ auto_scale_theta
        # computes the optimal value from median line reactance).
        if "scale_theta" in src_model:
            model_opts["scale_theta"] = src_model["scale_theta"]
        if "reserve_fail_cost" in src_model:
            model_opts["reserve_fail_cost"] = src_model["reserve_fail_cost"]
        if "use_line_losses" in src_model:
            model_opts["use_line_losses"] = src_model["use_line_losses"]
        if "line_losses_mode" in src_model:
            model_opts["line_losses_mode"] = src_model["line_losses_mode"]

        planning_opts: dict[str, Any] = {
            "method": method,
            "input_directory": input_dir_val,
            "input_format": input_format,
            "output_directory": "results",
            "output_format": output_format,
            "output_compression": compression,
            "model_options": model_opts,
            "sddp_options": sddp_opts,
        }

        if method == "cascade":
            planning_opts["cascade_options"] = self._build_default_cascade_options(
                model_opts, sddp_opts
            )

        self.planning["options"] = planning_opts

        # Set annual_discount_rate on the simulation section.
        self.planning["simulation"]["annual_discount_rate"] = discount_rate

    def to_json(self, options=None) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        if options is None:
            options = {}

        progress = options.get("_progress")

        def _step(key: str) -> None:
            if progress is not None:
                progress.step(key)

        _step("options")
        self.process_options(options)
        _step("stages")
        self.process_stage_blocks(options)
        self.process_indhor(options)
        _step("scenarios")
        self.process_scenarios(options)
        self.process_apertures(options)
        _step("buses")
        self.process_buses()
        self.process_lines(options)
        _step("generators")
        self.classify_pasada_centrals(options)
        self.process_centrals(options)
        _step("demands")
        self.process_demands(options)
        _step("hydro")
        self.process_ror_spec(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)
        self.process_flow_turbines(options)
        self.process_pmin_flowright(options)
        _step("water_rights")
        self.process_water_rights(options)
        _step("lng")
        self.process_lng(options)
        _step("pumped_storage")
        self.process_pumped_storage(options)
        _step("batteries")
        self.process_battery(options)
        _step("boundary")
        self.process_boundary_cuts(options)
        self.process_variable_scales(options)

        # Organize into planning structure
        name = options.get("name", "plp2gtopt") if options else "plp2gtopt"
        self.planning["system"]["name"] = name

        # Build version string with provenance info
        from plp2gtopt import __version__ as plp2gtopt_version  # noqa: PLC0415

        version = options.get("sys_version", "") if options else ""
        input_dir = options.get("input_dir", "")
        source = Path(input_dir).name if input_dir else ""
        parts = [f"plp2gtopt {plp2gtopt_version}"]
        if source:
            parts.append(f"from {source}")
        if version:
            parts.append(version)
        self.planning["system"]["version"] = ", ".join(parts)

        return self.planning

    def write(self, options=None):
        """Write JSON output to file."""
        if options is None:
            options = {}

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = Path(options["output_file"]) if options else Path("gtopt.json")
        output_file.parent.mkdir(parents=True, exist_ok=True)

        planning = self.to_json(options)

        # Per-segment piecewise feasibility check on the synthesised
        # planning (mirrors `validate_planning.cpp::check_piecewise_feasibility`
        # on the C++ side so authors get the same actionable warning
        # whether they're writing JSON by hand or generating it from
        # PLP).  Warn-only — does not abort the conversion, since the
        # warning may be a known acceptable approximation in the source
        # PLP data and the gtopt run will surface real infeasibilities
        # downstream anyway.
        for warn_msg in _validate_piecewise_segments(planning):
            _logger.warning("%s", warn_msg)

        progress = options.get("_progress")
        if progress is not None:
            progress.step("write")
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(_strip_internal_keys(planning), f, indent=4)
