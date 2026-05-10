# -*- coding: utf-8 -*-

"""Auto-derived water-shortfall pricing.

This module centralises the pricing of "unserved water" surfaces so the
LP can be reasoned about as a single coherent objective.  It replaces
ad-hoc fallbacks (vrebemb cost cascades, hard-coded ``$10000`` flow-
right fail prices, ``2 × max(rebalse_cost)`` heuristics) with a single
formula derived from the case's own demand-failure prices.

The construction:

``ANCHOR = (max_unit_gcost + min_falla_gcost) / 2``

is the **midpoint** between the most expensive supply unit's marginal
cost (``max(non-falla.gcost)``) and the cheapest curtailment rung
(``min(falla.gcost)``).  Economically: water shortage is priced
between "the priciest dispatchable thermal" and "the easiest
electric demand to curtail", so the LP neither over-fills reservoirs
to displace cheap dispatch nor leaves water rights unserved when
electric curtailment is much cheaper.

Replaces the earlier ``max(falla.gcost) × (1 + losses) + 1`` form
which clipped the anchor to the *most expensive* curtailment rung
and produced over-pricing of water (juan/IPLP scale: ~1.6 M $/hm³)
that drove the LP to over-fill at iter-0 and triggered cascade
infeasibilities through the SDDP pass.

Per-element pricing is then proportional to the **lost production
factor** ``lost_pf`` — the energy-equivalent value of the water in
question, computed from topology:

* **Reservoir** (cascade rule): walk ``ser_hid`` from the central,
  summing ``max_rendi`` of every visited central with ``bus > 0``.
  This gives the total energy that water released from the reservoir
  could generate as it flows through the downstream cascade.
* **FlowRight** (single-junction rule): use the ``max_rendi`` of the
  central whose pmin is being enforced.  If the central has ``bus = 0``
  (no generator), return 0 — the FR has no energy-equivalent cost.

``max_rendi`` for a central is the larger of its static ``Rendi``
(``efficiency`` field on the parsed central) and the value computed
from ``plpcenre.dat`` evaluated at the reservoir's ``vmax`` (when a
cenre entry exists for this central).  cenre lifts are applied per the
PLP point-slope formula ``constant + slope × (V - volume)``.

Cost surfaces (units in comments):

``fail_cost      = ANCHOR × lost_pf``                      [$/(m³/s·h)]
``efin_cost      = ANCHOR × lost_pf × 1e6 / 3600``         [$/hm³]
``soft_emin_cost = ANCHOR × lost_pf × 1e6 / 3600``         [$/hm³]

The ``1e6 / 3600`` factor converts ``$/(m³/s·h)`` to ``$/hm³``: 1 hm³ =
10⁶ m³, divided by 3600 s/h gives the m³/s·h equivalent flow-volume
quantity.
"""

from __future__ import annotations

from functools import cached_property
from typing import Any, Dict, Optional


_HM3_PER_M3 = 1.0e6
"""Cubic metres per hm³ — used to convert flow-volume prices."""

_SECONDS_PER_HOUR = 3600.0
"""Seconds per hour — the time-scale conversion in PLP fail-cost units."""


def _cenre_efficiency_at(segments: list, volume: float) -> Optional[float]:
    """Evaluate the cenre piecewise-linear efficiency curve at *volume*.

    cenre uses point-slope form: each segment is ``constant + slope ×
    (V − volume_i)``.  Returns ``None`` when there are no segments.

    The maximum over segments is returned (we want the **upper-envelope
    lift**, i.e. the most generous estimate of the energy a unit of
    water can produce).  This intentionally differs from the parser's
    ``FRendimientos`` concave-min envelope, which is what PLP uses for
    *physical dispatch*; here we are pricing water value, and the
    upper bound is the appropriate proxy.
    """
    if not segments:
        return None
    best: Optional[float] = None
    for seg in segments:
        constant = float(seg.get("constant", 0.0) or 0.0)
        slope = float(seg.get("slope", 0.0) or 0.0)
        v0 = float(seg.get("volume", 0.0) or 0.0)
        eff = constant + slope * (volume - v0)
        if best is None or eff > best:
            best = eff
    return best


