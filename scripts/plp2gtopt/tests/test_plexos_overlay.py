# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``plp2gtopt._plexos_overlay``.

Coverage targets:
* name normalization (whitespace / dash-vs-underscore / case)
* path resolution (.json file, directory with one .json, directory with
  preferred ``<dir>/<dir.name>.json``, error on ambiguity)
* generator overlay — scalar heat_rate
* generator overlay — piecewise heat_rate_segments + pmax_segments
* generator overlay — lossfactor + gcost (scalar)
* generator overlay — non-scalar PLEXOS field skipped + reported
* generator overlay — forbidden / commitment field stripped
  (no-integer invariant)
* fuel synthesis from PLEXOS source
* fuel reuse (existing PLP Fuel updated, not duplicated)
* overlay report shape (matched / unmatched-plp-only / unmatched-plexos-only /
  fuels_added / fuels_reused / skipped_fields / source_path)
* report file persisted to disk
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from plp2gtopt._plexos_overlay import (
    OverlayReport,
    PlexosOverlay,
    _normalize,
    _resolve_overlay_path,
    apply_plexos_overlay,
    load_plexos_overlay,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _plp_planning(
    generators: list[dict[str, Any]], fuels: list[dict[str, Any]] | None = None
) -> dict[str, Any]:
    return {
        "options": {},
        "simulation": {},
        "system": {
            "name": "plp_test",
            "generator_array": generators,
            "fuel_array": fuels if fuels is not None else [],
        },
    }


def _plexos_source(
    generators: list[dict[str, Any]], fuels: list[dict[str, Any]]
) -> dict[str, Any]:
    return {
        "options": {},
        "simulation": {},
        "system": {
            "name": "plexos_test",
            "generator_array": generators,
            "fuel_array": fuels,
        },
    }


def _write_json(path: Path, data: dict[str, Any]) -> Path:
    path.write_text(json.dumps(data))
    return path


# ---------------------------------------------------------------------------
# _normalize
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ("Ralco U1", "RALCOU1"),
        ("ralco-u1", "RALCOU1"),
        ("RALCO_U1", "RALCOU1"),
        ("  Coal_PLANT ", "COALPLANT"),
    ],
)
def test_normalize(raw: str, expected: str) -> None:
    assert _normalize(raw) == expected


def test_normalize_handles_mixed_spaces_and_dashes() -> None:
    # PLEXOS-style "Coal-Plant 1" vs PLP-style "coal_plant_1" both
    # collapse to a single canonical key.
    assert _normalize("Coal-Plant 1") == _normalize("coal_plant_1")


# ---------------------------------------------------------------------------
# _resolve_overlay_path
# ---------------------------------------------------------------------------


def test_resolve_overlay_path_file(tmp_path: Path) -> None:
    p = _write_json(tmp_path / "foo.json", {"system": {}})
    assert _resolve_overlay_path(p) == p


def test_resolve_overlay_path_directory_preferred(tmp_path: Path) -> None:
    case = tmp_path / "gtopt_week_1"
    case.mkdir()
    preferred = _write_json(case / "gtopt_week_1.json", {"system": {}})
    _write_json(case / "side.json", {"system": {}})
    # Even with multiple JSONs the dir.name match wins.
    assert _resolve_overlay_path(case) == preferred


def test_resolve_overlay_path_directory_single_json(tmp_path: Path) -> None:
    case = tmp_path / "case_x"
    case.mkdir()
    only = _write_json(case / "different_name.json", {"system": {}})
    assert _resolve_overlay_path(case) == only


def test_resolve_overlay_path_directory_ambiguous(tmp_path: Path) -> None:
    case = tmp_path / "ambiguous"
    case.mkdir()
    _write_json(case / "a.json", {})
    _write_json(case / "b.json", {})
    with pytest.raises(FileNotFoundError, match="multiple .json files"):
        _resolve_overlay_path(case)


def test_resolve_overlay_path_missing(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="no such file"):
        _resolve_overlay_path(tmp_path / "does_not_exist")


