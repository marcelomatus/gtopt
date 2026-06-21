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
    RESERVE_CLASS_ID,
    RESERVE_PROP_PRICE,
    RESERVE_PROP_SHORTAGE,
    STORAGE_COLLECTION_ID,
    STORAGE_PROP_END_VOLUME,
    STORAGE_PROP_MAX_VOLUME,
    STORAGE_PROP_MIN_VOLUME,
    SYS_RESERVE_COLLECTION_ID,
    AuditInputs,
    build_b12_bounds,
    build_b13_profile_collapse,
    build_b14_reservoir_bounds,
    build_b14_reservoir_bounds_sol,
    build_b15_fuel_price,
    build_b16_turbine_pf,
    build_b17_soft_slack,
    load_plexos_reserve_softness,
    load_plexos_reserve_vors,
    run_audit,
)


# ---------------------------------------------------------------------------
# Helpers for the sol-based B14/B15 checks
# ---------------------------------------------------------------------------
def _write_storage_cache(
    cache_dir: Path,
    storages: dict[str, dict[int, list[float]]],
) -> None:
    """Write a minimal PLEXOS sol cache for the given Storage per-period series.

    ``storages`` maps ``name -> {property_id: [v_block1, ...]}``.  Produces the
    four CSV tables :func:`_storage_series_from_cache` reads: ``t_object`` (one
    Storage object per name), ``t_membership`` (collection 93), ``t_key`` (one
    key per (storage, property)), ``t_data_0`` (period-ordered values).
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    obj_lines = ["object_id,name,class_id"]
    mem_lines = [
        "membership_id,parent_class_id,child_class_id,collection_id,"
        "parent_object_id,child_object_id"
    ]
    key_lines = ["key_id,membership_id,property_id"]
    data_lines = ["key_id,period_id,value"]
    oid = 100
    mid = 200
    kid = 300
    for name, props in storages.items():
        oid += 1
        mid += 1
        obj_lines.append(f"{oid},{name},8")
        mem_lines.append(f"{mid},1,8,{STORAGE_COLLECTION_ID},1,{oid}")
        for pid, series in props.items():
            kid += 1
            key_lines.append(f"{kid},{mid},{pid}")
            for period, v in enumerate(series, start=1):
                data_lines.append(f"{kid},{period},{v}")
    (cache_dir / "t_object.csv").write_text("\n".join(obj_lines) + "\n")
    (cache_dir / "t_membership.csv").write_text("\n".join(mem_lines) + "\n")
    (cache_dir / "t_key.csv").write_text("\n".join(key_lines) + "\n")
    (cache_dir / "t_data_0.csv").write_text("\n".join(data_lines) + "\n")


def _write_fuel_price_csv(path: Path, name: str, per_period: list[float]) -> None:
    """Write a long-format Fuel_Price.csv (NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE)."""
    lines = ["NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE"]
    for h, v in enumerate(per_period, start=1):
        lines.append(f"{name},2026,2,15,{h},1,{v}")
    path.write_text("\n".join(lines) + "\n")


def _res_json(path: Path, reservoirs: list[dict]) -> None:
    path.write_text(
        json.dumps({"system": {"generator_array": [], "reservoir_array": reservoirs}})
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


# ---------------------------------------------------------------------------
# B14 (sol-based): reservoir emin/emax vs the PLEXOS SOLUTION Storage series
# ---------------------------------------------------------------------------
def test_b14_sol_flags_emax_profile_collapsed(tmp_path: Path) -> None:
    """PLEXOS Max Volume VARIES but gtopt emitted a scalar emax → collapse."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "CIPRESES", "emin": 54.6, "emax": 1478.7}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "CIPRESES": {
                STORAGE_PROP_MAX_VOLUME: [1478.7, 2021.5, 1800.0],
                STORAGE_PROP_MIN_VOLUME: [54.6, 54.6, 54.6],
                STORAGE_PROP_END_VOLUME: [1400.0, 1400.0, 1400.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    by_kind = {
        (it["name"], it["field"], it["kind"]): it
        for it in build_b14_reservoir_bounds_sol(gtopt_json, storages)
    }
    key = ("CIPRESES", "reservoir.emax", "profile_collapsed")
    assert key in by_kind
    assert by_kind[key]["plexos_range"] == [1478.7, 2021.5]
    assert by_kind[key]["source"] == "plexos_sol"


def test_b14_sol_matching_emax_profile_not_flagged(tmp_path: Path) -> None:
    """A reservoir emitting the full per-block emax matrix matching the PLEXOS
    Max Volume series is NOT flagged."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(
        gtopt_json,
        [{"name": "CIPRESES", "emin": 54.6, "emax": [[1478.7, 2021.5, 1800.0]]}],
    )
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "CIPRESES": {
                STORAGE_PROP_MAX_VOLUME: [1478.7, 2021.5, 1800.0],
                STORAGE_PROP_MIN_VOLUME: [54.6, 54.6, 54.6],
                STORAGE_PROP_END_VOLUME: [1400.0, 1400.0, 1400.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    assert not build_b14_reservoir_bounds_sol(gtopt_json, storages)


def test_b14_sol_emax_value_mismatch(tmp_path: Path) -> None:
    """A per-block emax matrix whose values diverge from the PLEXOS Max Volume
    series beyond tolerance is flagged value_mismatch with the worst block."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(
        gtopt_json,
        [{"name": "X", "emin": 0.0, "emax": [[1000.0, 1000.0, 1000.0]]}],
    )
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "X": {
                STORAGE_PROP_MAX_VOLUME: [1000.0, 2000.0, 1000.0],
                STORAGE_PROP_END_VOLUME: [500.0, 500.0, 500.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache, (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_END_VOLUME)
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    vm = [it for it in items if it["kind"] == "value_mismatch"]
    assert len(vm) == 1
    assert vm[0]["field"] == "reservoir.emax"
    assert vm[0]["worst_block"] == 1
    assert vm[0]["gtopt_at_worst"] == 1000.0
    assert vm[0]["plexos_at_worst"] == 2000.0


def test_b14_sol_emax_profile_spurious(tmp_path: Path) -> None:
    """gtopt emitted a VARYING emax matrix but PLEXOS Max Volume is constant."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(
        gtopt_json,
        [{"name": "X", "emin": 0.0, "emax": [[1000.0, 1500.0, 1200.0]]}],
    )
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "X": {
                STORAGE_PROP_MAX_VOLUME: [1000.0, 1000.0, 1000.0],
                STORAGE_PROP_END_VOLUME: [500.0, 500.0, 500.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache, (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_END_VOLUME)
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    sp = [it for it in items if it["kind"] == "profile_spurious"]
    assert len(sp) == 1
    assert sp[0]["field"] == "reservoir.emax"
    assert sp[0]["plexos_constant"] == 1000.0
    assert sp[0]["gtopt_range"] == [1000.0, 1500.0]


def test_b14_sol_bound_violated_in_sol(tmp_path: Path) -> None:
    """PLEXOS End Volume drops BELOW gtopt's hard emin → bound_violated_in_sol
    (gtopt's floor is inconsistent with PLEXOS's own trajectory — the real
    POLCURA finding: gtopt emin 3.998 vs PLEXOS End drawing to 3.093)."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "POLCURA", "emin": 3.998, "emax": 12.5}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "POLCURA": {
                STORAGE_PROP_MAX_VOLUME: [12.5, 12.5, 12.5],
                STORAGE_PROP_MIN_VOLUME: [3.093, 3.093, 3.093],
                STORAGE_PROP_END_VOLUME: [7.8, 3.093, 10.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    bv = [it for it in items if it["kind"] == "bound_violated_in_sol"]
    assert len(bv) == 1
    assert bv[0]["name"] == "POLCURA"
    assert bv[0]["below_block"] == 1
    assert round(bv[0]["below_emin_by"], 3) == round(3.998 - 3.093, 3)


def test_b14_sol_emin_efin_design_not_over_constrained(tmp_path: Path) -> None:
    """gtopt emin (physical floor) sitting BELOW a varying PLEXOS Min Volume
    (operational floor carried on efin) is NOT flagged a collapse, an
    over-constraint, or an End-Volume violation — the documented efin design.
    It IS reported as an informational ``emin_value_mismatch`` (gtopt below
    PLEXOS) but never HIGH."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "ELTORO", "emin": 100.0, "emax": 64651.0}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "ELTORO": {
                STORAGE_PROP_MAX_VOLUME: [64651.0, 64651.0, 64651.0],
                # varying operational floor, all ABOVE gtopt's physical emin=100
                STORAGE_PROP_MIN_VOLUME: [3031.0, 13889.0, 3031.0],
                STORAGE_PROP_END_VOLUME: [5000.0, 14000.0, 5000.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    kinds = {it["kind"] for it in items}
    assert "emin_over_constrained" not in kinds
    assert "bound_violated_in_sol" not in kinds
    assert "profile_collapsed" not in kinds
    # gtopt floor below PLEXOS min → informational mismatch only.
    info = [it for it in items if it["kind"] == "emin_value_mismatch"]
    assert len(info) == 1
    assert info[0]["priority"] == "INFO"
    assert info[0]["direction"] == "gtopt_emin_below_plexos_min"


def test_b14_sol_emin_over_constrained_polcura(tmp_path: Path) -> None:
    """POLCURA case: gtopt emits a CONSTANT emin floor (3.998) taken from the
    PLEXOS static Min Volume property, but the PLEXOS matrix per-period Min
    Volume is a lower 3.093 — gtopt's hard floor forbids levels PLEXOS uses.
    Must be flagged HIGH ``emin_over_constrained`` (gtopt above PLEXOS) even
    though the PLEXOS Min series is constant (no profile_collapsed)."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "POLCURA", "emin": 3.998, "emax": 12.5}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "POLCURA": {
                STORAGE_PROP_MAX_VOLUME: [12.5, 12.5, 12.5, 12.5],
                STORAGE_PROP_MIN_VOLUME: [3.093, 3.093, 3.093, 3.093],
                # End Volume stays inside gtopt's [emin, emax] here so the ONLY
                # signal is the floor value comparison, not bound_violated.
                STORAGE_PROP_END_VOLUME: [5.0, 6.0, 4.5, 5.5],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    oc = [it for it in items if it["kind"] == "emin_over_constrained"]
    assert len(oc) == 1
    assert oc[0]["name"] == "POLCURA"
    assert oc[0]["field"] == "reservoir.emin"
    assert oc[0]["priority"] == "HIGH"
    assert oc[0]["direction"] == "gtopt_emin_above_plexos_min"
    assert oc[0]["n_blocks_over"] == 4
    assert round(oc[0]["max_over_by"], 3) == round(3.998 - 3.093, 3)
    assert round(oc[0]["gtopt_at_worst"], 3) == 3.998
    assert round(oc[0]["plexos_at_worst"], 3) == 3.093
    # No End-Volume violation in this fixture (it stays inside the band).
    assert not [it for it in items if it["kind"] == "bound_violated_in_sol"]


def test_b14_sol_emin_over_constrained_ignores_zero_pad(tmp_path: Path) -> None:
    """A PLEXOS Min Volume series that is zero-padded for periods with no row
    must NOT trigger emin_over_constrained against gtopt's real floor: a 0.0
    means "no floor", not a floor of 0 (the ELTORO 130/132 zero-pad case)."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "ELTORO", "emin": 3031.0, "emax": 64651.0}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "ELTORO": {
                STORAGE_PROP_MAX_VOLUME: [64651.0, 64651.0, 64651.0, 64651.0],
                # only the first period carries a real Min; the rest are 0-pad,
                # and the real Min (13453) is ABOVE gtopt's floor → not over.
                STORAGE_PROP_MIN_VOLUME: [13453.0, 0.0, 0.0, 0.0],
                STORAGE_PROP_END_VOLUME: [13500.0, 13600.0, 13700.0, 13800.0],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    assert not [it for it in items if it["kind"] == "emin_over_constrained"]


def test_b14_sol_emin_value_match_not_flagged(tmp_path: Path) -> None:
    """gtopt emin EXACTLY equal to the PLEXOS per-period Min Volume → no emin
    value bucket at all (matrix value equality holds)."""
    gtopt_json = tmp_path / "DATOS.json"
    _res_json(gtopt_json, [{"name": "RALCO", "emin": 3.093, "emax": 12.5}])
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "RALCO": {
                STORAGE_PROP_MAX_VOLUME: [12.5, 12.5, 12.5],
                STORAGE_PROP_MIN_VOLUME: [3.093, 3.093, 3.093],
                STORAGE_PROP_END_VOLUME: [5.0, 6.0, 4.5],
            }
        },
    )
    from plexos2gtopt.uc_audit import _storage_series_from_cache

    storages = _storage_series_from_cache(
        cache,
        (STORAGE_PROP_MAX_VOLUME, STORAGE_PROP_MIN_VOLUME, STORAGE_PROP_END_VOLUME),
    )
    items = build_b14_reservoir_bounds_sol(gtopt_json, storages)
    assert not [
        it
        for it in items
        if it["kind"] in ("emin_over_constrained", "emin_value_mismatch")
    ]


# ---------------------------------------------------------------------------
# B15: Fuel.price per-block profile (input-CSV fallback path)
# ---------------------------------------------------------------------------
def _fuel_json(path: Path, fuels: list[dict]) -> None:
    path.write_text(
        json.dumps({"system": {"generator_array": [], "fuel_array": fuels}})
    )


def test_b15_fuel_price_collapsed(tmp_path: Path) -> None:
    """A varying Fuel_Price series emitted as a SCALAR is flagged collapsed."""
    gtopt_json = tmp_path / "DATOS.json"
    _fuel_json(gtopt_json, [{"name": "GNL", "price": 5.0}])
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_fuel_price_csv(input_dir / "Fuel_Price.csv", "GNL", [5.0, 7.0, 6.0])
    by_kind = {
        (it["name"], it["kind"]): it
        for it in build_b15_fuel_price(gtopt_json, input_dir)
    }
    key = ("GNL", "profile_collapsed")
    assert key in by_kind
    assert by_kind[key]["source"] == "input_csv"
    assert by_kind[key]["plexos_range"] == [5.0, 7.0]


def test_b15_fuel_price_match_not_flagged(tmp_path: Path) -> None:
    """A per-block fuel price matrix matching the input series is NOT flagged."""
    gtopt_json = tmp_path / "DATOS.json"
    _fuel_json(gtopt_json, [{"name": "GNL", "price": [[5.0, 7.0, 6.0]]}])
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_fuel_price_csv(input_dir / "Fuel_Price.csv", "GNL", [5.0, 7.0, 6.0])
    assert not build_b15_fuel_price(gtopt_json, input_dir)


def test_b15_fuel_price_spurious(tmp_path: Path) -> None:
    """gtopt emitted a varying price but the input series is constant."""
    gtopt_json = tmp_path / "DATOS.json"
    _fuel_json(gtopt_json, [{"name": "GNL", "price": [[5.0, 7.0, 6.0]]}])
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_fuel_price_csv(input_dir / "Fuel_Price.csv", "GNL", [5.0, 5.0, 5.0])
    items = build_b15_fuel_price(gtopt_json, input_dir)
    assert [it["kind"] for it in items] == ["profile_spurious"]
    assert items[0]["plexos_constant"] == 5.0


def test_b15_fuel_price_value_mismatch(tmp_path: Path) -> None:
    """A constant gtopt price that disagrees with the constant input price."""
    gtopt_json = tmp_path / "DATOS.json"
    _fuel_json(gtopt_json, [{"name": "GNL", "price": 5.0}])
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_fuel_price_csv(input_dir / "Fuel_Price.csv", "GNL", [9.0, 9.0, 9.0])
    items = build_b15_fuel_price(gtopt_json, input_dir)
    assert [it["kind"] for it in items] == ["value_mismatch"]
    assert items[0]["gtopt_distinct"] == [5.0]
    assert items[0]["plexos_distinct"] == [9.0]


def test_b15_fuel_price_sol_preferred(tmp_path: Path) -> None:
    """When a per-period sol Fuel Price is supplied it is PREFERRED over the
    input CSV and the source is marked plexos_sol."""
    gtopt_json = tmp_path / "DATOS.json"
    _fuel_json(gtopt_json, [{"name": "GNL", "price": 5.0}])
    items = build_b15_fuel_price(
        gtopt_json, input_dir=None, plexos_fuel_price={"GNL": [5.0, 7.0, 6.0]}
    )
    assert len(items) == 1
    assert items[0]["kind"] == "profile_collapsed"
    assert items[0]["source"] == "plexos_sol"


# ---------------------------------------------------------------------------
# B16: Turbine production_factor / capacity (input-vs-emitted)
# ---------------------------------------------------------------------------
def _turbine_json(path: Path, turbines: list[dict]) -> None:
    path.write_text(
        json.dumps({"system": {"generator_array": [], "turbine_array": turbines}})
    )


def _write_pf_csv(path: Path, gen: str, per_period: list[float]) -> None:
    lines = ["NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE"]
    for h, v in enumerate(per_period, start=1):
        lines.append(f"{gen},2026,2,15,{h},1,{v}")
    path.write_text("\n".join(lines) + "\n")


def test_b16_turbine_pf_collapsed(tmp_path: Path) -> None:
    """A varying Hydro_EfficiencyIncr emitted as a scalar production_factor is
    flagged collapsed, always input-vs-emitted (source=input_csv)."""
    gtopt_json = tmp_path / "DATOS.json"
    _turbine_json(
        gtopt_json,
        [{"name": "turbine_X", "generator": "GEN_X", "production_factor": 0.43}],
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_pf_csv(input_dir / "Hydro_EfficiencyIncr.csv", "GEN_X", [0.43, 0.55, 0.50])
    by_kind = {
        (it["name"], it["kind"]): it
        for it in build_b16_turbine_pf(gtopt_json, input_dir)
    }
    key = ("turbine_X", "profile_collapsed")
    assert key in by_kind
    assert by_kind[key]["source"] == "input_csv"
    assert by_kind[key]["field"] == "turbine.production_factor"
    assert by_kind[key]["input_range"] == [0.43, 0.55]


def test_b16_turbine_pf_match_not_flagged(tmp_path: Path) -> None:
    """A per-block production_factor matrix matching the input is NOT flagged."""
    gtopt_json = tmp_path / "DATOS.json"
    _turbine_json(
        gtopt_json,
        [
            {
                "name": "turbine_X",
                "generator": "GEN_X",
                "production_factor": [[0.43, 0.55, 0.50]],
            }
        ],
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_pf_csv(input_dir / "Hydro_EfficiencyIncr.csv", "GEN_X", [0.43, 0.55, 0.50])
    assert not build_b16_turbine_pf(gtopt_json, input_dir)


def test_b16_turbine_pf_spurious(tmp_path: Path) -> None:
    """gtopt emitted a varying PF but the input series is constant."""
    gtopt_json = tmp_path / "DATOS.json"
    _turbine_json(
        gtopt_json,
        [
            {
                "name": "turbine_X",
                "generator": "GEN_X",
                "production_factor": [[0.43, 0.55, 0.50]],
            }
        ],
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_pf_csv(input_dir / "Hydro_EfficiencyIncr.csv", "GEN_X", [0.43, 0.43, 0.43])
    items = build_b16_turbine_pf(gtopt_json, input_dir)
    assert [it["kind"] for it in items] == ["profile_spurious"]
    assert items[0]["input_constant"] == 0.43


def test_b16_turbine_pf_value_mismatch(tmp_path: Path) -> None:
    """A constant gtopt PF that disagrees with the constant input PF."""
    gtopt_json = tmp_path / "DATOS.json"
    _turbine_json(
        gtopt_json,
        [{"name": "turbine_X", "generator": "GEN_X", "production_factor": 0.43}],
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_pf_csv(input_dir / "Hydro_EfficiencyIncr.csv", "GEN_X", [0.99, 0.99, 0.99])
    items = build_b16_turbine_pf(gtopt_json, input_dir)
    assert [it["kind"] for it in items] == ["value_mismatch"]
    assert items[0]["gtopt_distinct"] == [0.43]
    assert items[0]["plexos_distinct"] == [0.99]


def test_b16_turbine_no_input_skips(tmp_path: Path) -> None:
    """No bundle CSVs → B16 skips gracefully (the usual CEN-bundle case)."""
    gtopt_json = tmp_path / "DATOS.json"
    _turbine_json(
        gtopt_json,
        [{"name": "turbine_X", "generator": "GEN_X", "production_factor": 0.43}],
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    assert not build_b16_turbine_pf(gtopt_json, input_dir)


# ---------------------------------------------------------------------------
# End-to-end wiring: B14-sol/B15/B16 surface through run_audit
# ---------------------------------------------------------------------------
def test_b14_sol_b15_wired_into_run_audit(tmp_path: Path) -> None:
    """run_audit populates the sol-based B14 + B15 buckets from the cache /
    input dir without needing the Hydro_*Volume input CSVs."""
    cache = tmp_path / "cache"
    _write_storage_cache(
        cache,
        {
            "CIPRESES": {
                STORAGE_PROP_MAX_VOLUME: [1478.7, 2021.5, 1800.0],
                STORAGE_PROP_MIN_VOLUME: [54.6, 54.6, 54.6],
                STORAGE_PROP_END_VOLUME: [1400.0, 1400.0, 1400.0],
            }
        },
    )
    gtopt_dir = tmp_path / "gtopt"
    gtopt_dir.mkdir()
    gtopt_json = gtopt_dir / "DATOS.json"
    gtopt_json.write_text(
        json.dumps(
            {
                "system": {
                    "generator_array": [],
                    "reservoir_array": [
                        {"name": "CIPRESES", "emin": 54.6, "emax": 1478.7}
                    ],
                    "fuel_array": [{"name": "GNL", "price": 5.0}],
                }
            }
        )
    )
    input_dir = tmp_path / "datos"
    input_dir.mkdir()
    _write_fuel_price_csv(input_dir / "Fuel_Price.csv", "GNL", [5.0, 7.0, 6.0])

    result = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=gtopt_dir,
            gtopt_json=gtopt_json,
            plexos_input_dir=input_dir,
        )
    )
    b14 = result.buckets.get("B14_reservoir_bounds_mismatch", [])
    assert any(
        it["field"] == "reservoir.emax" and it["kind"] == "profile_collapsed"
        for it in b14
    )
    b15 = result.buckets.get("B15_fuel_price_mismatch", [])
    assert any(it["name"] == "GNL" and it["kind"] == "profile_collapsed" for it in b15)
    d = result.to_dict()["buckets"]
    assert "B14_reservoir_bounds_mismatch" in d
    assert "B15_fuel_price_mismatch" in d


# ---------------------------------------------------------------------------
# B17: soft-slack coverage for the integer-coupled constraint families
# ---------------------------------------------------------------------------
def _write_reserve_cache(
    cache_dir: Path,
    reserves: dict[str, dict[int, list[float]]],
) -> None:
    """Write a minimal PLEXOS sol cache for the given Reserve per-period series.

    ``reserves`` maps ``name -> {property_id: [v1, ...]}``.  Produces the four
    CSV tables :func:`load_plexos_reserve_softness` reads: ``t_object`` (one
    Reserve object per name, class 14), ``t_membership`` (collection 156),
    ``t_key`` (one key per (reserve, property)), ``t_data_0`` (values).
    A reserve mapping the ``RESERVE_PROP_SHORTAGE`` property marks PLEXOS-soft.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    obj_lines = ["object_id,name,class_id"]
    mem_lines = [
        "membership_id,parent_class_id,child_class_id,collection_id,"
        "parent_object_id,child_object_id"
    ]
    key_lines = ["key_id,membership_id,property_id"]
    data_lines = ["key_id,period_id,value"]
    oid = 100
    mid = 200
    kid = 300
    for name, props in reserves.items():
        oid += 1
        mid += 1
        obj_lines.append(f"{oid},{name},{RESERVE_CLASS_ID}")
        mem_lines.append(
            f"{mid},1,{RESERVE_CLASS_ID},{SYS_RESERVE_COLLECTION_ID},1,{oid}"
        )
        for pid, series in props.items():
            kid += 1
            key_lines.append(f"{kid},{mid},{pid}")
            for period, v in enumerate(series, start=1):
                data_lines.append(f"{kid},{period},{v}")
    (cache_dir / "t_object.csv").write_text("\n".join(obj_lines) + "\n")
    (cache_dir / "t_membership.csv").write_text("\n".join(mem_lines) + "\n")
    (cache_dir / "t_key.csv").write_text("\n".join(key_lines) + "\n")
    (cache_dir / "t_data_0.csv").write_text("\n".join(data_lines) + "\n")


def _plan_json(path: Path, model_options: dict, system: dict) -> None:
    """Write a minimal planning JSON with options.model_options + a system."""
    path.write_text(
        json.dumps({"options": {"model_options": model_options}, "system": system})
    )


def test_load_plexos_reserve_softness_detects_shortage(tmp_path: Path) -> None:
    """The reserve loader reports Shortage present + the Price max."""
    cache = tmp_path / "cache"
    _write_reserve_cache(
        cache,
        {
            "CPF_RS": {
                RESERVE_PROP_SHORTAGE: [0.0, 0.0],
                RESERVE_PROP_PRICE: [120.0, 191.56],
            },
            "CTF_LW": {
                RESERVE_PROP_SHORTAGE: [0.0],
                RESERVE_PROP_PRICE: [88.0],
            },
        },
    )
    rs = load_plexos_reserve_softness(cache)
    assert rs["shortage_present"] is True
    assert rs["price_max"] == 191.56
    assert rs["n_reserves"] == 2
    assert rs["reserve_names"] == ["CPF_RS", "CTF_LW"]


def test_load_plexos_reserve_softness_no_shortage(tmp_path: Path) -> None:
    """A reserve cache WITHOUT the Shortage property → not soft."""
    cache = tmp_path / "cache"
    _write_reserve_cache(cache, {"CPF_RS": {RESERVE_PROP_PRICE: [120.0]}})
    rs = load_plexos_reserve_softness(cache)
    assert rs["shortage_present"] is False
    assert rs["price_max"] == 120.0


def test_load_plexos_reserve_softness_missing_cache(tmp_path: Path) -> None:
    """A missing cache degrades gracefully to not-soft / no price."""
    rs = load_plexos_reserve_softness(tmp_path / "does_not_exist")
    assert rs["shortage_present"] is False
    assert rs["price_max"] is None


def test_b17_hard_reserve_plus_plexos_shortage_flags_high(tmp_path: Path) -> None:
    """(a) gtopt reserve HARD (reserve_shortage_cost unset) + PLEXOS Shortage
    present → HIGH ``reserve_requirement_hard_but_plexos_soft`` with the PLEXOS
    Price max as the suggested lower bound (the exact missed class)."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19, "scale_objective": 1000.0},  # NO reserve cost
        {
            "reserve_zone_array": [
                {"name": "CPF_RS", "urreq": [10.0]},
                {"name": "CTF_LW", "drreq": [5.0]},
            ],
        },
    )
    reserve_softness = {
        "shortage_present": True,
        "price_max": 191.555,
        "shortage_max": 0.0,
        "n_reserves": 2,
        "reserve_names": ["CPF_RS", "CTF_LW"],
    }
    items = build_b17_soft_slack(gtopt_json, reserve_softness)
    high = [
        it for it in items if it["kind"] == "reserve_requirement_hard_but_plexos_soft"
    ]
    assert len(high) == 1
    flag = high[0]
    assert flag["priority"] == "HIGH"
    assert flag["family"] == "reserve_requirement"
    assert flag["field"] == "model_options.reserve_shortage_cost"
    assert flag["gtopt_value"] is None
    assert flag["plexos_shortage_present"] is True
    assert flag["suggested_reserve_shortage_cost"] == 191.555
    assert flag["demand_fail_cost"] == 467.19
    assert flag["n_reserve_zones"] == 2


def test_b17_soft_reserve_set_reports_no_flag(tmp_path: Path) -> None:
    """(b) gtopt reserve SOFT (reserve_shortage_cost=466.19) → NO HIGH flag,
    value reported and marked matching the soft-PLEXOS treatment."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19, "reserve_shortage_cost": 466.19},
        {"reserve_zone_array": [{"name": "CPF_RS", "urreq": [10.0]}]},
    )
    reserve_softness = {
        "shortage_present": True,
        "price_max": 191.555,
        "shortage_max": 0.0,
        "n_reserves": 1,
        "reserve_names": ["CPF_RS"],
    }
    items = build_b17_soft_slack(gtopt_json, reserve_softness)
    reserve_items = [it for it in items if it["family"] == "reserve_requirement"]
    assert len(reserve_items) == 1
    rep = reserve_items[0]
    assert rep["kind"] == "soft_slack_set"
    assert "priority" not in rep
    assert rep["value"] == 466.19
    assert rep["plexos_reserve_soft"] is True
    assert rep["plexos_reserve_price_max"] == 191.555
    # No HIGH flag anywhere.
    assert not [it for it in items if it.get("priority") == "HIGH"]


def test_b17_hard_reserve_no_plexos_evidence_not_high(tmp_path: Path) -> None:
    """Hard reserve in gtopt but PLEXOS has NO Shortage evidence → reported
    informational (hard_floor_no_plexos_evidence), never HIGH."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19},
        {"reserve_zone_array": [{"name": "CPF_RS", "urreq": [10.0]}]},
    )
    reserve_softness = {
        "shortage_present": False,
        "price_max": 120.0,
        "shortage_max": None,
        "n_reserves": 1,
        "reserve_names": ["CPF_RS"],
    }
    items = build_b17_soft_slack(gtopt_json, reserve_softness)
    reserve_items = [it for it in items if it["family"] == "reserve_requirement"]
    assert len(reserve_items) == 1
    assert reserve_items[0]["kind"] == "hard_floor_no_plexos_evidence"
    assert not [it for it in items if it.get("priority") == "HIGH"]


def test_b17_pmin_fcost_and_fmin_fcost_reporting(tmp_path: Path) -> None:
    """(c) the soft-fmin / pmin_fcost reporting: committed gens with vs without
    pmin_fcost and the value; forced waterways split soft vs hard."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"reserve_shortage_cost": 466.19},  # soft reserve → no HIGH flag here
        {
            "reserve_zone_array": [{"name": "CPF_RS", "urreq": [10.0]}],
            "generator_array": [
                {"name": "G_SOFT", "pmin_fcost": 105.43},
                {"name": "G_HARD", "pmin_fcost": None},
                {"name": "G_NOCOMMIT", "pmin_fcost": 105.43},
            ],
            "commitment_array": [
                {"name": "c1", "generator": "G_SOFT", "pmin": 50.0},
                {"name": "c2", "generator": "G_HARD", "pmin": 30.0},
                # G_NOCOMMIT has a commitment with pmin=0 → no floor → excluded
                {"name": "c3", "generator": "G_NOCOMMIT", "pmin": 0.0},
            ],
            "waterway_array": [
                {
                    "name": "Caudal_Eco_Ralco",
                    "fmin": [[20.0, 20.0]],
                    "fmin_fcost": 10.0,
                },
                {"name": "B_Maule", "fmin": [[15.0]], "fmin_fcost": 10.0},
                {"name": "Filt_Laja", "fmin": 21.97},  # hard, no fmin_fcost
                {"name": "Spillway", "fmin": 0.0},  # no forced flow → excluded
            ],
        },
    )
    items = build_b17_soft_slack(gtopt_json, reserve_softness=None)

    # pmin_fcost report: only G_SOFT and G_HARD carry a real commitment pmin.
    pmin = [it for it in items if it["family"] == "commitment_min_stable"]
    assert len(pmin) == 1
    p = pmin[0]
    assert p["kind"] == "pmin_fcost_report"
    assert p["field"] == "Generator.pmin_fcost"
    assert p["n_committed_gens"] == 2  # G_SOFT, G_HARD (G_NOCOMMIT pmin=0 excluded)
    assert p["n_with_pmin_fcost"] == 1
    assert p["n_without_pmin_fcost"] == 1
    assert p["pmin_fcost_values"] == [105.43]
    assert "priority" not in p  # informational, never HIGH

    # fmin_fcost report: two soft (Ralco, B_Maule), one hard (Filt_Laja).
    wfm = [it for it in items if it["family"] == "waterway_forced_flow"]
    assert len(wfm) == 1
    w = wfm[0]
    assert w["kind"] == "fmin_fcost_report"
    assert w["field"] == "Waterway.fmin_fcost"
    assert w["n_forced_flows"] == 3  # Spillway fmin=0 excluded
    assert w["n_soft"] == 2
    assert w["n_hard"] == 1
    by_name = {f["name"]: f for f in w["forced_flows"]}
    assert by_name["Caudal_Eco_Ralco"]["soft"] is True
    assert by_name["Caudal_Eco_Ralco"]["fmin_fcost"] == 10.0
    assert by_name["B_Maule"]["soft"] is True
    assert by_name["Filt_Laja"]["soft"] is False
    assert by_name["Filt_Laja"]["fmin_fcost"] is None


def test_b17_context_options_reported(tmp_path: Path) -> None:
    """hydro_spill_cost / strict_storage_emin context options are reported."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {
            "reserve_shortage_cost": 466.19,
            "hydro_spill_cost": 0.01,
            "strict_storage_emin": True,
        },
        {"reserve_zone_array": []},
    )
    items = build_b17_soft_slack(gtopt_json, reserve_softness=None)
    ctx = [it for it in items if it["family"] == "context"]
    assert len(ctx) == 1
    assert ctx[0]["hydro_spill_cost"] == 0.01
    assert ctx[0]["strict_storage_emin"] is True


def test_b17_wired_into_run_audit_high_flag(tmp_path: Path) -> None:
    """run_audit populates B17 + the n_b17_high summary count from the reserve
    cache; the HIGH reserve mismatch surfaces end-to-end (old hard-reserve)."""
    cache = tmp_path / "cache"
    _write_reserve_cache(
        cache,
        {
            "CPF_RS": {
                RESERVE_PROP_SHORTAGE: [0.0, 0.0],
                RESERVE_PROP_PRICE: [120.0, 191.555],
            },
        },
    )
    gtopt_dir = tmp_path / "gtopt"
    gtopt_dir.mkdir()
    gtopt_json = gtopt_dir / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19},  # NO reserve_shortage_cost → hard
        {"reserve_zone_array": [{"name": "CPF_RS", "urreq": [10.0]}]},
    )
    result = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=gtopt_dir,
            gtopt_json=gtopt_json,
        )
    )
    b17 = result.buckets.get("B17_soft_slack_coverage", [])
    assert any(
        it["kind"] == "reserve_requirement_hard_but_plexos_soft"
        and it["priority"] == "HIGH"
        for it in b17
    )
    assert result.summary["n_b17_high"] == 1
    assert "B17_soft_slack_coverage" in result.to_dict()["buckets"]


def test_b17_wired_into_run_audit_soft_no_flag(tmp_path: Path) -> None:
    """run_audit on the NEW soft-reserve case: reserve_shortage_cost set →
    no HIGH flag, n_b17_high == 0, value reported."""
    cache = tmp_path / "cache"
    _write_reserve_cache(
        cache,
        {"CPF_RS": {RESERVE_PROP_SHORTAGE: [0.0], RESERVE_PROP_PRICE: [191.555]}},
    )
    gtopt_dir = tmp_path / "gtopt"
    gtopt_dir.mkdir()
    gtopt_json = gtopt_dir / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19, "reserve_shortage_cost": 466.19},
        {"reserve_zone_array": [{"name": "CPF_RS", "urreq": [10.0]}]},
    )
    result = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=gtopt_dir,
            gtopt_json=gtopt_json,
        )
    )
    assert result.summary["n_b17_high"] == 0
    b17 = result.buckets.get("B17_soft_slack_coverage", [])
    rep = [it for it in b17 if it["family"] == "reserve_requirement"]
    assert rep and rep[0]["kind"] == "soft_slack_set"
    assert rep[0]["value"] == 466.19


# ---------------------------------------------------------------------------
# B17 reserve-VoRS INPUT read (load_plexos_reserve_vors + B17 quoting)
# ---------------------------------------------------------------------------
class _FakeReserveDb:
    """Minimal PlexosDb stand-in for probe_reserve_violation_cost."""

    def __init__(self, props: dict[tuple[str, int, str], float]) -> None:
        self._props = props

    def static_property(
        self, child_class: str, object_id: int, prop: str, **_kw: object
    ) -> float:
        return self._props.get((child_class, object_id, prop), 0.0)


def test_probe_reserve_violation_cost_vors_sentinel() -> None:
    """VoRS=-1 is returned verbatim with its source property name."""
    from plexos2gtopt.parsers import probe_reserve_violation_cost

    db = _FakeReserveDb({("Reserve", 7, "VoRS"): -1.0})
    raw, src = probe_reserve_violation_cost(db, 7)
    assert raw == -1.0
    assert src == "VoRS"


def test_probe_reserve_violation_cost_explicit_first_match() -> None:
    """A positive cost on an earlier-probed property wins over a later VoRS."""
    from plexos2gtopt.parsers import probe_reserve_violation_cost

    db = _FakeReserveDb(
        {("Reserve", 3, "Violation Cost"): 250.0, ("Reserve", 3, "VoRS"): -1.0}
    )
    raw, src = probe_reserve_violation_cost(db, 3)
    assert raw == 250.0
    assert src == "Violation Cost"


def test_probe_reserve_violation_cost_none_defined() -> None:
    """No VoRS-family property defined → (0.0, None) (gtopt treats it hard)."""
    from plexos2gtopt.parsers import probe_reserve_violation_cost

    raw, src = probe_reserve_violation_cost(_FakeReserveDb({}), 1)
    assert raw == 0.0
    assert src is None


def test_load_plexos_reserve_vors_missing_xml(tmp_path: Path) -> None:
    """Missing / None XML degrades to an empty mapping (B17 still runs)."""
    assert not load_plexos_reserve_vors(None)
    assert not load_plexos_reserve_vors(tmp_path / "nope.xml")


def test_b17_quotes_vors_in_soft_reserve_item(tmp_path: Path) -> None:
    """When the VoRS input is supplied, the soft reserve item QUOTES the actual
    penalty input (the -1 default sentinel) alongside the gtopt cost."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19, "reserve_shortage_cost": 466.19},
        {"reserve_zone_array": [{"name": "CSF_RS", "urreq": [10.0]}]},
    )
    reserve_softness = {
        "shortage_present": True,
        "price_max": 191.555,
        "shortage_max": 0.0,
        "n_reserves": 1,
        "reserve_names": ["CSF_RS"],
    }
    reserve_vors = {
        "CSF_RS": {
            "vors": -1.0,
            "source_prop": "VoRS",
            "interpreted": "default_sentinel",
        },
    }
    items = build_b17_soft_slack(gtopt_json, reserve_softness, reserve_vors)
    rep = [it for it in items if it["family"] == "reserve_requirement"][0]
    assert rep["kind"] == "soft_slack_set"
    assert rep["plexos_vors_distinct"] == [-1.0]
    assert rep["plexos_vors_source"] == ["VoRS"]
    assert rep["plexos_vors_interpreted"] == ["default_sentinel"]


