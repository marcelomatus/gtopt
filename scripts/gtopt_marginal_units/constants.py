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
    # Lower bound on the merit-walk headroom check (``_recipes.py``).
    # ``tol.eps`` measures the LP dispatch slack; a separate explicit
    # floor below which "the gen has no real headroom up" prevents the
    # merit walk from picking a numerically-noisy gen as backfill.
    tol_headroom_mw: float = 1.0e-6  # MW
    # Lower bound on the reduced-cost test for line saturation
    # (``main.py``).  Same floor logic as ``tol_headroom_mw``: even
    # when the user dials ``tol_price`` below 1e-3, basic-vs-bound rc
    # should still be distinguishable from solver noise.
    tol_rc_floor: float = 1.0e-3  # $/MWh
    # Lower bound on the primal-fallback flow-saturation test
    # (``main.py``).  Same floor logic as ``tol_rc_floor``.
    tol_sat_floor: float = 1.0e-3  # dimensionless
    # Per-bus loss-factor scaling for #523 emission attribution.
    # raw = bus_LMP[i] / SRMC[g_marginal] (or LMP[bus_marginal] for storage).
    # Empirical envelope from the CEN 2-year cascade hits ~1.4 on stressed
    # radial buses (Atacama, Aysén); raw > ~5 is no longer a loss factor
    # but a cross-island congestion artifact (marginal lives in a
    # different electrical island).
    loss_factor_warn: float = 2.0  # raw > warn → warn-summary at end
    loss_factor_error: float = 5.0  # raw > error → AttributionError

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
    # Negative zone LMP with a NON-storage marginal is not a real
    # marginal: the LP value carries a reactance-loop artefact, an
    # energy-constraint dual (must-run / spillover penalty), or a
    # ramp-limit shadow.  Recipe row sets ``marginal_gen_uids=[]``,
    # records the original (negative) lambda_z in ``recomputed_value``
    # for audit, and emits a warning summary.  Downstream arbitrage /
    # battery-balance scripts MUST filter on
    # ``formula_kind == 'no_marginal_neg_lmp'`` to skip these cells —
    # there is no guaranteed payment / attribution.
    #
    # When the marginal IS a storage (battery / hydro) the negative
    # LMP IS meaningful (spillover penalty / regulation constraint
    # binding) and the recipe keeps the storage marginal flagged
    # SINGLE_UNIT / HYDRO_MARGINAL, with the dual clamped to 0 so
    # energy + CO2 arbitrage continues.
    NO_MARGINAL_NEG_LMP = "no_marginal_neg_lmp"
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


# Storage / opportunity-cost units (battery + reservoir hydro) bid on
# stored-energy / water value, not on a thermodynamic marginal cost.
# The LP can elect them as basic-in-LP (their rc ≈ 0 when they bind),
# but for marginal-unit attribution they behave like price-takers: the
# "real" marginal is the next-up thermal unit they displace.  Used
# together with the cogen filter in
# :func:`_select_marginal_candidates` to keep storage / hydro reservoir
# out of the LP-elected marginal pool; the recipe layer's
# ``_compute_consequential_moer`` walk-up then attributes the cell to
# the thermal merit gen actually setting the carbon / cost adder.
STORAGE_KINDS: frozenset[GeneratorKind] = frozenset(
    {GeneratorKind.BATTERY, GeneratorKind.HYDRO}
)

# The only "real" price-setting kind: dispatchable thermal units.
# Everything else (profile, battery, hydro reservoir, cogen) is a
# price-taker by one mechanism or another.  See `_select_marginal_candidates`
# for the cascade that uses this.
PRICE_SETTER_KINDS: frozenset[GeneratorKind] = frozenset({GeneratorKind.THERMAL})


# ---------------------------------------------------------------------------
# Synthetic-gen prefilter — CEN-PCP CCGT mode-variants + _INF placeholders.
# ---------------------------------------------------------------------------

#: Capacity threshold below which a Generator is treated as a *synthetic*
#: placeholder by ``_select_marginal_candidates``.  CEN PCP ships ~50 CCGT
#: alternative-dispatch entries (``ATA_CC1_TGA_DIE``, ``KELAR-TG1_GNL_X``,
#: …) with ``pmax = 0.01 MW``, ``gcost = 0``, and no Fuels membership —
#: these can never physically set the LMP at grid scale but the LP's
#: tie-break corners do elect them at the basis.  0.5 MW is the natural
#: cutoff: every real CEN dispatchable unit is at least 5 MW (small
#: diesels) and the synthetic ones are all at the 0.01 MW PLEXOS default.
#: Gens below this are pre-filtered from the dispatched pool with a
#: graceful fallback (if every dispatched gen in the zone is synthetic,
#: the cascade proceeds on the unfiltered set so the recipe still returns
#: *something* to the consequential walk-up).
SYNTHETIC_GEN_PMAX_MW: float = 0.5

#: Generator-name suffixes that mark a PLEXOS "infinity" placeholder.
#: CEN PCP wraps unbounded LNG-fuel supplies and a handful of synthetic
#: gas-import accounting entries with the ``_INF`` / ``_NOGNL_INF`` /
#: ``_GNL_INF`` suffixes (see ``plexos2gtopt`` parsers).  These are not
#: real merchant units; ``_select_marginal_candidates`` excludes them
#: from the LP-marginal pool with the same graceful fallback as
#: ``SYNTHETIC_GEN_PMAX_MW``.
SYNTHETIC_GEN_NAME_SUFFIXES: tuple[str, ...] = (
    "_INF",
    "_NOGNL_INF",
    "_GNL_INF",
)


# ---------------------------------------------------------------------------
# Default merit-ladder depth (master §4.9, §13 open question 5).
# ---------------------------------------------------------------------------


DEFAULT_MERIT_LADDER_DEPTH: int = 3


# ---------------------------------------------------------------------------
# Unit conversion constants.
# ---------------------------------------------------------------------------


#: Kilograms per metric ton.  Used to convert ``carbon_price_usd_per_ton``
#: ($/tCO2eq) into the ``carbon_adder_usd_per_mwh`` arithmetic when the
#: emission-intensity column is in kg / MWh.  Named to prevent the
#: ``cp / 1000.0`` idiom from drifting into a "$/kg" vs "$/t" bug.
KG_PER_T: float = 1000.0


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
    "KG_PER_T",
    "MARGINAL_STATUSES",
    "PRICE_SETTER_KINDS",
    "PROFILE_KINDS",
    "STORAGE_KINDS",
    "SYNTHETIC_GEN_NAME_SUFFIXES",
    "SYNTHETIC_GEN_PMAX_MW",
    "Status",
    "Tolerances",
]
