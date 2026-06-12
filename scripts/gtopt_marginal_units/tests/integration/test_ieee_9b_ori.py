# SPDX-License-Identifier: BSD-3-Clause
"""Integration test against the prebuilt IEEE 9b_ori benchmark output.

Skip-if-output-absent: we don't fail CI on a clean checkout, but
when the gtopt integration suite has produced the output we
exercise the end-to-end pipeline and assert the master plan §7.2
acceptance properties.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


_OUTPUT_DIR = Path(
    "/home/marce/git/gtopt-hygiene/build/integration_test/test_output/ieee_9b_ori"
)
_PLANNING = _OUTPUT_DIR / "planning.json"


@pytest.mark.skipif(
    not (_OUTPUT_DIR / "Bus/balance_dual.csv").exists() or not _PLANNING.exists(),
    reason="ieee_9b_ori prebuilt output not available; run the gtopt integration suite first",
)
def test_ieee_9b_ori_end_to_end(tmp_path):
    out = tmp_path / "ieee_9b_ori_marginals.parquet"
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
        ]
    )
    assert code in (EXIT_OK, EXIT_UNATTRIBUTED)

    ds = MarginalUnitDataset.open(out)
    per_bus = ds.per_bus()
    per_zone = ds.per_zone()

    # IEEE 9-bus has 9 buses.
    assert per_bus["bus_uid"].nunique() == 9

    # Uncongested → most cells should have zone_lmp around the merit
    # threshold (gcost of the most-expensive interior unit). The
    # benchmark's Bus/balance_dual reports λ_b = 30 at most buses,
    # 20 at bus 1 (the cheapest gen's bus); since we copy LP duals
    # over the reconstruction, per_zone.zone_lmp must reflect those.
    assert per_zone["zone_lmp"].max() >= 20.0
    assert per_zone["zone_lmp"].max() <= 1000.0  # below demand_fail_cost


@pytest.mark.skipif(
    not (_OUTPUT_DIR / "Bus/balance_dual.csv").exists() or not _PLANNING.exists(),
    reason="ieee_9b_ori prebuilt output not available",
)
def test_ieee_9b_ori_consumer_api_methods(tmp_path):
    out = tmp_path / "ieee_9b_ori.parquet"
    cli(
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
        ]
    )
    ds = MarginalUnitDataset.open(out)
    bus_lmp = ds.bus_lmp()
    assert not bus_lmp.empty

    # bus_lmp_and_emission combined view.
    combined = ds.bus_lmp_and_emission()
    assert not combined.empty
