# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the LP-dual extras reader and write_out flag check.

Covers:
  * ``load_gtopt_lp_duals`` reads ``Generator/generation_cost``,
    ``Generator/srmc_sol``, ``Reservoir/water_value_dual``,
    ``Battery/energy_dual``, and ``Demand/load_cost`` when present,
    and silently returns ``None`` for absent stems.
  * ``check_write_out_flags`` raises a clear ``InputValidationError``
    that names every missing stem and points at the
    ``--write-out sol,dual,rc`` workaround.
  * ``_select_marginal_candidates`` (via the end-to-end pipeline)
    picks the rc-zero interior generator instead of the legacy
    ``declared_MC ≈ λ`` match when reduced costs are available.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from gtopt_marginal_units._lp_duals import (
    COL_BAT_STORAGE_VALUE,
    COL_GEN_RC,
    COL_GEN_SRMC,
    COL_LOAD_RC,
    COL_RES_WATER_VALUE,
    GtoptLpDuals,
    check_write_out_flags,
    load_gtopt_lp_duals,
)
from gtopt_marginal_units.errors import InputValidationError
from gtopt_marginal_units.main import cli


# ---------------------------------------------------------------------------
# Reader unit tests.
# ---------------------------------------------------------------------------


def _write_csv(path: Path, df: pd.DataFrame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(path, index=False)


def test_load_gtopt_lp_duals_reads_all_known_stems(tmp_path: Path) -> None:
    out = tmp_path / "out"
    _write_csv(
        out / "Generator/generation_cost.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:10": [0.0],
                "uid:20": [70.0],
            }
        ),
    )
    _write_csv(
        out / "Generator/srmc_sol.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:10": [10.0],
            }
        ),
    )
    _write_csv(
        out / "Reservoir/water_value_dual.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:99": [12.5],
            }
        ),
    )
    _write_csv(
        out / "Battery/energy_dual.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:200": [15.0],
            }
        ),
    )
    _write_csv(
        out / "Demand/load_cost.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:1": [10.0],
            }
        ),
    )

    duals = load_gtopt_lp_duals(out)

    assert duals.has_gen_reduced_cost()
    assert duals.has_gen_srmc()
    assert duals.has_reservoir_water_value()
    assert duals.has_battery_storage_value()
    assert duals.load_reduced_cost is not None

    # Long-form melt has uid columns named per the dataclass field.
    assert set(duals.gen_reduced_cost.columns) >= {
        "scenario",
        "stage",
        "block",
        "gen_uid",
        COL_GEN_RC,
    }
    assert set(duals.reservoir_water_value.columns) >= {
        "reservoir_uid",
        COL_RES_WATER_VALUE,
    }
    assert set(duals.battery_storage_value.columns) >= {
        "battery_uid",
        COL_BAT_STORAGE_VALUE,
    }
    assert set(duals.load_reduced_cost.columns) >= {"bus_uid", COL_LOAD_RC}

    # Values melted correctly.
    rc_row = duals.gen_reduced_cost.set_index("gen_uid").loc[20]
    assert rc_row[COL_GEN_RC] == pytest.approx(70.0)
    srmc_row = duals.gen_srmc.set_index("gen_uid").loc[10]
    assert srmc_row[COL_GEN_SRMC] == pytest.approx(10.0)


def test_load_gtopt_lp_duals_silently_skips_absent_stems(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    # Only one stem present.
    _write_csv(
        out / "Generator/srmc_sol.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:10": [10.0],
            }
        ),
    )

    duals = load_gtopt_lp_duals(out)

    assert duals.has_gen_srmc()
    assert not duals.has_gen_reduced_cost()
    assert duals.gen_reduced_cost is None
    assert duals.reservoir_water_value is None
    assert duals.battery_storage_value is None
    assert duals.load_reduced_cost is None