class WaterValueResolver:
    """Compute and cache per-element water-shortfall prices.

    Construction is cheap; expensive lookups (anchor, cenre lifts,
    cascade walks) are memoised on demand via :class:`cached_property`
    or per-key dicts.  One resolver is shared between
    :class:`JunctionWriter` and :class:`PminFlowRightWriter`.
    """

    def __init__(
        self,
        *,
        central_parser: Any,
        cenre_parser: Any = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Bind input parsers and option bag.

        Args:
            central_parser: Parsed ``plpcnfce.dat`` (``CentralParser``).
                Required — provides ``centrals`` (with ``gcost``,
                ``efficiency``, ``bus``, ``ser_hid``, ``emax``, ``type``,
                ``number`` fields).
            cenre_parser: Parsed ``plpcenre.dat`` (``CenreParser``).
                Optional — when ``None`` cenre lifts are simply skipped
                and ``max_rendi`` falls back to the static rendi.
            options: plp2gtopt option bag.  Reads ``water_fail_cost``
                (manual override in $/MWh) and ``auto_water_fail_cost``
                (boolean gate).
        """
        self.central_parser = central_parser
        self.cenre_parser = cenre_parser
        self.options = options or {}
        # Pre-build the (number → central) and (name → central) maps so
        # cascade walks and per-central lookups are O(1).
        centrals = list(getattr(central_parser, "centrals", []) or [])
        self._by_number: Dict[int, Dict[str, Any]] = {
            int(c.get("number", -1)): c for c in centrals if "number" in c
        }
        self._by_name: Dict[str, Dict[str, Any]] = {
            str(c.get("name")): c for c in centrals if c.get("name")
        }
        # Memoisation tables for max_rendi() and the cascade walk.
        self._max_rendi_cache: Dict[str, float] = {}
        self._cascade_cache: Dict[int, float] = {}
        self._cenre_by_central: Dict[str, Dict[str, Any]] = {}
        if cenre_parser is not None:
            for entry in getattr(cenre_parser, "efficiencies", []) or []:
                cname = entry.get("name")
                if isinstance(cname, str):
                    self._cenre_by_central[cname] = entry

    # ------------------------------------------------------------------
    # Anchor
    # ------------------------------------------------------------------
    @cached_property
    def anchor(self) -> float:
        """``ANCHOR = (max_unit_gcost + min_falla_gcost) / 2`` (in ``$/MWh``).

        Economic interpretation: water shortage is priced midway
        between

          * the **most expensive supply unit** still in the merit order
            — the marginal cost of the priciest non-``falla`` generator
            that the LP would dispatch to avoid curtailment, and
          * the **cheapest curtailment rung** — the lowest-tier
            ``falla`` price (PLP's per-bus tiered unserved-energy cost).

        This is the natural break-even: any time the system would
        rather pay a thermal unit at ``max_unit_gcost`` than curtail
        at ``min_falla_gcost``, the water value sits halfway between
        — strictly above the most expensive unit (so water doesn't
        force-displace dispatchable thermal) and strictly below the
        cheapest unserved-energy tier (so water shortage isn't
        priced like demand shortage).

        Replaces the previous formula ``max(falla.gcost) + 1`` which
        clipped the anchor to the *most expensive* curtailment rung
        and produced over-pricing of water (e.g. juan/IPLP: 1,589,575
        $/hm³ for LMAULE) that drove the LP to over-fill reservoirs
        and triggered cascade infeasibilities at iter-0 forward
        passes.

        Falls back to ``0`` when either side is missing (degenerate
        cases / unit-test fixtures); callers should treat the
        resulting zero anchor as a no-op.

        Resolution order:

        1. Explicit ``--water-fail-cost`` override (option key
           ``water_fail_cost``) — when set, used directly as the anchor
           in ``$/MWh`` and the auto formula is bypassed.  This is the
           knob users reach for when they want to set the entire anchor
           by hand (e.g. 500 $/MWh) instead of letting it derive from
           the case data.
        2. Auto-derive from ``(max_unit_gcost + min_falla_gcost) / 2``.
        """
        explicit = self.options.get("water_fail_cost")
        if explicit is not None:
            try:
                return float(explicit)
            except (TypeError, ValueError):
                pass

        max_unit_gcost = 0.0  # most expensive non-falla generator
        min_falla_gcost = float("inf")  # cheapest curtailment rung
        for c in getattr(self.central_parser, "centrals", []) or []:
            ctype = c.get("type")
            try:
                gcost = float(c.get("gcost", 0.0) or 0.0)
            except (TypeError, ValueError):
                continue
            if ctype == "falla":
                if gcost > 0.0:
                    min_falla_gcost = min(min_falla_gcost, gcost)
            else:
                # Skip generators with no marginal cost (renewables /
                # zero-fuel hydros) — they don't define a meaningful
                # "most expensive unit" floor for the water anchor.
                if gcost > 0.0:
                    max_unit_gcost = max(max_unit_gcost, gcost)

        if max_unit_gcost <= 0.0 or min_falla_gcost == float("inf"):
            return 0.0
        return (max_unit_gcost + min_falla_gcost) / 2.0

    @property
    def water_fail_cost(self) -> float:
        """Alias for :attr:`anchor` — the resolved water-fail cost in
        ``$/MWh``.  Mirrors the ``--water-fail-cost`` CLI flag name to
        keep call-site code readable.
        """
        return self.anchor

    @property
    def is_active(self) -> bool:
        """True when the new pipeline is enabled.

        Activation gate: either an explicit ``--water-fail-cost``
        override is set (option key ``water_fail_cost``) **or** the
        ``--auto-water-fail-cost`` toggle is on.  When neither is set
        callers must fall back to the legacy pricing paths
        (``_resolve_storage_bound_cost`` /
        ``PminFlowRightWriter._resolve_fail_cost``).
        """
        if self.options.get("water_fail_cost") is not None:
            return True
        return bool(self.options.get("auto_water_fail_cost"))

    # ------------------------------------------------------------------
    # max_rendi  (static rendi lifted by cenre @ vmax)
    # ------------------------------------------------------------------
    def max_rendi(self, central_name: str) -> float:
        """Return ``max(static_rendi, cenre_rendi @ vmax)`` for a central.

        ``static_rendi`` is the central's ``efficiency`` field
        (``Rendi`` in PLP).  When the central has a cenre entry, the
        cenre curve is evaluated at the reservoir's ``vmax`` and the
        larger of the two values is returned.  Returns ``0.0`` when the
        central is unknown.
        """
        cached = self._max_rendi_cache.get(central_name)
        if cached is not None:
            return cached
        central = self._by_name.get(central_name)
        if central is None:
            self._max_rendi_cache[central_name] = 0.0
            return 0.0
        static_rendi = float(central.get("efficiency", 0.0) or 0.0)
        best = static_rendi
        cenre_entry = self._cenre_by_central.get(central_name)
        if cenre_entry is not None:
            # Use the central's own ``emax`` (already in hm³ per the
            # central_parser scale) as the volume at which to evaluate
            # the cenre curve.  When ``emax`` is missing or zero we
            # fall back to the static rendi alone.
            vmax = float(central.get("emax", 0.0) or 0.0)
            if vmax > 0.0:
                cenre_eff = _cenre_efficiency_at(
                    cenre_entry.get("segments", []) or [], vmax
                )
                if cenre_eff is not None and cenre_eff > best:
                    best = cenre_eff
        self._max_rendi_cache[central_name] = best
        return best

    # ------------------------------------------------------------------
    # lost_pf — cascade and single-junction rules
    # ------------------------------------------------------------------
    def cascade_lost_pf(self, start_central_number: int) -> float:
        """Sum of ``max_rendi`` along the ``ser_hid`` chain (bus>0 only).

        The walk starts at *start_central_number* and follows
        ``ser_hid`` until it hits 0 (terminal) or a previously visited
        node (cycle guard).  Only visited centrals with ``bus > 0`` are
        included in the sum — transit-only centrals (``bus = 0``)
        contribute no energy value.  Returns ``0.0`` for unknown
        starting numbers.
        """
        if start_central_number is None:
            return 0.0
        try:
            start = int(start_central_number)
        except (TypeError, ValueError):
            return 0.0
        cached = self._cascade_cache.get(start)
        if cached is not None:
            return cached
        total = 0.0
        visited: set[int] = set()
        cur_num = start
        while cur_num and cur_num > 0 and cur_num not in visited:
            visited.add(cur_num)
            cur = self._by_number.get(cur_num)
            if cur is None:
                break
            try:
                bus = int(cur.get("bus", 0) or 0)
            except (TypeError, ValueError):
                bus = 0
            if bus > 0:
                cname = cur.get("name")
                if isinstance(cname, str):
                    total += self.max_rendi(cname)
            try:
                cur_num = int(cur.get("ser_hid", 0) or 0)
            except (TypeError, ValueError):
                cur_num = 0
        self._cascade_cache[start] = total
        return total

    def junction_lost_pf(self, central_number: int) -> float:
        """Return the central's own ``max_rendi`` if ``bus > 0`` else 0.

        Used for FlowRights bound to a single junction (the gen
        waterway's downstream junction): the energy obligation is
        anchored to the central whose pmin is being enforced.  Bus=0
        transits have no generator, so the FR has no energy-equivalent
        cost.
        """
        if central_number is None:
            return 0.0
        try:
            num = int(central_number)
        except (TypeError, ValueError):
            return 0.0
        cur = self._by_number.get(num)
        if cur is None:
            return 0.0
        try:
            bus = int(cur.get("bus", 0) or 0)
        except (TypeError, ValueError):
            bus = 0
        if bus <= 0:
            return 0.0
        cname = cur.get("name")
        if not isinstance(cname, str):
            return 0.0
        return self.max_rendi(cname)

    # ------------------------------------------------------------------
    # Cost surfaces
    # ------------------------------------------------------------------
    def fail_cost(self, lost_pf: float) -> float:
        """``ANCHOR × lost_pf``  [units: $/(m³/s·h)]."""
        return self.anchor * float(lost_pf)

    def efin_cost(self, lost_pf: float) -> float:
        """``ANCHOR × lost_pf × 1e6 / 3600``  [units: $/hm³]."""
        return self.anchor * float(lost_pf) * _HM3_PER_M3 / _SECONDS_PER_HOUR
