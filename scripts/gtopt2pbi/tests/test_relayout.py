# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the gtopt2pbi relayout tool."""

import pandas as pd
import pyarrow.parquet as pq
import pytest

from gtopt2pbi.main import main, relayout_tree


def _write(path, df, compression="lz4"):
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_parquet(path, index=False, compression=compression)


def test_relayout_reshapes_wide_field_table(tmp_path):
    """A wide lz4 field table becomes long + the target codec, in place."""
    f = tmp_path / "Generator" / "pmax.parquet"
    _write(
        f, pd.DataFrame({"block": [1, 2], "uid:1": [10.0, 20.0], "uid:7": [1.0, 2.0]})
    )

    assert relayout_tree(tmp_path, compression="zstd") == 1

    out = pd.read_parquet(f)
    assert set(out.columns) == {"block", "uid", "value"}
    assert "uid:1" not in out.columns
    assert float(
        out[(out["block"] == 1) & (out["uid"] == 7)]["value"].iloc[0]
    ) == pytest.approx(1.0)
    # Codec is now zstd (was lz4).
    assert pq.ParquetFile(f).metadata.row_group(0).column(0).compression == "ZSTD"


def test_relayout_leaves_structural_untouched(tmp_path):
    """A non-field (structural) table is left exactly as-is — not even recoded."""
    f = tmp_path / "block.parquet"
    _write(f, pd.DataFrame({"block": [1, 2], "duration": [1.0, 2.0]}))
    before = f.read_bytes()

    assert relayout_tree(tmp_path, compression="zstd") == 0
    assert f.read_bytes() == before  # byte-identical: untouched


def test_relayout_dry_run_writes_nothing(tmp_path):
    """--dry-run reports the count but leaves files untouched."""
    f = tmp_path / "Demand" / "lmax.parquet"
    _write(f, pd.DataFrame({"block": [1], "uid:1": [80.0]}), compression="lz4")
    before = f.read_bytes()

    assert relayout_tree(tmp_path, compression="zstd", dry_run=True) == 1
    assert f.read_bytes() == before  # unchanged
    assert pq.ParquetFile(f).metadata.row_group(0).column(0).compression == "LZ4"


def test_relayout_idempotent(tmp_path):
    """Running twice is safe: an already-long file is left untouched."""
    f = tmp_path / "Generator" / "pmax.parquet"
    _write(f, pd.DataFrame({"stage": [1, 2], "uid:1": [10.0, 20.0]}))
    assert relayout_tree(tmp_path, compression="zstd") == 1
    after_first = f.read_bytes()
    assert relayout_tree(tmp_path, compression="zstd") == 0  # already long → skip
    assert f.read_bytes() == after_first  # byte-identical second pass
    assert set(pd.read_parquet(f).columns) == {"stage", "uid", "value"}


def test_relayout_skips_hive_output_leaves(tmp_path):
    """Hive-partitioned solve-output `part.parquet` leaves are left untouched."""
    inp = tmp_path / "Generator" / "pmax.parquet"  # flat input → convert
    _write(inp, pd.DataFrame({"block": [1, 2], "uid:1": [10.0, 20.0]}))
    leaf = (  # hive output leaf inside a `*.parquet/` directory → skip
        tmp_path
        / "results"
        / "Generator"
        / "generation_sol.parquet"
        / "scene=1"
        / "phase=1"
        / "part.parquet"
    )
    _write(
        leaf,
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [5.0]}),
    )
    leaf_before = leaf.read_bytes()

    assert relayout_tree(tmp_path, compression="zstd") == 1  # only the flat input
    assert leaf.read_bytes() == leaf_before  # hive output leaf untouched
    assert set(pd.read_parquet(inp).columns) == {"block", "uid", "value"}


def test_main_errors_on_missing_dir(tmp_path):
    """main() returns exit code 2 for a non-directory argument."""
    assert main([str(tmp_path / "does_not_exist")]) == 2


def test_main_converts(tmp_path, capsys):
    """End-to-end CLI run reshapes and reports."""
    f = tmp_path / "Flow" / "discharge.parquet"
    _write(f, pd.DataFrame({"scenario": [1, 1], "block": [1, 2], "uid:9": [3.0, 4.0]}))
    rc = main([str(tmp_path), "-c", "zstd"])
    assert rc == 0
    assert "converted 1 wide field file(s) to long" in capsys.readouterr().out
    assert set(pd.read_parquet(f).columns) == {"scenario", "block", "uid", "value"}
