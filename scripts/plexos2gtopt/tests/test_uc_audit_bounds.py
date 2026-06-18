# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the B12 parameter-bounds consistency check in
:mod:`plexos2gtopt.uc_audit`.

The B12 bucket compares converted gtopt generator/line bounds against the
raw PLEXOS input CSV time-series and flags any divergence beyond tolerance.
The key regression target is the Chacao cable (``PMontt220->Chiloe110``):
``Lin_MaxRating.csv`` = 90 MW but the converter emits ``tmax_ab`` = 180/450,
which B12 must report as a mismatch.
"""

from __future__ import annotations

import json
from pathlib import Path

from plexos2gtopt.uc_audit import (
    AuditInputs,
    build_b12_bounds,
    run_audit,
)


def _write_max_rating_csv(path: Path, name: str, value: float) -> None:
    path.write_text(
        f"NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n{name},2025,11,9,1,1,{value}\n"
    )


def _write_gen_csv(path: Path, name: str, value: float) -> None:
    """Write a single-row long-format generator CSV (period 1, band 1)."""
    path.write_text(
        f"NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n{name},2025,11,9,1,1,{value}\n"
    )


def test_b12_flags_chacao_line(tmp_path: Path) -> None:
    """A line at gtopt tmax_ab=180 vs Lin_MaxRating=90 is reported."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [
                        {
                            "uid": 219,
                            "name": "PMontt220->Chiloe110",
                            "tmax_ab": 180.0,
                            "tmax_ba": 180.0,
                            "tmax_normal_ab": 180.0,
                            "tmax_normal_ba": 180.0,
                        },
                    ],
                    "generator_array": [],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_max_rating_csv(input_dir / "Lin_MaxRating.csv", "PMontt220->Chiloe110", 90.0)

    items = build_b12_bounds(gtopt_json, input_dir)
    by_field = {it["field"]: it for it in items}
    assert "tmax_ab" in by_field
    chacao = by_field["tmax_ab"]
    assert chacao["name"] == "PMontt220->Chiloe110"
    assert chacao["gtopt"] == 90.0 + 90.0  # 180
    assert chacao["plexos"] == 90.0
    assert chacao["delta"] == 90.0
    # all four tmax fields flagged
    assert {"tmax_ab", "tmax_ba", "tmax_normal_ab", "tmax_normal_ba"} <= set(by_field)


def test_b12_within_tolerance_not_flagged(tmp_path: Path) -> None:
    """A line whose gtopt rating matches the CSV is NOT reported."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [
                        {
                            "name": "A->B",
                            "tmax_ab": 90.0,
                            "tmax_ba": 90.0,
                            "tmax_normal_ab": 90.0,
                            "tmax_normal_ba": 90.0,
                        },
                    ],
                    "generator_array": [],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_max_rating_csv(input_dir / "Lin_MaxRating.csv", "A->B", 90.0)

    items = build_b12_bounds(gtopt_json, input_dir)
    assert not items


def test_b12_flags_generator_pmax(tmp_path: Path) -> None:
    """A generator whose pmax diverges from Gen_Rating is reported."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [],
                    "generator_array": [
                        {"name": "GEN1", "pmin": None, "pmax": 0.0},
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    (input_dir / "Gen_Rating.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nGEN1,2025,11,9,1,1,108.5\n"
    )

    items = build_b12_bounds(gtopt_json, input_dir)
    assert len(items) == 1
    assert items[0]["name"] == "GEN1"
    assert items[0]["field"] == "pmax"
    assert items[0]["plexos"] == 108.5


def _write_gen_json(path: Path, name: str, pmin: float | None, pmax: float) -> None:
    path.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [],
                    "generator_array": [
                        {"name": name, "pmin": pmin, "pmax": pmax},
                    ],
                }
            }
        )
    )


def test_b12_units_out_generator_not_flagged(tmp_path: Path) -> None:
    """A forced-out generator (Gen_UnitsOut=1) whose gtopt pmax was
    correctly zeroed is NOT flagged, even though Gen_Rating > 0.

    Mirrors the real CEN diesels (LOS_CONDORES_U1, ATA-TG1A_DIE, ...):
    UnitsOut=1, Commit=-1, Rating>0 → converter emits pmax=0.
    """
    gtopt_json = tmp_path / "DATOS.json"
    _write_gen_json(gtopt_json, "LOS_CONDORES_U1", pmin=None, pmax=0.0)
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_gen_csv(input_dir / "Gen_Rating.csv", "LOS_CONDORES_U1", 81.25)
    _write_gen_csv(input_dir / "Gen_UnitsOut.csv", "LOS_CONDORES_U1", 1)
    # Commit=-1 is "endogenous / no commitment", NOT forced-off — it must
    # NOT influence the expected bound; UnitsOut alone zeroes pmax.
    _write_gen_csv(input_dir / "Gen_Commit.csv", "LOS_CONDORES_U1", -1)

    items = build_b12_bounds(gtopt_json, input_dir)
    assert not items


