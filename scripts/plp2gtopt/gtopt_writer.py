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

# JSON post-processing helpers live in ``gtopt_shared.json_utils`` so
# every converter shares a single implementation (issue #507 Phase 1).
# The aliases preserve the legacy private names plp2gtopt's existing
# tests + helpers already import.
from gtopt_shared.json_utils import sanitize_inf as _sanitize_inf
from gtopt_shared.json_utils import strip_internal_keys as _strip_internal_keys

from ._writer_boundary import BoundaryMixin
from ._writer_generation import GenerationMixin
from ._writer_hydro import HydroMixin
from ._writer_network import NetworkMixin
from ._writer_time import TimeMixin
from .line_parser import LineParser
from .plp_parser import PLPParser

_logger = logging.getLogger(__name__)


def _try_scalar(value: Any) -> float | None:
    """Extract a scalar float from a JSON value if possible.

    Mirrors the C++ side's ``try_scalar_value`` (validate_planning.cpp):
    a scalar Real / int collapses to ``float``; vector / file / string
    schedules return ``None`` (validation deferred to load time).
    """
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    return None


def _collapse_orphan_drain_outflows(system: Dict[str, Any]) -> int:
    """Convert waterway / turbine ``junction_b`` refs that target an orphan
    drain-only sink junction to **outflow mode** (drop ``junction_b``) and
    remove the now-unreferenced junction.

    A junction qualifies when:
      * ``drain=True`` AND
      * name ends in ``_sink`` or ``_ocean`` (the converter's drain-only
        synthesis convention — distinguishes from real reservoir junctions
        that may also carry ``drain=True``) AND
      * referenced ONLY as ``junction_b`` of waterway / turbine entries.

    Returns the number of junctions collapsed.  Keeps the spillage flow
    visible on its waterway / turbine while eliminating the synthetic
    ``<central>_ocean`` / ``<name>_sink`` downstream junction.
    """
    juncs = system.get("junction_array", [])
    waterways = system.get("waterway_array", [])
    turbines = system.get("turbine_array", [])

    def _is_candidate(j: Dict[str, Any]) -> bool:
        if not j.get("drain"):
            return False
        name = str(j.get("name", ""))
        return name.endswith("_sink") or name.endswith("_ocean")

    cand_names: set[str] = {j["name"] for j in juncs if _is_candidate(j)}
    if not cand_names:
        return 0

    by_uid: Dict[Any, str] = {j["uid"]: j["name"] for j in juncs}

    def _ref_name(ref: Any) -> str | None:
        if isinstance(ref, str):
            return ref
        if isinstance(ref, int):
            return by_uid.get(ref)
        return None

    BLOCK: tuple[str, Any] = ("block", None)
    refs: Dict[str, list[tuple[str, Any]]] = {n: [] for n in cand_names}

    for ww in waterways:
        nm = _ref_name(ww.get("junction_b"))
        if nm in cand_names:
            refs[nm].append(("ww_b", ww))
        nm_a = _ref_name(ww.get("junction_a"))
        if nm_a in cand_names:
            refs[nm_a].append(BLOCK)
    for t in turbines:
        nm = _ref_name(t.get("junction_b"))
        if nm in cand_names:
            refs[nm].append(("turb_b", t))
        nm_a = _ref_name(t.get("junction_a"))
        if nm_a in cand_names:
            refs[nm_a].append(BLOCK)
    for f in system.get("flow_array", []):
        nm = _ref_name(f.get("junction"))
        if nm in cand_names:
            refs[nm].append(BLOCK)
    for r in system.get("reservoir_array", []):
        for k in ("junction", "spill_junction"):
            nm = _ref_name(r.get(k))
            if nm in cand_names:
                refs[nm].append(BLOCK)
    for fr in system.get("flow_right_array", []):
        for k in ("junction_a", "junction_b"):
            nm = _ref_name(fr.get(k))
            if nm in cand_names:
                refs[nm].append(BLOCK)

    collapsed: set[str] = set()
    for nm, rlist in refs.items():
        if not rlist or any(r == BLOCK for r in rlist):
            continue
        for _kind, el in rlist:
            el.pop("junction_b", None)
        collapsed.add(nm)

    if collapsed:
        system["junction_array"] = [j for j in juncs if j["name"] not in collapsed]
        _logger.info(
            "GTOptWriter: collapsed %d orphan drain sink/ocean junction(s) "
            "to outflow waterways/turbines (junction_b unset): %s",
            len(collapsed),
            ", ".join(sorted(collapsed)),
        )

    return len(collapsed)


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
        # Set by ``process_options`` when ``method == "cascade-reduced"``;
        # consumed by ``write()`` to trigger the post-step that emits the
        # per-level reduced JSONs + CSVs.
        self._cascade_reduced_active: bool = False

    @staticmethod
    def _normalize_method(method: str) -> str:
        """Normalize solver type string.

        Accepts ``'sddp'``, ``'mono'`` / ``'monolithic'``, ``'cascade'``, or
        ``'cascade-reduced'``; returns the canonical form.

        ``cascade-reduced`` is a 4-level multi-fidelity cascade that uses the
        ``gtopt_reduce_network`` package to produce reduced grids for the
        middle two levels (L1 transport-only at ONB/6 buses, L2 DC-OPF with
        per-demand loss-factor uplift at ONB/3 buses); the wrapper JSON is
        written alongside the main case and referenced by each level's
        ``system_file`` field.
        """
        if method in ("mono", "monolithic"):
            return "monolithic"
        if method == "cascade":
            return "cascade"
        if method in ("cascade-reduced", "cascade_reduced"):
            return "cascade-reduced"
        return "sddp"

    def _build_default_cascade_options(
        self,
        model_opts: dict[str, Any],
        sddp_opts: dict[str, Any],
    ) -> dict[str, Any]:
        """Build a 4-level default cascade configuration.

        Iteration budgets (PLP's ``PDMaxIte`` = ``total_iter``):
          - L0 ``warmup``:       ``2 × total_iter`` — cheapest iter
            (1 aperture × single-bus), worth the extra headroom for
            the bootstrap policy that every later level inherits.
          - L1 / L2 / L3:        ``total_iter`` each.  Earlier the
            deeper levels were capped at ``total_iter / 2`` on the
            rationale that inherited cuts converge them faster, but
            the asymmetric caps made the transport / full_network
            exits opaque (e.g. "why did transport stop at iter 6?").
            With uniform ``total_iter`` caps, each level either
            stationary-converges or hits its own explicit budget — no
            magic-number ratios.

        Aperture budget split (via ``SddpOptions.num_apertures`` and
        ``SddpOptions.aperture_selection_mode``):
          - L0 ``warmup``:       ``num_apertures = 1, head``  — single
            wettest hydrology.
          - L1 ``uninodal``:     ``num_apertures = 4, stride`` —
            wettest + driest + 2 evenly-spaced interior.
          - L2 ``transport``:    ``num_apertures = 8, stride``.
          - L3 ``full_network``: ``num_apertures`` /
            ``aperture_selection_mode`` **unset** — iterates the full
            per-phase aperture list (every scenario emitted by
            :func:`aperture_writer.build_phase_apertures`).  Robust to
            per-case variations in aperture count.

        Stationary-convergence settings per level (post-2026-05
        rewrite — see ``sddp_iteration.cpp``).  ``stationary_tol``
        is now the **ΔUB** stationarity threshold (relative iter-over-
        iter change in the realised policy cost UB);
        ``stationary_gap_ceiling`` is the signed (UB-LB)/|UB| ceiling.
        Both AND'd: a level converges only when the policy has stopped
        moving (ΔUB < tol) AND the bound width is acceptable (gap <
        ceiling).  Negative gaps (multi-cut overshoot) automatically
        satisfy the ceiling without penalty.
          - L0 ``warmup``:       ``ΔUB < 0.005 %`` AND ``|gap| < 85 %``
          - L1 ``uninodal``:     ``ΔUB < 0.01 %``  AND ``|gap| < 85 %``
          - L2 ``transport``:    ``ΔUB < 0.25 %``  AND ``|gap| < 85 %``
          - L3 ``full_network``: ``ΔUB < 1 %``     AND ``|gap| < 85 %``
        ``stationary_tol`` is LOOSENED deeper into the cascade because
        each level's iter is more expensive (L0 ~12 s/iter on
        juan/IPLP, L3 ~9 min/iter) — we demand the strictest policy
        stability where iters are cheap and accept progressively
        looser stability where each iter costs more.  L2/L3 were
        loosened a further ×5 / ×10 on 2026-05-27 (0.05 %→0.25 %,
        0.1 %→1 %) so the full-fidelity levels exit on ΔUB stationarity
        rather than chasing sub-noise Δgap.
        ``stationary_gap_ceiling`` is intentionally flat at 85 %
        across all levels — the multi-cut + aperture-mode overshoot
        on production cases (juan/IPLP at uninodal) routinely
        produces transient |gap| around 5-10 % that the AND-mode check
        previously kept rejecting.  With the ceiling at 85 %,
        the policy-stability signal (ΔUB) is the binding constraint
        and the levels exit cleanly once the realised cost stops
        moving.

        Each level inherits state-variable targets and **all**
        optimality cuts from previous levels via elastic constraints
        (``inherit_targets = -1, inherit_optimality_cuts = -1``).  The
        cut store therefore accumulates across the cascade: L1 sees
        L0's cuts, L2 sees L0 + L1, L3 sees L0 + L1 + L2.  Preserves
        the tightest available value-function envelope at every level
        — at the cost of a larger master LP at the tail.

        ``Phase.apertures`` is emitted by
        :func:`aperture_writer.build_phase_apertures` in **wettest →
        driest** order, so ``num_apertures = N`` paired with the
        selection mode controls which N apertures each level visits per
        phase.  Resolved on the C++ side by ``sddp_aperture.cpp``'s
        ``select_apertures`` helper.
        """
        total_iter = sddp_opts.get("max_iterations", 100)
        convergence_tol = sddp_opts.get("convergence_tol", 0.01)

        # ── Iteration budgets (floored at 1; see docstring §1) ──
        # L0 gets 2 × total_iter (cheapest 1-aperture iter — doubling
        # buys the most policy-seed headroom).  L1/L2/L3 each receive
        # the full ``total_iter`` budget.  Earlier the deeper levels
        # were capped at ``total_iter / 2`` on the rationale that
        # inherited cuts would converge them faster, but the
        # asymmetric caps made the transport / full_network exits
        # opaque (e.g. "why did transport stop at iter 6?").  Each
        # level now either stationary-converges or hits its own
        # explicit cap.
        l0_iter = max(2 * total_iter, 1)
        l1_iter = max(total_iter, 1)
        l2_iter = max(total_iter, 1)
        l3_iter = max(total_iter, 1)

        # ── Cascade-global options ─────────────────────────────────────
        # ``max_iterations`` here is a GLOBAL budget across all levels
        # (cascade_method.cpp guards via "global iteration budget
        # exhausted"); set it to the sum of per-level budgets so every
        # level is reachable.
        cascade_sddp_opts = {
            **sddp_opts,
            "max_iterations": l0_iter + l1_iter + l2_iter + l3_iter,
            "stationary_tol": sddp_opts.get("stationary_tol", 0.01),
        }

        # ── Inter-level transition (see docstring §4) ──────────────────
        transition = {
            "inherit_targets": -1,
            "inherit_optimality_cuts": -1,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500.0,
        }

        # ── Per-level SDDP options (see docstring §§2, 3) ──────────────
        # Convergence settings per level — after the 2026-05 rewrite
        # the two knobs measure different things and the stop
        # condition is AND, not OR:
        #
        #   * ``stationary_tol``         = ΔUB / UB threshold (policy
        #                                  stability — the realised cost
        #                                  has stopped moving by more
        #                                  than this fraction iter-over-
        #                                  iter; ``UB`` is the unbiased
        #                                  Monte-Carlo estimate of the
        #                                  policy cost).
        #   * ``stationary_gap_ceiling`` = symmetric ``|gap|`` threshold
        #                                  on the signed
        #                                  ``(UB-LB)/|UB|`` gap (bound
        #                                  quality — accepts mild
        #                                  multi-cut / aperture
        #                                  overshoot, rejects wild
        #                                  overshoot beyond the ceiling
        #                                  as a pathology signal).
        #
        # ``stationary_tol`` ladder — 0.005 % (L0 warmup) →
        # 0.01 % (L1 uninodal) → 0.25 % (L2 transport) →
        # 1 % (L3 full_network).  The ladder is calibrated to the
        # empirical Δgap noise floor at each level (juan/IPLP):
        #   * L0 deterministic head aperture → solver-tolerance
        #     floor ~0.02-0.06 %, so 0.025 % is meaningful.
        #   * L1 4-aperture Monte-Carlo → 2× L0's floor.
        #   * L2 multi-bus + 8-aperture → modelling+sampling noise.
        #   * L3 + Kirchhoff + full aperture list → widest noise.  The knob LOOSENS deeper into the
        # cascade rather than tightening, because the noise floor of
        # the realised Δgap rises with level fidelity:
        #
        #   * L0/L1 are single-bus.  No network-model noise; the only
        #     noise source is aperture sampling, which is narrow
        #     (1 head aperture for L0, 4 stride apertures for L1).
        #     The strictest 0.01 % floor is statistically meaningful
        #     here, so we let both levels drive Δgap down to that
        #     threshold before handing off.
        #
        #   * L2 turns the multi-bus + transport-line network on,
        #     introducing modeling-driven noise on top of aperture
        #     sampling.  0.25 % is the tightest floor compatible with
        #     this noise regime.  Before the 2026-05-15
        #     convergence-table fix surfaced the real (windowed) Δgap,
        #     L2 was running at a misleadingly loose 1 % knob.
        #
        #   * L3 adds Kirchhoff plus the FULL per-phase aperture list
        #     (every scenario), so its sampling-driven noise floor is
        #     the WIDEST of any level.  0.5 % accepts that noise; a
        #     tighter floor would burn iters on Δgap differences that
        #     are sample-noise, not policy improvement.
        #
        # ``stationary_gap_ceiling = 0.85`` stays flat across every
        # level — the binding signal is policy stationarity, not
        # bound width.  The ceiling exists to reject wild multi-cut
        # overshoot (>85 %) as a pathology rather than to enforce a
        # bound-quality target.
        #
        # ``min_iterations`` per level: gtopt's default is 1 (just
        # enough to let the convergence check fire on a qualifying
        # first iter).  Only L0 ``warmup`` overrides this with 3,
        # because its single-aperture face-value bound is structurally
        # noisy and the bootstrap iter can fluke its way into the
        # ceiling on a single sample.  L1+ inherit a converged
        # envelope and may legitimately exit on their first qualifying
        # iter.
        l0_sddp_options: dict[str, Any] = {
            "max_iterations": l0_iter,
            # min_iterations=2 + stationary_window=3 — matches the L1/L2
            # cascade-level pairing.  L0 has no prior cascade level so
            # the cross-level seed is empty here; the window=3 just
            # delays stationarity-firing until 3 in-level iters of
            # history exist, which lets the alpha-bootstrap settle
            # (otherwise the iter-0 face-value bound can fluke into
            # the ceiling and trip stationarity on iter 1).
            "min_iterations": 2,
            # stationary_window=2 across every cascade level (2026-06-02
            # ladder).  Two consecutive sub-tol iters are enough to
            # call ΔUB stationary on the realised juan/IPLP noise
            # regime; the previous window=3 only delayed exits without
            # changing the converged policy.  L3 already used 2 (last
            # level optimisation); flattening the rest matches.
            "stationary_window": 2,
            "convergence_tol": convergence_tol,
            # 2026-06-02 (final): flat 1 % stationary_tol across every
            # cascade level (L0..L3).  Earlier same-day ladder values
            # (0.005 % → 0.5 % progression) all converged with
            # realised Δgap well below the threshold, leaving genuine
            # policy improvement on the table while burning iters on
            # sub-noise tightening.  The new_bess_emissions PLP-2y
            # case shows the LB-overshoot pathology under
            # heterogeneous-aperture transitions where the next-level
            # cuts re-tighten the bound regardless of the within-level
            # exit criterion — so the only useful per-iter floor is
            # the one that exits every level at the same noise regime
            # as the final L3 stationarity bar.
            "stationary_tol": 0.01,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": 1,
            "aperture_selection_mode": "head",
        }
        l1_sddp_options: dict[str, Any] = {
            "max_iterations": l1_iter,
            # min_iterations=2 paired with the C++ cross-level Δgap
            # seed (cascade_method.cpp calls
            # SDDPMethod::seed_prior_bounds with the prior level's
            # final UB/LB).  Without this min-iter guard, an iter 1
            # whose inherited envelope nearly matches L0's UB would
            # report a sub-tol Δgap and trip stationary convergence
            # immediately — defeating the cascade's purpose (L1
            # must demonstrate stationarity on its own 4-aperture
            # sample, not just inherit L0's bound).
            "min_iterations": 2,
            "stationary_window": 2,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.01,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": 4,
            "aperture_selection_mode": "stride",
        }
        l2_sddp_options: dict[str, Any] = {
            "max_iterations": l2_iter,
            # See L1 sddp_options comment for the min_iterations=2
            # rationale (cross-level Δgap seed safety).  Window=2
            # across the cascade (2026-06-02 flat-ladder change).
            "min_iterations": 2,
            "stationary_window": 2,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.01,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": 8,
            "aperture_selection_mode": "stride",
        }
        # L3: num_apertures / aperture_selection_mode intentionally
        # unset → full per-phase list (docstring §2).
        l3_sddp_options: dict[str, Any] = {
            "max_iterations": l3_iter,
            # stationary_window=2 (no min_iterations override; gtopt's
            # default of 1 applies).  L3 is the LAST cascade level and
            # inherits the fully-converged transport envelope, so the
            # cross-level seed gives iter 1 a meaningful 2-iter lookback
            # and we want a fast exit-when-stationary path once the
            # full-fidelity multi-bus + Kirchhoff model is at a
            # stationary fixed point.
            "stationary_window": 2,
            "convergence_tol": convergence_tol,
            # Relative ΔUB/UB tolerance — 1 % on L3.  L3 runs the FULL
            # per-phase aperture list (every scenario), so its
            # Monte-Carlo noise floor is the widest of any level; a 1 %
            # per-iter stationarity bar lets the full-fidelity model
            # exit once ΔUB has flattened to sample noise instead of
            # chasing sub-noise Δgap.  The absolute |gap| guard rises
            # to 85 % via ``stationary_gap_ceiling`` below.
            "stationary_tol": 0.01,
            "stationary_gap_ceiling": 0.85,
        }

        # ── Level array ────────────────────────────────────────────────
        level_array = [
            {
                "uid": 1,
                "name": "warmup",
                "model_options": {
                    "use_single_bus": True,
                },
                "sddp_options": l0_sddp_options,
            },
            {
                "uid": 2,
                "name": "uninodal",
                "model_options": {
                    "use_single_bus": True,
                },
                "sddp_options": l1_sddp_options,
                "transition": transition,
            },
            {
                "uid": 3,
                "name": "transport",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": False,
                    "use_line_losses": False,
                },
                "sddp_options": l2_sddp_options,
                "transition": transition,
            },
            {
                "uid": 4,
                "name": "full_network",
                "model_options": {
                    k: v
                    for k, v in model_opts.items()
                    if k
                    in (
                        "use_single_bus",
                        "use_kirchhoff",
                        "kirchhoff_mode",
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
                "sddp_options": l3_sddp_options,
                "transition": transition,
            },
        ]

        return {
            "model_options": model_opts,
            "sddp_options": cascade_sddp_opts,
            "level_array": level_array,
        }

    def _build_reduced_cascade_options(
        self,
        model_opts: dict[str, Any],
        sddp_opts: dict[str, Any],
        cascade_opts: dict[str, Any],
        case_stem: str,
    ) -> dict[str, Any]:
        """Build a 4-level cascade with reduced grids at L1 and L2.

        Level shape:
          - **L0 ``uninodal``** — full topology projected to a single bus
            (``use_single_bus=true``); 1 ``head`` aperture; LB bootstrap.
          - **L1 ``reduced_transport_K{K1}``** — reducer-emitted grid at
            ``K1 = max(ONB // l1_reduce_ratio, l1_min_buses)`` buses, pure
            transport (no KVL, no losses); 1 ``head`` aperture; LB
            refinement on a coarse multi-bus grid.
          - **L2 ``reduced_dcopf_K{K2}``** — reducer-emitted grid at
            ``K2 = max(ONB // l2_reduce_ratio, l2_min_buses)`` buses,
            full DC-OPF (KVL on, per-demand ``lossfactor`` uplift);
            ``max(ONA // l2_aperture_ratio, 1)`` ``stride`` apertures;
            first level with scenario diversity.
          - **L3 ``full_network``** — full topology with the user's
            original ``model_options``; full per-phase aperture list.

        The actual reduced JSON+CSV artefacts are written in a later step
        (``_emit_reduced_artefacts``) once the full ``self.planning`` is
        populated; this builder only emits the cascade level dict shape
        with ``system_file`` paths pointing at those future artefacts.

        ``cascade_opts`` carries the runtime knobs (reduce ratios, min
        buses, uplift pct, aperture ratio, distance metrics, disable
        flags) — typically populated by the plp2gtopt CLI.

        ``case_stem`` is the stem of the main JSON (without ``.json``
        extension) — used to derive the reduced-file basenames so the
        cascade can find them in the parent JSON's directory.
        """
        total_iter = sddp_opts.get("max_iterations", 100)
        convergence_tol = sddp_opts.get("convergence_tol", 0.01)

        l1_iter = max(total_iter // 2, 1)
        l2_iter = max(total_iter // 2, 1)
        l3_iter = max(total_iter // 4, 1)

        disable_l1 = bool(cascade_opts.get("disable_l1", False))
        disable_l2 = bool(cascade_opts.get("disable_l2", False))

        cascade_sddp_opts = {
            **sddp_opts,
            "max_iterations": total_iter + l1_iter + l2_iter + l3_iter,
            "stationary_tol": sddp_opts.get("stationary_tol", 0.01),
        }

        transition = {
            "inherit_targets": -1,
            "inherit_optimality_cuts": -1,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500.0,
        }

        # Aperture inference: ONA = max apertures referenced by any phase
        # in self.planning.simulation.phase_array.  Falls back to 1 if
        # the field is absent (early-call edge case).
        #
        # L1 and L2 both use stride apertures derived from ONA.  L1 is
        # coarser (ratio 4 → fewer apertures), L2 finer (ratio 2 → more).
        ona = self._count_max_apertures_per_phase()
        l1_aperture_ratio = max(int(cascade_opts.get("l1_aperture_ratio", 4)), 1)
        l2_aperture_ratio = max(int(cascade_opts.get("l2_aperture_ratio", 2)), 1)
        l1_num_apertures = max(ona // l1_aperture_ratio, 1)
        l2_num_apertures = max(ona // l2_aperture_ratio, 1)

        # L3 model_options: same filter as _build_default_cascade_options.
        l3_model_options = {
            k: v
            for k, v in model_opts.items()
            if k
            in (
                "use_single_bus",
                "use_kirchhoff",
                "kirchhoff_mode",
                "use_line_losses",
                "kirchhoff_threshold",
                "loss_segments",
                "strict_storage_emin",
            )
        }

        l0_sddp_options: dict[str, Any] = {
            "max_iterations": total_iter,
            "min_iterations": 3,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.05,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": 1,
            "aperture_selection_mode": "head",
        }
        l1_sddp_options = {
            "max_iterations": l1_iter,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.04,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": l1_num_apertures,
            "aperture_selection_mode": "stride",
        }
        l2_sddp_options = {
            "max_iterations": l2_iter,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.03,
            "stationary_gap_ceiling": 0.85,
            "num_apertures": l2_num_apertures,
            "aperture_selection_mode": "stride",
        }
        l3_sddp_options = {
            "max_iterations": l3_iter,
            "convergence_tol": convergence_tol,
            "stationary_tol": 0.02,
            "stationary_gap_ceiling": 0.85,
        }

        level_array: list[dict[str, Any]] = [
            {
                "uid": 1,
                "name": "uninodal",
                "model_options": {"use_single_bus": True},
                "sddp_options": l0_sddp_options,
            },
        ]

        next_uid = 2
        if not disable_l1:
            level_array.append(
                {
                    "uid": next_uid,
                    "name": "reduced_transport",
                    "model_options": {
                        "use_single_bus": False,
                        "use_kirchhoff": False,
                        "use_line_losses": False,
                    },
                    "system_file": f"{case_stem}.L1.json",
                    "sddp_options": l1_sddp_options,
                    "transition": transition,
                }
            )
            next_uid += 1

        if not disable_l2:
            level_array.append(
                {
                    "uid": next_uid,
                    "name": "reduced_dcopf",
                    "model_options": {
                        "use_single_bus": False,
                        "use_kirchhoff": True,
                        "use_line_losses": False,
                    },
                    "system_file": f"{case_stem}.L2.json",
                    "sddp_options": l2_sddp_options,
                    "transition": transition,
                }
            )
            next_uid += 1

        level_array.append(
            {
                "uid": next_uid,
                "name": "full_network",
                "model_options": l3_model_options,
                "sddp_options": l3_sddp_options,
                "transition": transition,
            }
        )

        return {
            "model_options": model_opts,
            "sddp_options": cascade_sddp_opts,
            "level_array": level_array,
        }

    def _count_max_apertures_per_phase(self) -> int:
        """Return the max ``apertures`` array length across all phases.

        Used by the cascade-reduced builder to compute L2's
        ``num_apertures``.  Defaults to 1 if phases or apertures are
        absent (e.g. early-call before phases populated).
        """
        phase_array = self.planning.get("simulation", {}).get("phase_array", []) or []
        if not phase_array:
            return 1
        max_aps = 0
        for ph in phase_array:
            aps = ph.get("apertures") or []
            if isinstance(aps, list):
                max_aps = max(max_aps, len(aps))
        return max(max_aps, 1)

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
        # NOTE: a top-level ``num_apertures`` option (legacy CLI input) is NOT
        # piped through here.  The cascade builder emits per-level
        # ``num_apertures`` directly into each level's ``sddp_options``
        # (see ``_build_default_cascade_options``); for plain SDDP the user
        # can override at the gtopt CLI with ``--sddp-num-apertures N`` or
        # ``--set sddp_options.num_apertures=N``.
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

        # Drop feasibility cuts from aperture clone replay so they don't
        # conflict with perturbed trial states.  Default true (matches C++).
        sddp_opts["aperture_drop_fcuts"] = options.get("aperture_drop_fcuts", True)

        # Backward-solver threads: defer to the C++ default (2).  The
        # historical override pinning to 1 was based on per-cell solve
        # being parallel-simplex-overhead-bound, but the post-2026-05
        # acceleration work (Option B + bulk add_rows + presolve OFF in
        # the CPLEX plugin) collapsed cut-replay overhead enough that
        # parallel-simplex on the cell pays off.  Override via
        # `--set sddp_options.backward_solver_options.threads=N` if
        # needed; do NOT pin to 1 by default any more.
        if options.get("backward_solver_threads") is not None:
            sddp_opts["backward_solver_options"] = {
                "threads": options["backward_solver_threads"],
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

        # `output_directory` is written RELATIVE TO the input directory so each
        # case's results are isolated.  When the JSON lives outside the data
        # dir (`-f` elsewhere, input_dir_val != "."), a bare "results" would
        # resolve against gtopt's CWD and every case launched from a shared
        # parent would clobber a common ./results.  Nesting it under the
        # case-scoped input dir (e.g. `<case>/results`) keeps outputs beside
        # their inputs.  When input_dir_val == "." the JSON already lives in
        # the data dir, so "results" is already case-local.
        output_dir_val = (
            "results" if input_dir_val == "." else f"{input_dir_val}/results"
        )

        src_model = options.get("model_options", {})

        # PLP parity: the curtailment cost is applied PER-DEMAND via each
        # real demand's ``fcost`` field (set from falla_by_bus below).
        # The global ``model_options.demand_fail_cost`` is the fallback
        # for demands without an explicit fcost.  When the user has not
        # supplied an explicit value, we derive it from the case's own
        # PLP falla units: ``max(falla.gcost)`` — the highest fail price
        # anywhere in the system.  Going lower than that risks the LP
        # preferring wholesale curtailment via the global on a synthetic
        # path over actual generation that costs less than the most
        # expensive falla but more than the global.
        #
        # Synthetic battery-charge demands (created by C++
        # ``System::expand_batteries``) pin fcost=0 explicitly in C++ so
        # they are NOT sensitive to this global default — raising the
        # global never distorts battery dispatch.
        user_demand_fail = src_model.get("demand_fail_cost")
        if user_demand_fail is not None:
            effective_demand_fail = user_demand_fail
        else:
            max_falla = 0.0
            try:
                max_falla = float(self.central_parser.max_falla_cost())
            except (AttributeError, TypeError, ValueError):
                max_falla = 0.0
            effective_demand_fail = max_falla

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
            # §11.10 rename: gtopt canonical is `state_violation_cost`;
            # `state_fail_cost` JSON key still accepted via the
            # naming-dialects registry for back-compat (so existing
            # callers passing the legacy key in `src_model` keep working).
            "state_violation_cost": src_model.get(
                "state_violation_cost",
                src_model.get("state_fail_cost", 1000),
            ),
            "strict_storage_emin": src_model.get("strict_storage_emin", False),
            "auto_scale": src_model.get("auto_scale", True),
        }
        # scale_objective is method-aware on the C++ side since the SDDP
        # basis-condition tuning (commit ab041592f): the default is 1.0 for
        # sddp / cascade and 1000 for monolithic.  scale_objective is the
        # dominant LP basis-condition contributor once Benders cuts
        # accumulate — forwarding the monolithic 1000 into a cascade case
        # inflates kappa (measured 3.0e9 -> 7.6e9 on a 2-year cascade,
        # 118 vs 20 "high kappa" warnings) without changing the optimum.
        # plp2gtopt's CLI/conf default is the monolithic 1000, so for
        # sddp/cascade methods we emit 1.0 instead.  An explicit non-1000
        # value from the conf/CLI is treated as a deliberate override and
        # forwarded verbatim for every method.
        _src_scale = src_model.get("scale_objective")
        _sddp_like = method in ("sddp", "cascade", "cascade-reduced")
        if _src_scale is not None and float(_src_scale) != 1000.0:
            model_opts["scale_objective"] = _src_scale
        elif _sddp_like:
            model_opts["scale_objective"] = 1.0
        elif _src_scale is not None:
            model_opts["scale_objective"] = _src_scale
        # Only emit scale_theta if explicitly set (C++ auto_scale_theta
        # computes the optimal value from median line reactance).
        if "scale_theta" in src_model:
            model_opts["scale_theta"] = src_model["scale_theta"]
        # §11.10 rename: gtopt canonical is `reserve_shortage_cost`;
        # accept either spelling from `src_model` for back-compat,
        # emit the new canonical.
        if "reserve_shortage_cost" in src_model:
            model_opts["reserve_shortage_cost"] = src_model["reserve_shortage_cost"]
        elif "reserve_fail_cost" in src_model:
            model_opts["reserve_shortage_cost"] = src_model["reserve_fail_cost"]
        if "use_line_losses" in src_model:
            model_opts["use_line_losses"] = src_model["use_line_losses"]
        if "line_losses_mode" in src_model:
            model_opts["line_losses_mode"] = src_model["line_losses_mode"]
        if "kirchhoff_mode" in src_model:
            model_opts["kirchhoff_mode"] = src_model["kirchhoff_mode"]

        # ── Iterative fast-path defaults (sddp + cascade) ──────────────────
        # Benchmarked plp2gtopt -> gtopt pipeline (~PLP parity on CEN65 2-year):
        #   * lp_reduction          — elide provably-zero LP columns (~-19% wall)
        #   * aperture_solve_mode   = warm   (dual aperture warm-start)
        #   * aperture_chunk_size   = -1     (all apertures/phase per chunk,
        #                                     one LP clone, warm-start reuse)
        #   * forward/backward_solver_options.algorithm = dual + advanced_basis
        # These are applied here (not only in main.process_options, whose nested
        # opts the writer otherwise rebuilds from an allowlist) so they actually
        # reach the emitted JSON.  They are written onto the TOP-LEVEL
        # model_options / sddp_options, which for cascade become the base options
        # (`m_base_opts_` in cascade_method.cpp) that every level copies via
        # `build_level_sddp_opts` — so the same config flows through L0..L3.
        # The bundled cplex.prm is retuned to dual / no-presolve to match (see
        # plp2gtopt.install_solver_param_files).  Every field is overridable from
        # the source conf's model_options / sddp_options.
        if method in ("sddp", "cascade", "cascade-reduced"):
            src_sddp = options.get("sddp_options") or {}
            if src_model.get("lp_reduction") is not None:
                model_opts["lp_reduction"] = src_model["lp_reduction"]
            else:
                model_opts.setdefault("lp_reduction", True)
            sddp_opts.setdefault(
                "aperture_solve_mode",
                src_sddp.get("aperture_solve_mode") or "warm",
            )
            if "aperture_chunk_size" not in sddp_opts:
                _acs = src_sddp.get("aperture_chunk_size")
                sddp_opts["aperture_chunk_size"] = -1 if _acs is None else _acs
            _fwd = dict(src_sddp.get("forward_solver_options") or {})
            _fwd.setdefault("algorithm", "dual")
            _fwd.setdefault("advanced_basis", True)
            sddp_opts.setdefault("forward_solver_options", _fwd)
            _bwd = sddp_opts.get("backward_solver_options")
            if _bwd is None:
                _bwd = dict(src_sddp.get("backward_solver_options") or {})
            _bwd.setdefault("algorithm", "dual")
            sddp_opts["backward_solver_options"] = _bwd
            # PLP-faithful cut sharing: each scene-LP carries N dedicated
            # future-cost columns (varphi_0..N-1) and scenario-s's backward
            # cut lands on varphi_s in every scene-LP, priced 1/N (matches
            # plp-agrespd.f:94 source indexing + defprbpd.f:810 averaging).
            # Defaulted here too — not just in main.build_options — so
            # writer-direct callers (convert_plp_case with a raw opts dict)
            # also emit it.  An explicit options["cut_sharing_mode"] set at
            # line ~978 above wins (setdefault is a no-op then).
            sddp_opts.setdefault("cut_sharing_mode", "multicut")

        planning_opts: dict[str, Any] = {
            "method": method,
            "input_directory": input_dir_val,
            "input_format": input_format,
            "output_directory": output_dir_val,
            "output_format": output_format,
            "output_compression": compression,
            "model_options": model_opts,
            "sddp_options": sddp_opts,
            "lp_matrix_options": {
                "equilibration_method": "ruiz",
            },
        }
        # ``options.write_out`` controls which output streams gtopt
        # serialises (sol / dual / rc / extras, optionally restricted to
        # element classes).  When the CLI was given (or the conf file
        # set a default), thread it through verbatim; otherwise leave
        # the field unset so gtopt picks its own default.
        write_out = options.get("write_out")
        if write_out:
            planning_opts["write_out"] = write_out

        if method == "cascade":
            # Per-level aperture budgets use ``num_apertures`` (resolved
            # at the C++ side via ``truncate_apertures`` against the
            # wettest-first ``Phase.apertures``), so the cascade options
            # can be built once here without depending on the
            # ``aperture_array`` that ``process_apertures`` produces.
            planning_opts["cascade_options"] = self._build_default_cascade_options(
                model_opts, sddp_opts
            )
        elif method == "cascade-reduced":
            # 4-level multi-fidelity cascade: L0 uninodal, L1 reduced
            # transport (ONB/6), L2 reduced DC-OPF + lossfactor uplift
            # (ONB/3), L3 full network.  Reduced JSONs + busmap/linemap
            # CSVs are written alongside the main case in a post-step
            # (``_emit_reduced_artefacts``) once the full system arrays
            # are populated — see ``write()``.
            #
            # On the C++ side this is just a regular cascade with
            # ``system_file`` set on some levels — gtopt's MethodType
            # enum still sees ``"cascade"`` in the JSON.  The Python
            # writer tracks the cascade-reduced flavour internally so
            # the post-step knows to emit the reduced artefacts.
            case_stem = Path(options.get("output_file", "case.json")).stem
            cascade_opts = options.get("cascade_reduced_opts") or {}
            planning_opts["cascade_options"] = self._build_reduced_cascade_options(
                model_opts, sddp_opts, cascade_opts, case_stem
            )
            planning_opts["method"] = "cascade"  # ← C++-facing label
            # Remember the original label so ``write()``'s post-step
            # knows to invoke the reducer.
            self._cascade_reduced_active = True

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
        # All reservoir-bearing phases have run (junctions, water rights,
        # pumped storage); stamp the global default water-fail value onto
        # any reservoir left with an `efin` floor but no `efin_cost`.
        self.apply_default_water_fail()
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

        # Final topology pass: convert waterways / turbines whose
        # ``junction_b`` is an orphan ``*_sink`` / ``*_ocean`` drain junction
        # into outflow mode (drop ``junction_b``), and remove the now-
        # unreferenced sink/ocean junction.  Saves one synthetic junction
        # per terminal spillway (``_ver`` waterways routed to
        # ``<central>_ocean``) and per 1-ended diversion (``_sink``) while
        # keeping the flow VISIBLE on its waterway/turbine.
        _collapse_orphan_drain_outflows(self.planning["system"])

        # Optional PLEXOS overlay: merge heat-rate / Fuel data from a
        # plexos2gtopt-emitted gtopt JSON onto the PLP-derived planning.
        # Only continuous fields are carried (no commitment / UC /
        # integer primitives — see _plexos_overlay._FORBIDDEN_FIELDS).
        plexos_overlay_src = options.get("plexos_overlay") if options else None
        if plexos_overlay_src is not None:
            from ._plexos_overlay import apply_plexos_overlay  # noqa: PLC0415

            report_path = options.get("plexos_overlay_report") if options else None
            if report_path is None:
                out_dir = options.get("output_dir") if options else None
                if out_dir is not None:
                    report_path = Path(out_dir) / "plexos_overlay_report.json"
            # When --emissions-file is also set, pass it as the fallback
            # source for per-generator metadata the PLEXOS overlay
            # couldn't supply (cogen / geothermal / waste-heat units).
            # See _plexos_overlay.apply_plexos_overlay's
            # `emissions_fallback_path` param + GeneratorOverride.
            emissions_fallback = options.get("emissions_file") if options else None
            apply_plexos_overlay(
                self.planning,
                Path(plexos_overlay_src),
                report_path=Path(report_path) if report_path is not None else None,
                emissions_fallback_path=(
                    Path(emissions_fallback) if emissions_fallback else None
                ),
            )

        # IPCC emission-factor fill-in.  Runs AFTER the PLEXOS overlay so
        # any CO2 factor PLEXOS shipped wins; this step only fills
        # gaps on Fuel elements that still lack one and synthesizes the
        # ``emission_array`` pollutant row when needed.  Enabled by the
        # ``--emissions`` master switch (off by default).
        only_emissions = bool(options and options.get("only_emissions", False))
        if options and (options.get("emissions", False) or only_emissions):
            from gtopt_shared.emissions import (  # noqa: PLC0415
                apply_emission_defaults_from_file,
            )

            emissions_src = options.get("emissions_file")
            emissions_report = options.get("emissions_report")
            if emissions_report is None:
                out_dir = options.get("output_dir")
                if out_dir is not None:
                    emissions_report = Path(out_dir) / "plexos_emissions_report.json"
            # ``only_emissions`` (issue #519) → also stamps
            # ``model_options.objective_mode = "emissions"`` and
            # ``EmissionZone.price = carbon_price`` (default 35.0
            # USD/tCO2eq Chile SCC).  See gtopt_shared/emissions.py.
            apply_emission_defaults_from_file(
                self.planning,
                Path(emissions_src) if emissions_src is not None else None,
                report_path=(
                    Path(emissions_report) if emissions_report is not None else None
                ),
                only_emissions=only_emissions,
                carbon_price=options.get("carbon_price"),
            )

            # Synthetic emissions ray (#520) — only in --only-emissions
            # mode.  Computes EPF per reservoir from the cascade graph
            # then writes a single-row boundary_cuts.csv that replaces
            # the dollar-FCF the emissions overlay just dropped.  Each
            # reservoir slope = EPF · gas_em · loss · 8760 · NPV(r,N).
            if only_emissions:
                out_dir = options.get("output_dir")
                if out_dir is not None:
                    from gtopt_shared.hydro_epf import (  # noqa: PLC0415
                        epf_per_reservoir,
                    )
                    from plp2gtopt._emissions_ray import (  # noqa: PLC0415
                        stamp_boundary_cuts_file_ref,
                        write_emissions_ray_csv,
                    )

                    epfs = epf_per_reservoir(self.planning.get("system", {}))
                    if epfs:
                        cuts_path = Path(out_dir) / "boundary_cuts.csv"
                        write_emissions_ray_csv(
                            cuts_path,
                            epfs,
                            discount_rate=options.get("emissions_discount_rate", 0.05),
                            horizon_years=options.get("emissions_horizon_years"),
                        )
                        stamp_boundary_cuts_file_ref(self.planning, "boundary_cuts.csv")

        # Topology-driven reservoir extraction-flow estimate (shared with
        # plexos2gtopt).  Runs LAST so the reservoir_array is fully
        # assembled AND the discharge parquet (written by
        # ``process_afluents``) exists on disk — natural inflows are
        # parquet references here, resolved from ``<output_dir>/Flow``.
        # Replaces the generic C++ ReservoirLP -9000/6000 m³/s extraction
        # defaults with tight per-reservoir bounds.  On by default;
        # ``--no-reservoir-flow-estimate`` sets the option False.
        if options is None or options.get("reservoir_flow_estimate", True):
            from gtopt_shared.reservoir_flow import (  # noqa: PLC0415
                apply_reservoir_flow_estimates,
                widen_extraction_bounds_symmetric,
            )

            out_dir = options.get("output_dir") if options else None
            apply_reservoir_flow_estimates(
                self.planning,
                input_dir=Path(out_dir) if out_dir is not None else None,
            )
            # Replace the tight directional estimate with the SAME symmetric
            # box plexos2gtopt uses: ``[-2·e, +2·e]`` with
            # ``e = max(|fmin_est|, |fmax_est|)``.  The directional accept
            # cap (``fmin = -max_inflow``) was binding on spill at
            # PEHUENCHE s14/p7 and driving the SDDP forward pass infeasible;
            # the loose-but-finite symmetric box keeps the LP conditioned
            # (no free ±inf extraction columns) without cutting feasible
            # operation.
            widen_extraction_bounds_symmetric(self.planning, factor=2.0)

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
            json.dump(
                _sanitize_inf(_strip_internal_keys(planning)),
                f,
                indent=4,
            )

        # Post-step: when the cascade-reduced method is selected, run the
        # network reducer on the now-complete planning to produce the
        # per-level reduced JSONs + busmap/linemap/aggregator CSVs that
        # the cascade level dicts already reference via ``system_file``.
        if getattr(self, "_cascade_reduced_active", False):
            self._emit_reduced_artefacts(output_file, options)

    def _emit_reduced_artefacts(self, output_file: Path, options: dict) -> None:
        """Run the network reducer on the completed planning and write
        the L1 (transport-only) + L2 (DC-OPF with lossfactor uplift)
        reduced JSONs alongside the main case.

        ``options['cascade_reduced_opts']`` carries the runtime knobs
        (``l1_reduce_ratio``, ``l2_reduce_ratio``, ``l1_min_buses``,
        ``l2_min_buses``, ``l2_uplift_pct``, ``l2_uplift_collision``,
        ``l1_distance``, ``l2_distance``, ``disable_l1``, ``disable_l2``).
        """
        # Local import — the reducer package is a peer of plp2gtopt; we
        # import lazily so plp2gtopt cases that never use cascade-reduced
        # don't pay the import cost (and stay testable in environments
        # where gtopt_reduce_network is absent).
        # pylint: disable=import-outside-toplevel
        from gtopt_reduce_network._busmap import (  # noqa: PLC0415
            save_aggregator,
            save_busmap,
            save_linemap,
            save_reducer_config,
        )
        from gtopt_reduce_network._io import Case, _index_buses  # noqa: PLC0415
        from gtopt_reduce_network._io import save_case as _save_case  # noqa: PLC0415
        from gtopt_reduce_network._reduce import (  # noqa: PLC0415
            ReduceConfig,
            reduce_case,
        )

        cascade_opts = options.get("cascade_reduced_opts") or {}
        disable_l1 = bool(cascade_opts.get("disable_l1", False))
        disable_l2 = bool(cascade_opts.get("disable_l2", False))
        if disable_l1 and disable_l2:
            _logger.info(
                "cascade-reduced: both L1 and L2 disabled — no reduced "
                "artefacts emitted"
            )
            return

        out_path = Path(output_file)
        case_stem = out_path.stem
        case_dir = out_path.parent

        # Build a Case wrapping the in-memory planning (deep-copied so the
        # reducer cannot mutate the writer's own dict).
        base_case = Case(raw=json.loads(json.dumps(self.planning)))
        _index_buses(base_case)
        n_buses = len(base_case.array("bus_array"))
        if n_buses == 0:
            _logger.warning("cascade-reduced: bus_array is empty — skipping reducer")
            return

        l1_ratio = max(int(cascade_opts.get("l1_reduce_ratio", 6)), 1)
        l2_ratio = max(int(cascade_opts.get("l2_reduce_ratio", 3)), 1)
        l1_min = max(int(cascade_opts.get("l1_min_buses", 4)), 1)
        l2_min = max(int(cascade_opts.get("l2_min_buses", 8)), 1)
        l1_K = max(n_buses // l1_ratio, l1_min)
        l2_K = max(n_buses // l2_ratio, l2_min)
        # Cap at n_buses-1 so the reducer always has some merging to do
        # (k == n_buses degenerates into the identity case, which still
        # writes a valid JSON but defeats the purpose).
        l1_K = min(l1_K, max(n_buses - 1, 1))
        l2_K = min(l2_K, max(n_buses - 1, 1))

        # parquet_case_dir is the directory containing the main JSON; the
        # reducer reads <case_dir>/<input_directory>/Line/*.parquet there.
        parquet_case_dir_str = str(case_dir)

        for tag, cfg in (
            (
                "L1",
                ReduceConfig(
                    target_buses=l1_K,
                    distance=cascade_opts.get("l1_distance", "reactance-shortest-path"),
                    transport_only=True,
                    # L1 carries the same per-demand uplift as L2 so the
                    # bus-balance row is consistent across levels; the
                    # only difference between L1 and L2 is the network
                    # topology + KVL.
                    loss_mode="uplift",
                    loss_uplift_pct=float(cascade_opts.get("l1_uplift_pct", 3.0)),
                    loss_uplift_collision=cascade_opts.get(
                        "l1_uplift_collision", "replace"
                    ),
                    include_reservoir_hosts=True,
                    reduced_tag="L1",
                    parquet_case_dir=parquet_case_dir_str,
                ),
            ),
            (
                "L2",
                ReduceConfig(
                    target_buses=l2_K,
                    distance=cascade_opts.get("l2_distance", "ptdf"),
                    transport_only=False,
                    loss_mode="uplift",
                    loss_uplift_pct=float(cascade_opts.get("l2_uplift_pct", 3.0)),
                    loss_uplift_collision=cascade_opts.get(
                        "l2_uplift_collision", "replace"
                    ),
                    include_reservoir_hosts=True,
                    reduced_tag="L2",
                    parquet_case_dir=parquet_case_dir_str,
                ),
            ),
        ):
            if tag == "L1" and disable_l1:
                continue
            if tag == "L2" and disable_l2:
                continue

            # Each level needs a fresh deep-copied case — reduce_case
            # mutates internal caches.
            case = Case(raw=json.loads(json.dumps(self.planning)))
            _index_buses(case)
            result = reduce_case(case, cfg)

            stem = f"{case_stem}.{tag}"
            _save_case(result.case, case_dir / f"{stem}.json")
            save_busmap(result.busmap, case_dir / f"{stem}.busmap.csv")
            save_linemap(result.linemap, case_dir / f"{stem}.linemap.csv")
            save_aggregator(result.aggregator, case_dir / f"{stem}.aggregator.csv")
            save_reducer_config(cfg.as_dict(), case_dir / f"{stem}.reducer_config.json")
            _logger.info(
                "cascade-reduced %s: %d→%d buses, %d→%d lines "
                "(written: %s.json + 3 CSVs + reducer_config.json)",
                tag,
                n_buses,
                len(result.case.array("bus_array")),
                len(self.planning.get("system", {}).get("line_array", [])),
                len(result.case.array("line_array")),
                stem,
            )
