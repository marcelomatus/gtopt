# -*- coding: utf-8 -*-

"""Shared reservoir water-value pricing for the gtopt converters.

Centralises the pricing of "unserved water" surfaces (reservoir terminal
soft cost ``efin_cost``, flow-right ``fail_cost``, soft ``emin`` cost) so
both ``plp2gtopt`` and ``plexos2gtopt`` derive them the same way, and so
the gtopt C++ side no longer has to reconstruct water values from the
boundary-cut file at load time.

Computation (decisions 2026-06-16):

1. Per-reservoir estimate — anchor blend × cascade-lost-PF::

       ANCHOR    = 0.75·avg_thermal_gcost + 0.25·min_falla_gcost      [$/MWh]
       efin_cost = ANCHOR × lost_pf × 1e6 / 3600                      [$/hm³]

   ``lost_pf`` (energy-equivalent of the stored water) is the sum of
   ``max_rendi`` along the downstream hydro cascade — see
   :meth:`WaterValueResolver.cascade_lost_pf`.

2. Boundary-cut OVERWRITE — when SDDP boundary cuts are available for a
   reservoir, the estimate is *overwritten* by the cut **lower-bound**
   water value (not min-capped).  The lower bound replicates the gtopt
   C++ ``cut_soft_cost(min)`` rule: the cut ships coefficient ``-wv`` on
   each state variable, so the lower-bound cost is ``-max(coeff)``,
   positive-floored — see :func:`cut_lower_bound`.

3. Default water-fail value — the global fallback for reservoirs/rights
   without a specific value is the **maximum** of all per-reservoir water
   values — see :func:`default_water_fail_value`.

The ``central_parser`` / ``cenre_parser`` inputs are duck-typed: any
object exposing ``.centrals`` (an iterable of dicts with ``number``,
``name``, ``gcost``, ``efficiency``, ``bus``, ``ser_hid``, ``emax``,
``type`` ∈ {``termica``, ``falla``, ``embalse``, ``serie``, ``pasada``})
and ``.efficiencies`` (cenre curves) works.  ``plp2gtopt`` passes its
native parsers; ``plexos2gtopt`` passes lightweight adapters built from
its entity tables.
"""

from __future__ import annotations

from functools import cached_property
from typing import Any, Dict, Iterable, Mapping, Optional


_HM3_PER_M3 = 1.0e6
"""Cubic metres per hm³ — used to convert flow-volume prices."""

_SECONDS_PER_HOUR = 3600.0
"""Seconds per hour — the time-scale conversion in PLP fail-cost units."""

_CUT_FLOOR = 1.0
"""Absolute floor ($/hm³) for a cut-derived water value, mirroring the
gtopt C++ ``cut_soft_cost`` fallback when a cut prices water as a
liability (all coefficients non-negative)."""


def cut_lower_bound(coeffs: Iterable[float]) -> Optional[float]:
    """Lower-bound water value ($/hm³) from one reservoir's cut coefficients.

    Replicates the gtopt C++ ``cut_soft_cost(BoundaryCutSoftCost::min)``:
    a boundary cut ships coefficient ``-wv`` on each state variable, so
    the *lower bound* of the (positive) water-value cost is
    ``-max(coeff)`` — the most-negative coefficient yields the largest
    cost, the least-negative yields the smallest, and the **maximum**
    coefficient yields the lower bound.

    Positive-floored: if the selected cost is non-positive (a cut that
    prices water as a liability, i.e. some coefficient ≥ 0), fall back to
    the column's own smallest positive water value ``-max_neg_coeff`` if
    one exists, else :data:`_CUT_FLOOR`.

    Returns ``None`` when ``coeffs`` is empty (no cut for this reservoir).
    """
    vals = [float(c) for c in coeffs]
    if not vals:
        return None
    cost = -max(vals)  # lower bound of the positive cost
    if cost > 0.0:
        return cost
    # Cut prices water as a liability — fall back to the smallest positive
    # water value in the column (negate the largest negative coeff).
    neg = [v for v in vals if v < 0.0]
    if neg:
        return -max(neg)
    return _CUT_FLOOR


def default_water_fail_value(water_values: Iterable[float]) -> float:
    """Global default water-fail value = **max** of all reservoir water
    values ($/hm³).  Used as the fallback for reservoirs / rights that do
    not carry their own value.  Returns ``0.0`` for an empty input.
    """
    vals = [float(v) for v in water_values if v is not None]
    return max(vals) if vals else 0.0


