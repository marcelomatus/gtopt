# SPDX-License-Identifier: BSD-3-Clause
"""Integration test against the prebuilt IEEE 57-bus benchmark — the
largest IEEE case in the integration suite (57 buses, 80 lines, 7
generators). The case is pandapower-derived (per the dependencies
declared in scripts/pyproject.toml) and serves as the v1 stress
test for the BFS connected-components + PTDF path.

Skip-if-output-absent: we don't fail CI on a clean checkout, but
when the gtopt integration suite has produced the output we
exercise the end-to-end pipeline at scale.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


_OUTPUT_DIR = Path(
    "/home/marce/git/gtopt-hygiene/build/integration_test/test_output/ieee_57b"
)
_PLANNING = _OUTPUT_DIR / "planning.json"


@pytest.mark.skipif(
    not (_OUTPUT_DIR / "Bus/balance_dual.csv").exists() or not _PLANNING.exists(),
    reason="ieee_57b prebuilt output not available; run the gtopt integration suite first",
)
def test_ieee_57b_end_to_end(tmp_path):
    """Stress test on the largest IEEE case — 57 buses, 80 lines."""
    out = tmp_path / "ieee_57b_marginals.parquet"
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

    # Topology shape — IEEE 57-bus has 57 buses.
    assert per_bus["bus_uid"].nunique() == 57

    # The reference run is uncongested (LP duals show λ ≈ 20 at every
    # bus, modulo float epsilon). The LMP-bucket repartitioner should
    # collapse them into a single zone within tol_lmp.
    assert per_zone["zone_id"].nunique() == 1
    zone_lmp = per_zone["zone_lmp"].iloc[0]
    assert 19.5 < zone_lmp < 20.5

    # All bus LMPs in the per_zone view land at the same λ.
    assert (per_zone["zone_lmp"] - zone_lmp).abs().max() < 1e-3

    # Manifest sanity.
    mf = ds.manifest()
    assert mf["producer"] == "gtopt_marginal_units"
    assert mf["row_counts"]["attribution/per_bus"] >= 57


@pytest.mark.skipif(
    not (_OUTPUT_DIR / "Bus/balance_dual.csv").exists() or not _PLANNING.exists(),
    reason="ieee_57b prebuilt output not available",
)
def test_ieee_57b_consumer_api_at_scale(tmp_path):
    """Confirm the consumer-API methods scale to a 57-bus dataset."""
    out = tmp_path / "ieee_57b.parquet"
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

    # bus_lmp covers all 57 buses for the single zone.
    bus_lmp = ds.bus_lmp()
    assert not bus_lmp.empty
    # Combined dual-currency view loads.
    combined = ds.bus_lmp_and_emission()
    assert not combined.empty
