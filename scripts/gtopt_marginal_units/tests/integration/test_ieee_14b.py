# SPDX-License-Identifier: BSD-3-Clause
"""Integration test against the prebuilt IEEE 14b benchmark — a
deliberately-congested case (line saturations confirmed by the
master plan §3.2 grep on flowp_cost)."""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


_OUTPUT_DIR = Path(
    "/home/marce/git/gtopt-hygiene/build/integration_test/test_output/ieee_14b"
)
_PLANNING = _OUTPUT_DIR / "planning.json"


@pytest.mark.skipif(
    not (_OUTPUT_DIR / "Bus/balance_dual.csv").exists() or not _PLANNING.exists(),
    reason="ieee_14b prebuilt output not available",
)
def test_ieee_14b_runs_to_completion(tmp_path):
    out = tmp_path / "ieee_14b_marginals.parquet"
    code = cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "simulated",
            "--planning",
            str(_PLANNING),
            "--output",
            str(_OUTPUT_DIR),
            "--out",
            str(out),
            # IEEE 14b is a deliberately-stressed congestion benchmark;
            # bus_LMP / SRMC ratios reach ~25 on the saturated zones.
            # Raise the loss-factor-error guard so the pipeline runs to
            # completion (production CEN cases keep the default 5.0).
            "--loss-factor-error",
            "100.0",
        ]
    )
    assert code in (EXIT_OK, EXIT_UNATTRIBUTED)

    ds = MarginalUnitDataset.open(out)
    per_bus = ds.per_bus()
    per_zone = ds.per_zone()
    assert not per_bus.empty
    assert not per_zone.empty

    # IEEE 14-bus has 14 buses.
    assert per_bus["bus_uid"].nunique() == 14

    # The benchmark exhibits congestion (flowp_cost values in the
    # 50-150 range, see master §3.2 grep). With v1 zones=1 fallback
    # we still get at least one zone; the test is a smoke check that
    # the pipeline doesn't crash on a real congested case.
    assert per_zone["zone_id"].nunique() >= 1