def test_b17_vors_sentinel_marks_plexos_soft_without_shortage_output(
    tmp_path: Path,
) -> None:
    """VoRS=-1 (default sentinel) marks PLEXOS soft even when the Shortage
    OUTPUT is absent from the cache → a HARD gtopt reserve still flags HIGH."""
    gtopt_json = tmp_path / "DATOS.json"
    _plan_json(
        gtopt_json,
        {"demand_fail_cost": 467.19},  # NO reserve_shortage_cost → hard gtopt
        {"reserve_zone_array": [{"name": "CSF_RS", "urreq": [10.0]}]},
    )
    reserve_softness = {
        "shortage_present": False,  # Shortage output absent from the cache
        "price_max": None,
        "shortage_max": None,
        "n_reserves": 1,
        "reserve_names": ["CSF_RS"],
    }
    reserve_vors = {
        "CSF_RS": {
            "vors": -1.0,
            "source_prop": "VoRS",
            "interpreted": "default_sentinel",
        },
    }
    items = build_b17_soft_slack(gtopt_json, reserve_softness, reserve_vors)
    high = [it for it in items if it.get("priority") == "HIGH"]
    assert len(high) == 1
    assert high[0]["kind"] == "reserve_requirement_hard_but_plexos_soft"
    assert high[0]["plexos_vors_interpreted"] == ["default_sentinel"]
    # plexos_shortage_present reflects the actual (absent) OUTPUT, not the VoRS.
    assert high[0]["plexos_shortage_present"] is False