def test_resolve_overlay_path_directory_no_json(tmp_path: Path) -> None:
    case = tmp_path / "empty_case"
    case.mkdir()
    with pytest.raises(FileNotFoundError, match="no .json file"):
        _resolve_overlay_path(case)


# ---------------------------------------------------------------------------
# Resolve "latest" sentinel against the plexos2gtopt run registry
# ---------------------------------------------------------------------------


def _seed_registry(tmp_path: Path, monkeypatch, rows: list[dict]) -> Path:
    """Create a temporary registry + monkeypatch the module constant to it.

    Returns the registry path so tests can append more rows / mutate it.
    """
    import plp2gtopt._plexos_overlay as overlay_mod  # noqa: PLC0415

    reg_path = tmp_path / "registry" / "runs.jsonl"
    reg_path.parent.mkdir(parents=True, exist_ok=True)
    with open(reg_path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")
    monkeypatch.setattr(overlay_mod, "_PLEXOS_RUN_REGISTRY", reg_path)
    return reg_path


def test_resolve_latest_returns_last_registry_entry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The ``latest`` sentinel must resolve to the LAST line of the registry,
    not the first — the registry is append-only / chronological.
    """
    earlier_dir = tmp_path / "earlier"
    earlier_dir.mkdir()
    earlier_json = earlier_dir / "earlier.json"
    earlier_json.write_text("{}")
    later_dir = tmp_path / "later"
    later_dir.mkdir()
    later_json = later_dir / "later.json"
    later_json.write_text("{}")
    _seed_registry(
        tmp_path,
        monkeypatch,
        rows=[
            {
                "timestamp": "2026-05-01T00:00:00Z",
                "output_dir": str(earlier_dir),
                "output_file": str(earlier_json),
                "input_path": "/dev/null",
                "bundle_stem": "earlier",
            },
            {
                "timestamp": "2026-06-01T00:00:00Z",
                "output_dir": str(later_dir),
                "output_file": str(later_json),
                "input_path": "/dev/null",
                "bundle_stem": "later",
            },
        ],
    )
    assert _resolve_overlay_path(Path("latest")) == later_json


def test_resolve_latest_missing_registry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When the registry file does not exist, ``latest`` raises with a
    self-explanatory message pointing at the missing path.
    """
    import plp2gtopt._plexos_overlay as overlay_mod  # noqa: PLC0415

    monkeypatch.setattr(
        overlay_mod, "_PLEXOS_RUN_REGISTRY", tmp_path / "absent" / "runs.jsonl"
    )
    with pytest.raises(FileNotFoundError, match="no plexos2gtopt run registry"):
        _resolve_overlay_path(Path("latest"))


def test_resolve_latest_empty_registry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An empty (just-created) registry file is treated like missing — the
    resolver tells the user to run plexos2gtopt first.
    """
    _seed_registry(tmp_path, monkeypatch, rows=[])
    with pytest.raises(FileNotFoundError, match="no valid entries"):
        _resolve_overlay_path(Path("latest"))


def test_resolve_latest_target_no_longer_exists(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When EVERY registry entry points at a deleted path, the resolver
    raises with a path-pointing message instead of returning a stale Path.
    """
    _seed_registry(
        tmp_path,
        monkeypatch,
        rows=[
            {
                "timestamp": "2026-06-01T00:00:00Z",
                "output_dir": str(tmp_path / "gone"),
                "output_file": str(tmp_path / "gone" / "gone.json"),
                "input_path": "/dev/null",
                "bundle_stem": "gone",
            },
        ],
    )
    with pytest.raises(FileNotFoundError, match="no longer exists"):
        _resolve_overlay_path(Path("latest"))


def test_resolve_latest_walks_back_past_stale_entries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The resolver walks newest→oldest and returns the first entry
    whose ``output_file`` still exists on disk.  Stale entries
    (pytest scratch dirs, manually-deleted outputs) are silently
    skipped — the user wanted "latest", and the truly latest VALID
    run is the right interpretation.
    """
    # One real dir + JSON that survives, then a "newer" stale entry
    # appended on top.  The stale entry must be skipped, the older
    # real entry returned.
    real_dir = tmp_path / "real"
    real_dir.mkdir()
    real_json = real_dir / "real.json"
    real_json.write_text("{}")
    _seed_registry(
        tmp_path,
        monkeypatch,
        rows=[
            {
                "timestamp": "2026-06-01T00:00:00Z",
                "output_dir": str(real_dir),
                "output_file": str(real_json),
                "input_path": "/dev/null",
                "bundle_stem": "real",
            },
            {
                "timestamp": "2026-06-02T00:00:00Z",
                "output_dir": str(tmp_path / "stale"),
                "output_file": str(tmp_path / "stale" / "stale.json"),
                "input_path": "/dev/null",
                "bundle_stem": "stale",
            },
            {
                "timestamp": "2026-06-03T00:00:00Z",
                "output_dir": str(tmp_path / "also_stale"),
                "output_file": str(tmp_path / "also_stale" / "also.json"),
                "input_path": "/dev/null",
                "bundle_stem": "also_stale",
            },
        ],
    )
    # Walks back past the two stale tail entries, returns the real one.
    assert _resolve_overlay_path(Path("latest")) == real_json


def test_resolve_latest_skips_malformed_lines(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A partially-written tail row (process killed mid-append) must NOT
    poison the resolver — the last VALID row should still win.
    """
    import plp2gtopt._plexos_overlay as overlay_mod  # noqa: PLC0415

    valid_dir = tmp_path / "valid"
    valid_dir.mkdir()
    valid_json = valid_dir / "valid.json"
    valid_json.write_text("{}")
    reg_path = tmp_path / "reg.jsonl"
    with open(reg_path, "w", encoding="utf-8") as f:
        f.write(
            json.dumps(
                {
                    "timestamp": "2026-06-01T00:00:00Z",
                    "output_dir": str(valid_dir),
                    "output_file": str(valid_json),
                    "input_path": "/dev/null",
                    "bundle_stem": "valid",
                }
            )
            + "\n"
        )
        # Tail row written partially — broken JSON
        f.write('{"timestamp": "2026-06-02T00:00:00')
    monkeypatch.setattr(overlay_mod, "_PLEXOS_RUN_REGISTRY", reg_path)
    assert _resolve_overlay_path(Path("latest")) == valid_json


# ---------------------------------------------------------------------------
# Generator overlay — scalar heat_rate path
# ---------------------------------------------------------------------------


def test_apply_scalar_heat_rate_and_fuel() -> None:
    planning = _plp_planning(
        generators=[{"uid": 1, "name": "Coal1", "bus": 1, "gcost": 50.0}],
    )
    src = _plexos_source(
        generators=[
            {
                "uid": 1,
                "name": "coal-1",  # name match after normalization
                "bus": "bus1",
                "fuel": "coal",
                "heat_rate": 9.5,
                "gcost": 2.0,
            }
        ],
        fuels=[{"uid": 1, "name": "coal", "price": 4.0, "heat_content": 25.0}],
    )
    overlay = PlexosOverlay(src, Path("/tmp/plexos.json"))
    report = overlay.apply(planning)

    gen = planning["system"]["generator_array"][0]
    assert gen["heat_rate"] == 9.5
    assert gen["fuel"] == "coal"
    assert gen["gcost"] == 2.0  # PLEXOS scalar wins over PLP scalar
    # Fuel was synthesized
    fuels = planning["system"]["fuel_array"]
    assert len(fuels) == 1
    assert fuels[0]["name"] == "coal"
    assert fuels[0]["price"] == 4.0
    assert fuels[0]["heat_content"] == 25.0
    # Report shape
    assert report.matched == ("Coal1",)
    assert not report.unmatched_plp_only
    assert not report.unmatched_plexos_only
    assert report.fuels_added == ("coal",)
    assert not report.fuels_reused


# ---------------------------------------------------------------------------
# Generator overlay — piecewise segments path
# ---------------------------------------------------------------------------


def test_apply_piecewise_segments_replaces_scalar() -> None:
    planning = _plp_planning(
        generators=[
            {
                "uid": 1,
                "name": "Diesel1",
                "bus": 1,
                "gcost": 80.0,
                "heat_rate": 11.0,  # stale scalar, should be dropped
            }
        ],
    )
    src = _plexos_source(
        generators=[
            {
                "uid": 1,
                "name": "Diesel1",
                "bus": "x",
                "fuel": "diesel",
                "pmax_segments": [100.0, 200.0],
                "heat_rate_segments": [9.0, 10.5],
                "gcost": 1.5,
            }
        ],
        fuels=[{"uid": 1, "name": "diesel", "price": 80.0}],
    )
    PlexosOverlay(src, Path("p.json")).apply(planning)

    gen = planning["system"]["generator_array"][0]
    # Piecewise present, scalar heat_rate stripped
    assert gen["heat_rate_segments"] == [9.0, 10.5]
    assert gen["pmax_segments"] == [100.0, 200.0]
    assert "heat_rate" not in gen
    assert gen["fuel"] == "diesel"
    assert gen["gcost"] == 1.5


# ---------------------------------------------------------------------------
# Generator overlay — non-scalar PLEXOS field skipped + reported
# ---------------------------------------------------------------------------


def test_non_scalar_heat_rate_skipped_and_reported() -> None:
    planning = _plp_planning(
        generators=[{"uid": 1, "name": "TimeVar", "gcost": 10.0}],
    )
    # PLEXOS source ships a Parquet-reference shape (string) instead of
    # a scalar; first pass does not copy field files → skip + report.
    src = _plexos_source(
        generators=[{"uid": 1, "name": "TimeVar", "heat_rate": "heat_rate"}],
        fuels=[],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    gen = planning["system"]["generator_array"][0]
    assert "heat_rate" not in gen  # not overwritten with a string
    assert "TimeVar" in report.skipped_fields
    assert any("heat_rate" in entry for entry in report.skipped_fields["TimeVar"])


# ---------------------------------------------------------------------------
# Generator overlay — lossfactor + gcost preservation when plp profile
# ---------------------------------------------------------------------------


def test_lossfactor_copied_and_plp_profile_gcost_preserved() -> None:
    profile_gcost: list[list[float]] = [[50.0, 60.0]]
    planning = _plp_planning(
        generators=[{"uid": 1, "name": "Gen1", "gcost": profile_gcost}],
    )
    src = _plexos_source(
        generators=[
            {
                "uid": 1,
                "name": "Gen1",
                "lossfactor": 0.05,
                "gcost": 3.0,  # scalar in PLEXOS, but plp ships a profile
            }
        ],
        fuels=[],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    gen = planning["system"]["generator_array"][0]
    assert gen["lossfactor"] == 0.05
    # PLP per-stage profile is preserved over the PLEXOS scalar (the
    # overlay does NOT clobber per-stage shapes).
    assert gen["gcost"] == profile_gcost
    assert "Gen1" in report.skipped_fields
    assert any("gcost" in entry for entry in report.skipped_fields["Gen1"])


# ---------------------------------------------------------------------------
# Generator overlay — forbidden / commitment field stripped
# ---------------------------------------------------------------------------


def test_forbidden_field_stripped_from_plp_gen() -> None:
    """A commitment / UC field on a plp-side generator is removed by the
    overlay pass even when it sneaks in from upstream — preserves the
    no-integer invariant enforced by ``test_no_integer_variables``.
    """
    planning = _plp_planning(
        generators=[
            {
                "uid": 1,
                "name": "Gen1",
                "gcost": 5.0,
                # commitment-typed leak from upstream
                "commitment": "uc-Gen1",
                "startup_cost": 1000.0,
                "min_uptime": 8,
            }
        ],
    )
    src = _plexos_source(
        generators=[{"uid": 1, "name": "Gen1", "heat_rate": 10.0}],
        fuels=[],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    gen = planning["system"]["generator_array"][0]
    for forbidden in ("commitment", "startup_cost", "min_uptime"):
        assert forbidden not in gen, f"{forbidden} should be stripped"
    assert "Gen1" in report.skipped_fields


# ---------------------------------------------------------------------------
# Fuel overlay — reuse existing entry vs synthesize
# ---------------------------------------------------------------------------


def test_existing_fuel_is_updated_not_duplicated() -> None:
    planning = _plp_planning(
        generators=[{"uid": 1, "name": "Coal1", "gcost": 0.0}],
        fuels=[{"uid": 1, "name": "coal", "price": 999.0}],
    )
    src = _plexos_source(
        generators=[{"uid": 1, "name": "Coal1", "fuel": "coal", "heat_rate": 9.5}],
        fuels=[
            {
                "uid": 1,
                "name": "coal",
                "price": 4.0,
                "max_offtake": 5000.0,
                "max_offtake_cost": 1000.0,
                "emission_factors": [{"emission": "co2", "combustion": 0.094}],
            }
        ],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    fuels = planning["system"]["fuel_array"]
    assert len(fuels) == 1  # no duplicate
    coal = fuels[0]
    assert coal["uid"] == 1  # original uid preserved
    assert coal["price"] == 4.0  # PLEXOS wins
    assert coal["max_offtake"] == 5000.0
    assert coal["max_offtake_cost"] == 1000.0
    assert coal["emission_factors"][0]["emission"] == "co2"
    assert not report.fuels_added
    assert report.fuels_reused == ("coal",)


def test_fuel_uid_uniqueness_when_synthesizing() -> None:
    planning = _plp_planning(
        generators=[
            {"uid": 1, "name": "G1", "gcost": 0.0},
            {"uid": 2, "name": "G2", "gcost": 0.0},
        ],
        fuels=[{"uid": 7, "name": "biomass", "price": 0.0}],
    )
    src = _plexos_source(
        generators=[
            {"uid": 1, "name": "G1", "fuel": "coal", "heat_rate": 9.0},
            {"uid": 2, "name": "G2", "fuel": "diesel", "heat_rate": 8.0},
        ],
        fuels=[
            {"uid": 1, "name": "coal", "price": 4.0},
            {"uid": 2, "name": "diesel", "price": 80.0},
        ],
    )
    PlexosOverlay(src, Path("p.json")).apply(planning)

    fuels = planning["system"]["fuel_array"]
    uids = [f["uid"] for f in fuels]
    assert len(uids) == len(set(uids))  # no collisions
    assert max(uids) >= 9  # new uids start above existing 7


# ---------------------------------------------------------------------------
# Unmatched accounting
# ---------------------------------------------------------------------------


def test_unmatched_accounting() -> None:
    planning = _plp_planning(
        generators=[
            {"uid": 1, "name": "Shared", "gcost": 0.0},
            {"uid": 2, "name": "OnlyInPlp", "gcost": 0.0},
        ],
    )
    src = _plexos_source(
        generators=[
            {"uid": 1, "name": "Shared", "heat_rate": 10.0},
            {"uid": 2, "name": "OnlyInPlexos", "heat_rate": 11.0},
        ],
        fuels=[],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    assert report.matched == ("Shared",)
    assert report.unmatched_plp_only == ("OnlyInPlp",)
    assert report.unmatched_plexos_only == ("OnlyInPlexos",)


def test_generator_with_fuel_ref_unknown_in_source_is_not_synthesized() -> None:
    """If a PLEXOS Generator's fuel FK points at a fuel name absent from
    the PLEXOS fuel_array, the overlay must not invent a placeholder.
    """
    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    src = _plexos_source(
        generators=[{"uid": 1, "name": "G", "fuel": "mystery", "heat_rate": 9.0}],
        fuels=[],  # mystery is not declared
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)
    assert not planning["system"]["fuel_array"]
    assert not report.fuels_added
    # Generator did receive heat_rate + fuel FK; the FK simply doesn't
    # resolve in this case — gtopt will warn at LP build, as documented
    # in the module docstring.
    assert planning["system"]["generator_array"][0]["fuel"] == "mystery"


# ---------------------------------------------------------------------------
# End-to-end via apply_plexos_overlay (report on disk)
# ---------------------------------------------------------------------------


def test_apply_plexos_overlay_writes_report(tmp_path: Path) -> None:
    plexos_dir = tmp_path / "gtopt_plx"
    plexos_dir.mkdir()
    _write_json(
        plexos_dir / "gtopt_plx.json",
        _plexos_source(
            generators=[{"uid": 1, "name": "G", "fuel": "coal", "heat_rate": 10.0}],
            fuels=[{"uid": 1, "name": "coal", "price": 4.0}],
        ),
    )

    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    report_path = tmp_path / "out" / "report.json"
    report = apply_plexos_overlay(planning, plexos_dir, report_path=report_path)

    assert isinstance(report, OverlayReport)
    assert report.matched == ("G",)
    assert report_path.exists()
    loaded = json.loads(report_path.read_text())
    assert loaded["summary"]["matched"] == 1
    assert loaded["fuels_added"] == ["coal"]
    assert "system" not in loaded  # report is a digest, not the case


def test_load_plexos_overlay_rejects_non_dict_root(tmp_path: Path) -> None:
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(["not", "a", "dict"]))
    with pytest.raises(ValueError, match="expected a JSON object"):
        load_plexos_overlay(bad)


# ---------------------------------------------------------------------------
# Idempotence — applying the overlay twice yields the same result
# ---------------------------------------------------------------------------


def test_overlay_is_idempotent() -> None:
    src = _plexos_source(
        generators=[
            {
                "uid": 1,
                "name": "G",
                "fuel": "coal",
                "pmax_segments": [50.0, 100.0],
                "heat_rate_segments": [9.0, 11.0],
                "gcost": 2.0,
            }
        ],
        fuels=[{"uid": 1, "name": "coal", "price": 4.0}],
    )
    overlay = PlexosOverlay(src, Path("p.json"))

    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    overlay.apply(planning)
    gen_first = json.dumps(planning["system"]["generator_array"][0], sort_keys=True)
    fuels_first = json.dumps(planning["system"]["fuel_array"], sort_keys=True)
    overlay.apply(planning)  # second pass
    gen_second = json.dumps(planning["system"]["generator_array"][0], sort_keys=True)
    fuels_second = json.dumps(planning["system"]["fuel_array"], sort_keys=True)
    assert gen_first == gen_second
    assert fuels_first == fuels_second


# ---------------------------------------------------------------------------
# emission_array + emission_zone_array carry-over (so plp doesn't need to
# re-run --emissions when the source plexos2gtopt JSON already had it)
# ---------------------------------------------------------------------------


def _src_with_full_emissions() -> dict:
    """Source planning that emulates a plexos2gtopt --emissions run:
    Fuel with per-pollutant emission_factors + matching emission_array
    pollutant rows + an EmissionZone covering them."""
    return _plexos_source(
        generators=[{"uid": 1, "name": "G", "fuel": "coal", "heat_rate": 9.5}],
        fuels=[
            {
                "uid": 1,
                "name": "coal",
                "price": 4.0,
                "heat_content": 25.8,
                "emission_factors": [
                    {"emission": "co2", "combustion": 0.0961},
                    {"emission": "ch4", "combustion": 1e-06},
                    {"emission": "n2o", "combustion": 1.5e-06},
                ],
            }
        ],
    ) | {
        "system": _plexos_source(
            generators=[{"uid": 1, "name": "G", "fuel": "coal", "heat_rate": 9.5}],
            fuels=[
                {
                    "uid": 1,
                    "name": "coal",
                    "price": 4.0,
                    "heat_content": 25.8,
                    "emission_factors": [
                        {"emission": "co2", "combustion": 0.0961},
                        {"emission": "ch4", "combustion": 1e-06},
                        {"emission": "n2o", "combustion": 1.5e-06},
                    ],
                }
            ],
        )["system"]
        | {
            "emission_array": [
                {"uid": 1, "name": "co2"},
                {"uid": 2, "name": "ch4"},
                {"uid": 3, "name": "n2o"},
            ],
            "emission_zone_array": [
                {
                    "uid": 1,
                    "name": "global_ghg",
                    "emissions": [
                        {"emission": "co2", "weight": 1.0},
                        {"emission": "ch4", "weight": 28.0},
                        {"emission": "n2o", "weight": 265.0},
                    ],
                },
            ],
        }
    }


def test_overlay_carries_emission_array_from_source() -> None:
    """When the plexos2gtopt source already has emission_array (because
    plexos2gtopt was run with --emissions), the overlay must carry
    those pollutant rows over.  Otherwise gtopt's
    expand_fuel_emission_sources drops the Fuel.emission_factors with
    'unresolved emission name'.
    """
    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    src = _src_with_full_emissions()
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    # All three pollutants from the source landed in plp's emission_array.
    pollutants = {p["name"] for p in planning["system"].get("emission_array", [])}
    assert pollutants == {"co2", "ch4", "n2o"}
    assert set(report.emissions_carried) == {"co2", "ch4", "n2o"}


def test_overlay_carries_emission_zone_from_source() -> None:
    """The EmissionZone (with cap / price / pollutant basket) must
    also carry across — otherwise gtopt's
    ``expand_fuel_emission_sources`` short-circuits on an empty
    ``emission_zone_array`` and zero EmissionSource rows land.
    """
    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    src = _src_with_full_emissions()
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    zones = planning["system"].get("emission_zone_array", [])
    assert len(zones) == 1
    assert zones[0]["name"] == "global_ghg"
    # GWP-100 weights preserved
    weights = {ef["emission"]: ef.get("weight") for ef in zones[0]["emissions"]}
    assert weights == {"co2": 1.0, "ch4": 28.0, "n2o": 265.0}
    assert report.emission_zones_carried == ("global_ghg",)


def test_overlay_emission_carry_dedupes_by_name() -> None:
    """An existing plp-side emission/zone row with the same name MUST
    survive the overlay (plp data wins on name collision).
    """
    planning = _plp_planning(
        generators=[{"uid": 1, "name": "G", "gcost": 0.0}],
    )
    planning["system"]["emission_array"] = [
        {"uid": 7, "name": "co2"},  # plp-side, uid=7 stays
    ]
    planning["system"]["emission_zone_array"] = [
        {
            "uid": 5,
            "name": "my_zone",
            "emissions": [{"emission": "co2", "weight": 1.0}],
            "price": 25.0,  # user-supplied carbon price stays
        }
    ]
    src = _src_with_full_emissions()
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    # co2 preserved at uid=7, ch4/n2o added with fresh uids
    by_name = {p["name"]: p for p in planning["system"]["emission_array"]}
    assert by_name["co2"]["uid"] == 7  # plp-side wins
    assert "ch4" in by_name
    assert "n2o" in by_name
    # plp-side zone preserved with price intact; global_ghg added alongside
    zones = planning["system"]["emission_zone_array"]
    by_zone = {z["name"]: z for z in zones}
    assert by_zone["my_zone"]["price"] == 25.0  # plp-side wins
    assert "global_ghg" in by_zone
    # Report shows only freshly added names
    assert set(report.emissions_carried) == {"ch4", "n2o"}  # co2 already present
    assert report.emission_zones_carried == ("global_ghg",)


def test_overlay_no_emission_arrays_in_source_no_op() -> None:
    """When the source has no emission infrastructure (e.g., plexos2gtopt
    was run WITHOUT --emissions), the overlay still works and the
    emission carry-over is a clean no-op (no empty arrays sprouted).
    """
    planning = _plp_planning(generators=[{"uid": 1, "name": "G", "gcost": 0.0}])
    # _plexos_source helper doesn't add emission arrays
    src = _plexos_source(
        generators=[{"uid": 1, "name": "G", "fuel": "coal", "heat_rate": 9.5}],
        fuels=[{"uid": 1, "name": "coal", "price": 4.0}],
    )
    report = PlexosOverlay(src, Path("p.json")).apply(planning)

    # No emission infrastructure auto-sprouted; nothing was carried.
    assert not report.emissions_carried
    assert not report.emission_zones_carried