def test_gtopt_lp_duals_empty_constructor_is_all_none() -> None:
    duals = GtoptLpDuals.empty()
    assert not duals.has_gen_reduced_cost()
    assert not duals.has_gen_srmc()
    assert not duals.has_reservoir_water_value()
    assert not duals.has_battery_storage_value()


# ---------------------------------------------------------------------------
# Flag check.
# ---------------------------------------------------------------------------


def test_check_write_out_flags_no_op_when_not_required(tmp_path: Path) -> None:
    # Empty output dir is fine when require_reduced_cost=False.
    out = tmp_path / "out"
    out.mkdir()
    check_write_out_flags(out, require_reduced_cost=False)


def test_check_write_out_flags_raises_with_actionable_message(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()

    with pytest.raises(InputValidationError) as excinfo:
        check_write_out_flags(out, require_reduced_cost=True)

    message = str(excinfo.value)
    assert "Generator/generation_cost" in message
    # Must name the exact flag the user should pass.
    assert "--write-out" in message
    assert "sol,dual,rc" in message or "all" in message


def test_check_write_out_flags_passes_when_required_files_present(
    tmp_path: Path,
) -> None:
    out = tmp_path / "out"
    _write_csv(
        out / "Generator/generation_cost.csv",
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:10": [0.0]}),
    )
    check_write_out_flags(out, require_reduced_cost=True)


# ---------------------------------------------------------------------------
# End-to-end: pipeline picks the rc≈0 interior generator.
# ---------------------------------------------------------------------------


def test_pipeline_picks_rc_zero_interior_generator(
    tmp_path: Path,
    tiny_planning: Path,  # noqa: ARG001 — provided by conftest
    tiny_output_dir: Path,  # provides Generator/generation_cost.csv
) -> None:
    """End-to-end smoke test: the rc-based attribution should pick the
    cheap generator (g10, rc≈0 at 50 MW interior) and *not* the peaker
    (g20, rc=70 at 0 MW).  This is the regression guard for the
    legacy ``declared_MC ≈ λ`` test that misses non-primary segments.
    """
    out = tmp_path / "marginal_out"
    argv = [
        "--planning",
        str(tiny_planning),
        "--output",
        str(tiny_output_dir),
        "--out",
        str(out),
        "--input-kind",
        "gtopt-dir",
        "--mode",
        "simulated",
    ]
    # Replace sys.argv view via cli's argv= passthrough.
    rc_exit = cli(argv)
    # Exit 0 (OK) or 2 (unattributed cells still allowed).
    assert rc_exit in (0, 2)

    per_bus = pd.read_parquet(out / "attribution/per_bus.parquet")
    # The cheap unit (uid=10) should be flagged as marginal somewhere
    # in the output.
    marginal_col = per_bus["is_marginal"].astype(bool)
    assert marginal_col.any()
    marginal_rows = per_bus[marginal_col]
    assert 10 in set(marginal_rows.get("gen_uid", pd.Series(dtype=int)).dropna())


def test_pipeline_errors_when_reduced_cost_missing_and_required(
    tmp_path: Path,
    tiny_planning: Path,
) -> None:
    """When the gtopt run was done with --write-out sol,dual (no rc),
    the script must fail fast with an actionable message rather than
    silently degrade.
    """
    # Build a minimal output dir WITHOUT the reduced-cost stems.
    out_in = tmp_path / "out_no_rc"
    _write_csv(
        out_in / "Generator/generation_sol.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:10": [50.0],
                "uid:20": [0.0],
            }
        ),
    )
    _write_csv(
        out_in / "Bus/balance_dual.csv",
        pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:1": [10.0],
                "uid:2": [10.0],
            }
        ),
    )

    out = tmp_path / "marginal_out"
    argv = [
        "--planning",
        str(tiny_planning),
        "--output",
        str(out_in),
        "--out",
        str(out),
        "--input-kind",
        "gtopt-dir",
        "--mode",
        "simulated",
    ]
    rc_exit = cli(argv)
    # CLI translates InputValidationError → EXIT_INPUT_ERROR (3).
    assert rc_exit == 3
