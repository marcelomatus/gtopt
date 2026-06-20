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
    build_b13_profile_collapse,
    build_b14_reservoir_bounds,
    run_audit,
)


def _write_wide_volume_csv(path: Path, col: str, per_hour: list[float]) -> None:
    """Write a 1-day WIDE Hydro_*Volume.csv (YEAR,MONTH,DAY,PERIOD,<col>)."""
    lines = ["YEAR,MONTH,DAY,PERIOD," + col]
    for h, v in enumerate(per_hour, start=1):
        lines.append(f"2025,10,19,{h},{v}")
    path.write_text("\n".join(lines) + "\n")


def test_b14_flags_emax_cap_below_input(tmp_path: Path) -> None:
    """B14 flags a reservoir whose gtopt emax caps BELOW the PLEXOS Max Volume
    peak (the CANUTILLAR 12,331 -> 10,570 silent collapse)."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [],
                    "reservoir_array": [
                        {"name": "CANUTILLAR", "emin": 5205.0, "emax": 10569.6},
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_wide_volume_csv(
        input_dir / "Hydro_MaxVolume.csv", "CANUTILLAR", [12330.8] * 23 + [10569.6]
    )
    by_kind = {
        (it["name"], it["kind"]): it
        for it in build_b14_reservoir_bounds(gtopt_json, input_dir)
    }
    key = ("CANUTILLAR", "emax_peak_below_input")
    assert key in by_kind
    assert by_kind[key]["input_peak"] == 12330.8
    assert by_kind[key]["gtopt_peak"] == 10569.6
    # The varying input + scalar gtopt value is ALSO a collapsed profile.
    assert ("CANUTILLAR", "profile_collapsed") in by_kind


def test_b14_profile_emax_not_flagged(tmp_path: Path) -> None:
    """A reservoir emitting the full per-block emax profile (carrying the high
    intra-day cap) is NOT flagged."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [],
                    "reservoir_array": [
                        {
                            "name": "CANUTILLAR",
                            "emin": 5205.0,
                            "emax": [[12330.8, 10569.6]],
                        },
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_wide_volume_csv(
        input_dir / "Hydro_MaxVolume.csv", "CANUTILLAR", [12330.8] * 23 + [10569.6]
    )
    assert not build_b14_reservoir_bounds(gtopt_json, input_dir)


def test_b14_constant_input_not_flagged(tmp_path: Path) -> None:
    """A constant Max Volume series emitted as a scalar is correct."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [],
                    "reservoir_array": [
                        {"name": "COLBUN", "emin": 4416.9, "emax": 17977.4},
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_wide_volume_csv(input_dir / "Hydro_MaxVolume.csv", "COLBUN", [17977.4] * 24)
    assert not build_b14_reservoir_bounds(gtopt_json, input_dir)


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


def _write_cfdata_mru(
    root: Path, gen: str, direction: str, values: list[float]
) -> None:
    """Write a CFdata/CPF/CPF_<gen>_<dir>.csv with VALUE as the last column."""
    d = root / "CFdata" / "CPF"
    d.mkdir(parents=True, exist_ok=True)
    rows = ["NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE"]
    for i, v in enumerate(values, start=1):
        rows.append(f"{gen},2025,11,9,{i},1,{v}")
    (d / f"CPF_{gen}_{direction}.csv").write_text("\n".join(rows) + "\n")


def test_b12_flags_reserve_provision_cap(tmp_path: Path) -> None:
    """A reserve provision whose emitted urmax peak disagrees with the CFdata
    MRU peak is reported; a matching one is not.  Guards the B12 extension that
    closed the reserve-provision-cap blind spot."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "line_array": [],
                    "generator_array": [],
                    "reserve_provision_array": [
                        # peak 100 matches CFdata peak 100 → no flag
                        {"name": "p_ok", "generator": "GEN1", "urmax": [[0.0, 100.0]]},
                        # peak 50 vs CFdata peak 100 → flagged
                        {"name": "p_bad", "generator": "GEN2", "urmax": [[0.0, 50.0]]},
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_cfdata_mru(input_dir, "GEN1", "MRU", [0.0, 100.0])
    _write_cfdata_mru(input_dir, "GEN2", "MRU", [0.0, 100.0])

    items = build_b12_bounds(gtopt_json, input_dir)
    by_name = {it["name"]: it for it in items}
    assert "p_ok (GEN1)" not in by_name  # peak ties → not flagged
    bad = by_name.get("p_bad (GEN2)")
    assert bad is not None and bad["field"] == "urmax"
    assert bad["gtopt"] == 50.0
    assert bad["plexos"] == 100.0


def test_b13_flags_battery_power_collapse(tmp_path: Path) -> None:
    """B13 flags a varying Gen_Rating emitted as a SCALAR cap (the blind spot
    B12's peak-vs-peak misses), and passes a properly per-block profile."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [],
                    "battery_array": [
                        # varying rating but scalar cap → collapse, flagged
                        {"name": "BAT_DLR", "pmax_discharge": 110.0},
                        # varying rating emitted as a per-block profile → OK
                        {"name": "BAT_OK", "pmax_discharge": [[72.0, 110.0]]},
                    ],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    (input_dir / "Gen_Rating.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "BAT_DLR,2026,1,1,1,1,72\nBAT_DLR,2026,1,1,2,1,110\n"
        "BAT_OK,2026,1,1,1,1,72\nBAT_OK,2026,1,1,2,1,110\n"
    )
    by_name = {
        it["name"]: it for it in build_b13_profile_collapse(gtopt_json, input_dir)
    }
    assert "BAT_DLR" in by_name  # scalar cap on a varying rating → flagged
    assert by_name["BAT_DLR"]["field"] == "battery.pmax_discharge"
    assert by_name["BAT_DLR"]["gtopt_scalar"] == 110.0
    assert by_name["BAT_DLR"]["input_range"] == [72.0, 110.0]
    assert "BAT_OK" not in by_name  # per-block profile → not flagged


def test_b13_constant_rating_not_flagged(tmp_path: Path) -> None:
    """A constant Gen_Rating emitted as a scalar is correct — not flagged."""
    gtopt_json = tmp_path / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [{"name": "G1", "pmax": 50.0}],
                    "battery_array": [],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    (input_dir / "Gen_Rating.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "G1,2026,1,1,1,1,50\nG1,2026,1,1,2,1,50\n"
    )
    assert not build_b13_profile_collapse(gtopt_json, input_dir)


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