def test_b12_fixed_load_generator_not_flagged(tmp_path: Path) -> None:
    """A Fixed Load generator (ARAUCO_MAPA_BL1: FixedLoad=36) whose gtopt
    pmin=pmax=36 is NOT flagged even though Gen_MinStableLevel=3.

    Fixed Load is a hard required-generation equality that OVERRIDES
    MinStableLevel for non-renewable units.
    """
    gtopt_json = tmp_path / "DATOS.json"
    _write_gen_json(gtopt_json, "ARAUCO_MAPA_BL1", pmin=36.0, pmax=36.0)
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_gen_csv(input_dir / "Gen_Rating.csv", "ARAUCO_MAPA_BL1", 36.0)
    _write_gen_csv(input_dir / "Gen_MinStableLevel.csv", "ARAUCO_MAPA_BL1", 3.0)
    _write_gen_csv(input_dir / "Gen_FixedLoad.csv", "ARAUCO_MAPA_BL1", 36.0)
    _write_gen_csv(input_dir / "Gen_UnitsOut.csv", "ARAUCO_MAPA_BL1", 0)
    _write_gen_csv(input_dir / "Gen_Commit.csv", "ARAUCO_MAPA_BL1", 1)

    items = build_b12_bounds(gtopt_json, input_dir)
    # Neither pmax (36==36) nor pmin (36 vs expected fixed-load 36) fires;
    # the legacy MinStableLevel=3 comparison is correctly overridden.
    assert not items


def test_b12_genuine_pmax_mismatch_still_flagged(tmp_path: Path) -> None:
    """An AVAILABLE generator (no units out, no Fixed Load) whose gtopt
    pmax diverges from Gen_Rating IS still flagged — the fix must not
    silence genuine converter bugs.
    """
    gtopt_json = tmp_path / "DATOS.json"
    _write_gen_json(gtopt_json, "GENX", pmin=None, pmax=50.0)
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_gen_csv(input_dir / "Gen_Rating.csv", "GENX", 108.5)
    _write_gen_csv(input_dir / "Gen_UnitsOut.csv", "GENX", 0)
    _write_gen_csv(input_dir / "Gen_FixedLoad.csv", "GENX", 0)
    _write_gen_csv(input_dir / "Gen_Commit.csv", "GENX", -1)

    items = build_b12_bounds(gtopt_json, input_dir)
    assert len(items) == 1
    assert items[0]["name"] == "GENX"
    assert items[0]["field"] == "pmax"
    assert items[0]["gtopt"] == 50.0
    assert items[0]["plexos"] == 108.5


def test_b12_wired_into_run_audit(tmp_path: Path) -> None:
    """``run_audit`` populates the B12 bucket + summary count when an
    input dir is supplied; the Chacao mismatch surfaces end-to-end."""
    # Minimal PLEXOS sol cache (empty UC side — B12 is independent of it).
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "t_object.csv").write_text("object_id,name,class_id\n")
    (cache / "t_membership.csv").write_text(
        "membership_id,collection_id,child_class_id,child_object_id\n"
    )
    (cache / "t_key.csv").write_text("key_id,membership_id,property_id\n")
    (cache / "t_data_0.csv").write_text("key_id,value\n")

    gtopt_dir = tmp_path / "gtopt"
    gtopt_dir.mkdir()
    gtopt_json = gtopt_dir / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [
                        {
                            "name": "PMontt220->Chiloe110",
                            "tmax_ab": 450.0,
                            "tmax_ba": 450.0,
                            "tmax_normal_ab": 180.0,
                            "tmax_normal_ba": 180.0,
                        },
                    ],
                    "generator_array": [],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_max_rating_csv(input_dir / "Lin_MaxRating.csv", "PMontt220->Chiloe110", 90.0)

    result = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=gtopt_dir,
            gtopt_json=gtopt_json,
            plexos_input_dir=input_dir,
        )
    )
    b12 = result.buckets.get("B12_bounds_mismatch", [])
    names = {(it["name"], it["field"]) for it in b12}
    assert ("PMontt220->Chiloe110", "tmax_ab") in names
    assert result.summary["bucket_counts"]["B12_bounds_mismatch"] == len(b12)
    # JSON serialisation includes the bucket.
    assert "B12_bounds_mismatch" in result.to_dict()["buckets"]
