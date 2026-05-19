# SPDX-License-Identifier: BSD-3-Clause
"""Optional LP-dual extras from a gtopt output directory.

Holds per-(scene, stage, block, uid) frames that are produced *only* by
a gtopt simulated run with the right `--write-out` flags, never by the
real-data feed-parquet path. Kept outside ``gtopt_canonical_feed.Cells``
because that schema is frozen at SCHEMA_VERSION 1.0.0.

The fields are all optional; consumers must guard with ``is not None``
before reading them. The :func:`load_gtopt_lp_duals` helper reads what is
present and silently skips what is not — :func:`check_write_out_flags`
is the place to fail loudly when something the analysis *needs* is
missing.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import pandas as pd

from gtopt_check_output._reader import read_table
from gtopt_marginal_units.errors import InputValidationError


# ---------------------------------------------------------------------------
# Long-form value-column names for the LP-dual extras.
# ---------------------------------------------------------------------------
COL_GEN_RC = "gen_reduced_cost"  # $/MWh
COL_GEN_SRMC = "gen_srmc"  # $/MWh — primary-segment VOM + fuel
COL_GEN_VOM = "gen_vom_cost"
COL_GEN_FUEL = "gen_fuel_cost"
COL_RES_WATER_VALUE = "reservoir_water_value"  # $/MWh-equivalent
COL_BAT_STORAGE_VALUE = "battery_storage_value"  # $/MWh
COL_LOAD_RC = "load_reduced_cost"  # $/MWh


# Parquet stems (under <output_dir>/) that this module knows how to read.
# Keep these constants in lock-step with the C++ output_context emit calls
# in source/{generator,storage,battery,reservoir,demand}_lp.cpp.
STEM_GEN_RC = "Generator/generation_cost"
STEM_GEN_SRMC = "Generator/srmc_sol"
STEM_GEN_VOM = "Generator/vom_cost_sol"
STEM_GEN_FUEL = "Generator/fuel_cost_sol"
STEM_RES_WATER_VALUE = "Reservoir/water_value_dual"
STEM_BAT_STORAGE_VALUE = "Battery/energy_dual"
STEM_LOAD_RC = "Demand/load_cost"


# ---------------------------------------------------------------------------
# Data container.
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class GtoptLpDuals:
    """Optional LP-dual frames read from a gtopt output directory.

    Each frame, when populated, is keyed on
    ``(scenario, stage, block, <entity>_uid)`` and carries a single
    physical-quantity column named per the ``COL_*`` constants above.
    """

    gen_reduced_cost: Optional[pd.DataFrame] = None
    gen_srmc: Optional[pd.DataFrame] = None
    gen_vom_cost: Optional[pd.DataFrame] = None
    gen_fuel_cost: Optional[pd.DataFrame] = None
    reservoir_water_value: Optional[pd.DataFrame] = None
    battery_storage_value: Optional[pd.DataFrame] = None
    load_reduced_cost: Optional[pd.DataFrame] = None

    @classmethod
    def empty(cls) -> "GtoptLpDuals":
        return cls()

    def has_gen_reduced_cost(self) -> bool:
        return self.gen_reduced_cost is not None and not self.gen_reduced_cost.empty

    def has_gen_srmc(self) -> bool:
        return self.gen_srmc is not None and not self.gen_srmc.empty

    def has_reservoir_water_value(self) -> bool:
        return (
            self.reservoir_water_value is not None
            and not self.reservoir_water_value.empty
        )

    def has_battery_storage_value(self) -> bool:
        return (
            self.battery_storage_value is not None
            and not self.battery_storage_value.empty
        )


# ---------------------------------------------------------------------------
# Reader.
# ---------------------------------------------------------------------------


def load_gtopt_lp_duals(output_dir: Path) -> GtoptLpDuals:
    """Read every LP-dual / reduced-cost parquet stem this module knows
    about. Missing stems silently produce ``None`` fields.

    Use :func:`check_write_out_flags` to enforce that the analysis-required
    stems are present.
    """
    output_dir = Path(output_dir)
    return GtoptLpDuals(
        gen_reduced_cost=_read_long(output_dir, STEM_GEN_RC, "gen_uid", COL_GEN_RC),
        gen_srmc=_read_long(output_dir, STEM_GEN_SRMC, "gen_uid", COL_GEN_SRMC),
        gen_vom_cost=_read_long(output_dir, STEM_GEN_VOM, "gen_uid", COL_GEN_VOM),
        gen_fuel_cost=_read_long(output_dir, STEM_GEN_FUEL, "gen_uid", COL_GEN_FUEL),
        reservoir_water_value=_read_long(
            output_dir, STEM_RES_WATER_VALUE, "reservoir_uid", COL_RES_WATER_VALUE
        ),
        battery_storage_value=_read_long(
            output_dir, STEM_BAT_STORAGE_VALUE, "battery_uid", COL_BAT_STORAGE_VALUE
        ),
        load_reduced_cost=_read_long(output_dir, STEM_LOAD_RC, "bus_uid", COL_LOAD_RC),
    )


# ---------------------------------------------------------------------------
# Flag check.
# ---------------------------------------------------------------------------

# Stems that the improved attribution pipeline strictly needs.  Only
# `Generator/generation_cost` is gated by the `--write-out` flag (it is
# the reduced-cost stream of the `generation` column).  `srmc_sol` is
# emitted unconditionally by modern gtopt builds but did not exist in
# pre-`4b75921a4` outputs, so it stays a nice-to-have refinement
# rather than a hard requirement.
REQUIRED_FOR_REDUCED_COST_ATTRIBUTION: tuple[str, ...] = (STEM_GEN_RC,)


def check_write_out_flags(
    output_dir: Path,
    *,
    require_reduced_cost: bool,
) -> None:
    """Raise :class:`InputValidationError` if reduced-cost analysis was
    requested but the gtopt output directory is missing the stems we
    need.

    The error message names every missing stem and tells the user the
    exact ``--write-out`` flag combination to re-run gtopt with. This is
    the fail-fast path for the post-``a3b026a93`` default
    (``write_out = sol|dual``) — users now have to opt in to reduced
    costs explicitly.
    """
    if not require_reduced_cost:
        return

    output_dir = Path(output_dir)
    missing = [
        stem
        for stem in REQUIRED_FOR_REDUCED_COST_ATTRIBUTION
        if read_table(output_dir, stem) is None
    ]
    if not missing:
        return

    listing = "\n  - ".join(missing)
    raise InputValidationError(
        "marginal-unit attribution requires gtopt reduced-cost output, "
        f"but the following parquet stems are missing under {output_dir}:\n"
        f"  - {listing}\n"
        "Re-run gtopt with `--write-out sol,dual,rc` (or `--write-out all`) "
        "and point this tool at the new output directory. The default since "
        "commit a3b026a93 (2026-05-18) is `--write-out sol,dual`, which "
        "omits reduced costs."
    )


# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------


def _read_long(
    output_dir: Path,
    stem: str,
    uid_col: str,
    value_col: str,
) -> Optional[pd.DataFrame]:
    """Read ``<output_dir>/<stem>.parquet`` and return its long-form
    melt. Returns ``None`` when the stem is absent or empty.
    """
    wide = read_table(output_dir, stem)
    if wide is None or wide.empty:
        return None

    key_cols = [c for c in ("scenario", "stage", "block") if c in wide.columns]
    uid_cols = [c for c in wide.columns if c.startswith("uid:")]
    if not uid_cols:
        return None

    melted = wide.melt(
        id_vars=key_cols,
        value_vars=uid_cols,
        var_name=uid_col,
        value_name=value_col,
    )
    melted[uid_col] = melted[uid_col].str.split(":").str[1].astype(int)
    melted[value_col] = melted[value_col].astype(float)
    return melted.reset_index(drop=True)


__all__ = [
    "COL_BAT_STORAGE_VALUE",
    "COL_GEN_FUEL",
    "COL_GEN_RC",
    "COL_GEN_SRMC",
    "COL_GEN_VOM",
    "COL_LOAD_RC",
    "COL_RES_WATER_VALUE",
    "GtoptLpDuals",
    "REQUIRED_FOR_REDUCED_COST_ATTRIBUTION",
    "STEM_BAT_STORAGE_VALUE",
    "STEM_GEN_FUEL",
    "STEM_GEN_RC",
    "STEM_GEN_SRMC",
    "STEM_GEN_VOM",
    "STEM_LOAD_RC",
    "STEM_RES_WATER_VALUE",
    "check_write_out_flags",
    "load_gtopt_lp_duals",
]
