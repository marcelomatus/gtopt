"""Integration tests for :func:`sddp2gtopt.convert_sddp_case`."""

from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path

import pytest

from sddp2gtopt.sddp2gtopt import (
    _resolve_output_paths,
    convert_sddp_case,
    validate_sddp_case,
)


# --- validate_sddp_case ----------------------------------------------


def test_validate_case0_ok(case0_dir: Path) -> None:
    assert validate_sddp_case({"input_dir": case0_dir}) is True


def test_validate_min_ok(case_min_dir: Path) -> None:
    assert validate_sddp_case({"input_dir": case_min_dir}) is True


def test_validate_no_input_returns_false() -> None:
    assert validate_sddp_case({}) is False


def test_validate_no_study_returns_false(case_bad_no_study_dir: Path) -> None:
    assert validate_sddp_case({"input_dir": case_bad_no_study_dir}) is False


def test_validate_truncated_returns_false(
    case_bad_truncated_dir: Path, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.ERROR, logger="sddp2gtopt.sddp2gtopt"):
        assert validate_sddp_case({"input_dir": case_bad_truncated_dir}) is False
    assert any("validate failed" in r.message for r in caplog.records)


def test_validate_missing_dir_returns_false(tmp_path: Path) -> None:
    assert validate_sddp_case({"input_dir": tmp_path / "nope"}) is False


# --- _resolve_output_paths -------------------------------------------


def test_resolve_paths_strips_sddp_prefix(tmp_path: Path) -> None:
    inp = tmp_path / "sddp_demo"
    inp.mkdir()
    out_dir, out_file, name = _resolve_output_paths(inp, None, None, None)
    assert out_dir.name == "gtopt_demo"
    assert out_file.name == "gtopt_demo.json"
    assert name == "gtopt_demo"


def test_resolve_paths_default_prefix(tmp_path: Path) -> None:
    inp = tmp_path / "raw_case"
    inp.mkdir()
    out_dir, out_file, _ = _resolve_output_paths(inp, None, None, None)
    assert out_dir.name == "gtopt_raw_case"
    assert out_file.parent == out_dir


def test_resolve_paths_explicit_overrides_inference(tmp_path: Path) -> None:
    inp = tmp_path / "sddp_demo"
    inp.mkdir()
    out_dir = tmp_path / "custom"
    out_file = tmp_path / "explicit.json"
    res_dir, res_file, name = _resolve_output_paths(
        inp, out_dir, out_file, "explicit_name"
    )
    assert res_dir == out_dir
    assert res_file == out_file
    assert name == "explicit_name"


# --- convert_sddp_case -----------------------------------------------


def test_convert_no_input_raises() -> None:
    with pytest.raises(ValueError, match="input_dir"):
        convert_sddp_case({})


def test_convert_case_min_writes_planning(case_min_dir: Path, tmp_path: Path) -> None:
    out = tmp_path / "out" / "min.json"
    rc = convert_sddp_case(
        {
            "input_dir": case_min_dir,
            "output_dir": tmp_path / "out",
            "output_file": out,
            "name": "case_min",
        }
    )
    assert rc == 0
    assert out.is_file()
    plan = json.loads(out.read_text(encoding="utf-8"))
    assert plan["system"]["name"] == "case_min"
    # 1 stage × 1 block, 1 thermal, 1 demand
    assert len(plan["simulation"]["block_array"]) == 1
    assert len(plan["system"]["generator_array"]) == 1
    assert plan["system"]["generator_array"][0]["name"] == "MinThermal"
    assert plan["system"]["demand_array"][0]["name"] == "MinLoad"


def test_convert_case0_includes_thermal_and_hydro(
    case0_dir: Path, tmp_path: Path
) -> None:
    out_dir = tmp_path / "case0_out"
    rc = convert_sddp_case({"input_dir": case0_dir, "output_dir": out_dir})
    assert rc == 0
    out_file = next(out_dir.glob("*.json"))
    plan = json.loads(out_file.read_text(encoding="utf-8"))
    names = [g["name"] for g in plan["system"]["generator_array"]]
    assert "Thermal 1" in names
    assert "Thermal 2" in names
    assert "Thermal 3" in names
    assert "Hydro 1" in names
    # Single bus synthesised from PSRSystem
    assert len(plan["system"]["bus_array"]) == 1
    assert plan["system"]["bus_array"][0]["name"] == "sys_1_bus"
    # Two stages, one block per stage
    assert len(plan["simulation"]["stage_array"]) == 2
    assert len(plan["simulation"]["block_array"]) == 2


def test_convert_case_thermal_only(case_thermal_only_dir: Path, tmp_path: Path) -> None:
    out_dir = tmp_path / "to"
    rc = convert_sddp_case({"input_dir": case_thermal_only_dir, "output_dir": out_dir})
    assert rc == 0
    plan = json.loads(next(out_dir.glob("*.json")).read_text(encoding="utf-8"))
    # 3 stages × 2 blocks
    assert len(plan["simulation"]["block_array"]) == 6
    # Two thermals, no hydro
    gens = plan["system"]["generator_array"]
    assert len(gens) == 2
    # Currency from study
    assert plan["options"]["annual_discount_rate"] == pytest.approx(0.05)


def test_convert_two_systems_rejected(
    case_two_systems_dir: Path, tmp_path: Path
) -> None:
    with pytest.raises(ValueError, match="single-system"):
        convert_sddp_case(
            {
                "input_dir": case_two_systems_dir,
                "output_dir": tmp_path / "x",
            }
        )


def test_convert_truncated_raises(case_bad_truncated_dir: Path, tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        convert_sddp_case(
            {
                "input_dir": case_bad_truncated_dir,
                "output_dir": tmp_path / "x",
            }
        )


# --- end-to-end with the gtopt binary --------------------------------


@pytest.mark.integration
def test_case0_lp_only_smoke(case0_dir: Path, tmp_path: Path, gtopt_bin: str) -> None:
    """Convert case0 and let ``gtopt --lp-only`` confirm the LP builds.

    This is the v0 acceptance test: it ensures the converter produces
    a planning that gtopt can ingest, build the LP, and exit cleanly.
    """
    out_dir = tmp_path / "case0_e2e"
    convert_sddp_case({"input_dir": case0_dir, "output_dir": out_dir})
    plan_file = next(out_dir.glob("*.json"))
    proc = subprocess.run(
        [gtopt_bin, "--lp-only", "-s", str(plan_file)],
        capture_output=True,
        check=False,
        timeout=60,
    )
    assert proc.returncode == 0, (
        f"gtopt --lp-only failed:\nstdout:\n{proc.stdout.decode()}\n"
        f"stderr:\n{proc.stderr.decode()}"
    )
