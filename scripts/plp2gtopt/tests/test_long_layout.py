# SPDX-License-Identifier: BSD-3-Clause
"""Tests for long-layout field output (``plp2gtopt --layout long``, the
default) and the wide↔long reshape helpers in :mod:`plp2gtopt.base_writer`.

gtopt reads long input transparently (it auto-detects the layout and pivots
long→wide at load), so plp2gtopt emits the tidy ``[<index cols>, uid, value]``
shape by default — read natively by Power BI / Power Query without an unpivot.
The wide ``uid:N`` shape remains available via ``--layout wide`` and is covered
by the assertions in the other integration modules.
"""

from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.base_writer import convert_tree_to_long, to_long_layout
from plp2gtopt.plp2gtopt import convert_plp_case

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLP_MIN_1BUS = _CASES_DIR / "plp_min_1bus"


def test_to_long_layout_basic():
    """A wide field table reshapes to dense ``[index, uid, value]``."""
    wide = pd.DataFrame({"block": [1, 2], "uid:1": [80.0, 90.0], "uid:2": [10.0, 20.0]})
    long_df = to_long_layout(wide)
    assert long_df is not None
    assert list(long_df.columns) == ["block", "uid", "value"]
    assert len(long_df) == 4  # 2 blocks × 2 uids (dense)
    assert long_df["uid"].dtype == np.int32

    row = long_df[(long_df["block"] == 1) & (long_df["uid"] == 1)]
    assert float(row["value"].iloc[0]) == pytest.approx(80.0)
    row2 = long_df[(long_df["block"] == 2) & (long_df["uid"] == 2)]
    assert float(row2["value"].iloc[0]) == pytest.approx(20.0)


def test_to_long_layout_skips_structural():
    """Tables that are not wide field tables pass through (return None)."""
    # No uid:N value column → structural (e.g. a block/duration table).
    assert (
        to_long_layout(pd.DataFrame({"block": [1, 2], "duration": [1.0, 2.0]})) is None
    )
    # No index column.
    assert to_long_layout(pd.DataFrame({"uid:1": [1.0]})) is None
    # Empty frame.
    assert to_long_layout(pd.DataFrame()) is None


def test_to_long_layout_name_prefix():
    """``<name>:N`` columns (use_uid_label=False) still parse to uid N."""
    wide = pd.DataFrame({"stage": [1], "BUS:5": [3.0]})
    long_df = to_long_layout(wide)
    assert long_df is not None
    assert int(long_df["uid"].iloc[0]) == 5
    assert float(long_df["value"].iloc[0]) == pytest.approx(3.0)


def test_convert_tree_to_long(tmp_path):
    """``convert_tree_to_long`` rewrites field files long, skips structural."""
    gen = tmp_path / "Generator"
    gen.mkdir()
    pd.DataFrame({"block": [1, 2], "uid:1": [10.0, 20.0]}).to_parquet(
        gen / "pmax.parquet", index=False
    )
    struct = tmp_path / "block.parquet"
    pd.DataFrame({"block": [1, 2], "duration": [1.0, 2.0]}).to_parquet(
        struct, index=False
    )

    n_converted = convert_tree_to_long(tmp_path, {"compression": "zstd"})
    assert n_converted == 1

    field = pd.read_parquet(gen / "pmax.parquet")
    assert set(field.columns) == {"block", "uid", "value"}
    assert float(
        field[(field["block"] == 1) & (field["uid"] == 1)]["value"].iloc[0]
    ) == pytest.approx(10.0)

    # Structural table untouched.
    rebuilt = pd.read_parquet(struct)
    assert "duration" in rebuilt.columns
    assert "uid" not in rebuilt.columns


@pytest.mark.integration
def test_convert_plp_case_emits_long_lmax(tmp_path):
    """convert_plp_case (default long) writes Demand/lmax in long layout."""
    out_dir = tmp_path / "plp_min_1bus"
    out_dir.mkdir(parents=True, exist_ok=True)
    opts = {
        "input_dir": _PLP_MIN_1BUS,
        "output_dir": out_dir,
        "output_file": out_dir / "plp_min_1bus.json",
        "hydrologies": "1",
        "layout": "long",  # explicit, though it is the default
    }
    convert_plp_case(opts)

    lmax = out_dir / "Demand" / "lmax.parquet"
    assert lmax.exists(), "Demand/lmax.parquet not written"
    df = pd.read_parquet(lmax)
    assert {"block", "uid", "value"}.issubset(df.columns)
    assert "uid:1" not in df.columns  # not wide
    row = df[(df["block"] == 1) & (df["uid"] == 1)]
    assert len(row) == 1
    assert float(row["value"].iloc[0]) == pytest.approx(80.0)