def _cenre_efficiency_at(segments: list, volume: float) -> Optional[float]:
    """Evaluate the cenre piecewise-linear efficiency curve at *volume*.

    cenre uses point-slope form: each segment is ``constant + slope ×
    (V − volume_i)``.  Returns ``None`` when there are no segments.

    The maximum over segments is returned (upper-envelope lift — the most
    generous estimate of the energy a unit of water can produce), which
    intentionally differs from the parser's concave-min envelope used for
    *physical dispatch*; here we are pricing water value.
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
    or per-key dicts.
    """

    def __init__(
        self,
        *,
        central_parser: Any,
        cenre_parser: Any = None,
        options: Optional[Dict[str, Any]] = None,
        cut_water_values: Optional[Mapping[str, float]] = None,
    ) -> None:
        """Bind input parsers and option bag.

        Args:
            central_parser: Object exposing ``.centrals`` (see module
                docstring for the duck-typed field contract).
            cenre_parser: Optional object exposing ``.efficiencies``
                (cenre curves).  When ``None`` cenre lifts are skipped and
                ``max_rendi`` falls back to the static rendi.
            options: option bag.  Reads ``water_fail_cost`` (manual
                anchor override in $/MWh) and ``auto_water_fail_cost``
                (boolean gate).
            cut_water_values: Optional per-reservoir boundary-cut
                **lower-bound** water value in ``$/hm³`` (typically built
                by the caller via :func:`cut_lower_bound` from the cut
                file).  When a reservoir is present here,
                :meth:`efin_cost_for` **overwrites** the estimate with
                this value; missing reservoirs keep the auto estimate.
        """
        self.central_parser = central_parser
        self.cenre_parser = cenre_parser
        self.options = options or {}
        self._cut_water_values: Dict[str, float] = dict(cut_water_values or {})
        centrals = list(getattr(central_parser, "centrals", []) or [])
        self._by_number: Dict[int, Dict[str, Any]] = {
            int(c.get("number", -1)): c for c in centrals if "number" in c
        }
        self._by_name: Dict[str, Dict[str, Any]] = {
            str(c.get("name")): c for c in centrals if c.get("name")
        }
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
        """``ANCHOR = 0.75·avg_thermal_gcost + 0.25·min_falla_gcost`` ($/MWh).

        A weighted blend between the average thermal generator marginal
        cost (typical replacement cost) and the cheapest curtailment rung
        (``min(falla.gcost)``).  Falls back to ``0`` when either side is
        missing.  An explicit ``water_fail_cost`` option overrides the
        auto formula (used directly as the anchor in $/MWh).
        """
        explicit = self.options.get("water_fail_cost")
        if explicit is not None:
            try:
                return float(explicit)
            except (TypeError, ValueError):
                pass

        thermal_gcosts: list[float] = []
        min_falla_gcost = float("inf")
        for c in getattr(self.central_parser, "centrals", []) or []:
            ctype = c.get("type")
            try:
                gcost = float(c.get("gcost", 0.0) or 0.0)
            except (TypeError, ValueError):
                continue
            if ctype == "falla":
                if gcost > 0.0:
                    min_falla_gcost = min(min_falla_gcost, gcost)
            elif ctype == "termica":
                if gcost > 0.0:
                    thermal_gcosts.append(gcost)

        if not thermal_gcosts or min_falla_gcost == float("inf"):
            return 0.0
        avg_thermal_gcost = sum(thermal_gcosts) / len(thermal_gcosts)
        return 0.75 * avg_thermal_gcost + 0.25 * min_falla_gcost

    @property
    def water_fail_cost(self) -> float:
        """Alias for :attr:`anchor` — resolved water-fail cost in $/MWh."""
        return self.anchor

    @property
    def is_active(self) -> bool:
        """True when the auto/explicit water-value pipeline is enabled."""
        if self.options.get("water_fail_cost") is not None:
            return True
        return bool(self.options.get("auto_water_fail_cost"))

    # ------------------------------------------------------------------
    # max_rendi  (static rendi lifted by cenre @ vmax)
    # ------------------------------------------------------------------
    def max_rendi(self, central_name: str) -> float:
        """Return ``max(static_rendi, cenre_rendi @ vmax)`` for a central."""
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
        """Sum of ``max_rendi`` along ``ser_hid`` up to the next reservoir.

        Walks ``ser_hid`` from *start_central_number*, stopping at
        ``ser_hid = 0``, a cycle, or the next downstream ``embalse``.
        Only ``embalse`` (start only) and ``serie`` units with ``bus > 0``
        contribute; ``pasada`` (run-of-river) is excluded.  Returns
        ``0.0`` for unknown starting numbers.
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
            cur = self._by_number.get(cur_num)
            if cur is None:
                break
            if cur.get("type") == "embalse" and cur_num != start:
                break
            visited.add(cur_num)
            try:
                bus = int(cur.get("bus", 0) or 0)
            except (TypeError, ValueError):
                bus = 0
            ctype = cur.get("type")
            if bus > 0 and ctype in ("embalse", "serie"):
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
        """Return the central's own ``max_rendi`` if ``bus > 0`` else 0."""
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
        """``ANCHOR × lost_pf``  [$/(m³/s·h)], rounded to 2 d.p."""
        return round(self.anchor * float(lost_pf), 2)

    def efin_cost(self, lost_pf: float) -> float:
        """``ANCHOR × lost_pf × 1e6 / 3600``  [$/hm³], rounded to 2 d.p.

        Auto estimate only — ignores any boundary-cut override.
        Reservoir-aware callers should prefer :meth:`efin_cost_for`.
        """
        return round(self.anchor * float(lost_pf) * _HM3_PER_M3 / _SECONDS_PER_HOUR, 2)

    def efin_cost_for(self, reservoir_name: str, lost_pf: float) -> float:
        """Per-reservoir ``efin_cost`` ($/hm³), boundary-cut aware.

        When ``cut_water_values`` carries a value for *reservoir_name*,
        that boundary-cut **lower-bound** water value **overwrites** the
        auto estimate (per the 2026-06-16 decision); otherwise the auto
        ``ANCHOR × lost_pf`` estimate is returned.
        """
        override = self._cut_water_values.get(reservoir_name)
        if override is not None:
            return round(float(override), 2)
        return self.efin_cost(lost_pf)
