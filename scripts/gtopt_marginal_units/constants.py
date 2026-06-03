# SPDX-License-Identifier: BSD-3-Clause
"""Constants and enums for gtopt_marginal_units.

* Tolerances — every unit-bearing knob is named with its unit so a
  reader cannot conflate `tol_price` ($/MWh) with `tol_load_mw` (MW).
  This is the lp-numerics P0.2 fix from the planner-fan-out review.
* Status enum — the 8-status classification table from master §4.4
  plus the synthetic statuses emitted by §4.5's marginal-unit
  picker when no merit-eligible interior unit exists.
* FormulaKind enum — the bus_price_recipe / emission_intensity_recipe
  taxonomy from master §4.6.4 / §4.12.2.
* Confidence enum — how the classification was derived.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


# ---------------------------------------------------------------------------
# Tolerances. All defaults match the master plan §4.4 table.
# ---------------------------------------------------------------------------


@dataclass(slots=True, frozen=True)
class Tolerances:
    eps: float = 1.0e-4  # MW absolute slack on dispatch bounds
    tol_price: float = 1.0e-3  # $/MWh — LMP vs MC and zone-LMP spread
    tol_flow: float = 5.0e-3  # dimensionless — fractional saturation
    tol_mu: float = 1.0e-2  # $/MWh — line dual nonzero threshold
    tol_lmp: float = 1.0e-2  # $/MWh — intra-zone LMP spread
    tol_load_mw: float = 1.0  # MW — zone-load vs Σpmin (lp-numerics P0.2 fix)

    @classmethod
    def default(cls) -> "Tolerances":
        return cls()


# ---------------------------------------------------------------------------
# Status enum — the 8-status priority-ordered rule from master §4.4
# plus the synthetic markers from §4.5.
# ---------------------------------------------------------------------------


class Status(str, Enum):
    OFF = "off"
    FORCED_PMIN = "forced_pmin"
    CAPPED_PMAX = "capped_pmax"
    HYDRO_MARGINAL = "hydro_marginal"
    MARGINAL = "marginal"
    INFRAMARGINAL = "inframarginal"
    EXTRAMARGINAL_INTERIOR = "extramarginal_interior"
    PROFILE_DISPATCHED = "profile_dispatched"

    # Synthetic / zone-level §4.5 markers
    FORCED_PMIN_MARGINAL = "forced_pmin_marginal"
    DEMAND_FAIL = "__demand_fail__"
    RENEWABLE_CURTAILMENT = "__renewable_curtailment__"
    UNATTRIBUTED = "__unattributed__"


# Statuses that mark a unit as "marginal" for the recipe table:
MARGINAL_STATUSES: frozenset[Status] = frozenset(
    {Status.MARGINAL, Status.HYDRO_MARGINAL, Status.FORCED_PMIN_MARGINAL}
)


# ---------------------------------------------------------------------------
# FormulaKind enum — bus_price_recipe rows.
# ---------------------------------------------------------------------------


class FormulaKind(str, Enum):
    SINGLE_UNIT = "single_unit"
    TIED_UNITS = "tied_units"
    FORCED_PMIN_MARGINAL = "forced_pmin_marginal"
    HYDRO_MARGINAL = "hydro_marginal"
    DEMAND_FAIL = "demand_fail"
    RENEWABLE_CURTAILMENT = "renewable_curtailment"
    # A real island (one or more buses) that has no merit-eligible
    # candidate AND no demand AND no generator with positive pmax at
    # this cell — i.e. nothing is happening on the island this hour.
    # LP gives LMP = 0 by free-vertex choice.  Distinct from
    # ``UNATTRIBUTED`` (which signals a recipe / data gap) — this one
    # is a faithful "nobody's home" classification.  Applies whether
    # the island is a singleton (tie lines inactive) or several
    # genuinely-disconnected buses.
    EMPTY_ISLAND = "empty_island"
    UNATTRIBUTED = "unattributed"


# ---------------------------------------------------------------------------
# Confidence — how the classification was derived (§4.6 output schema).
# ---------------------------------------------------------------------------


class Confidence(str, Enum):
    LP_DUAL = "lp_dual"  # gtopt simulated, duals available
    MERIT_ORDER = "merit_order"  # real-mode merit-order reconstruction
    FALLBACK = "fallback"  # degenerate / missing-data / clamped


# ---------------------------------------------------------------------------
# Generator kind enum — used for merit-eligibility filtering.
# ---------------------------------------------------------------------------


class GeneratorKind(str, Enum):
    THERMAL = "thermal"
    HYDRO = "hydro"
    BATTERY = "battery"
    PROFILE = "profile"


# Profile units (wind/solar) cannot set price by definition — zero MC,
# never marginal. Used by §4.11.2's merit_eligible filter.
PROFILE_KINDS: frozenset[GeneratorKind] = frozenset({GeneratorKind.PROFILE})


# ---------------------------------------------------------------------------
# Default merit-ladder depth (master §4.9, §13 open question 5).
# ---------------------------------------------------------------------------


DEFAULT_MERIT_LADDER_DEPTH: int = 3


# ---------------------------------------------------------------------------
# CLI exit codes (master §5).
# ---------------------------------------------------------------------------


EXIT_OK = 0
EXIT_UNATTRIBUTED = 2  # script ran but at least one cell had __unattributed__
EXIT_INPUT_ERROR = 3  # missing files / inconsistent flags / writer-side invariant fail


__all__ = [
    "Confidence",
    "DEFAULT_MERIT_LADDER_DEPTH",
    "EXIT_INPUT_ERROR",
    "EXIT_OK",
    "EXIT_UNATTRIBUTED",
    "FormulaKind",
    "GeneratorKind",
    "MARGINAL_STATUSES",
    "PROFILE_KINDS",
    "Status",
    "Tolerances",
]
