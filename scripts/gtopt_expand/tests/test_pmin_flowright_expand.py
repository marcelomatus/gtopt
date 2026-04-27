# -*- coding: utf-8 -*-

"""Tests for the pmin-as-FlowRight expansion (CSV parsing + transform + CLI)."""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

import pytest

from gtopt_expand.cli import main as cli_main
from gtopt_expand.pmin_flowright_expand import (
    DEFAULT_PMIN_CSV,
    DEFAULT_UID_START,
    PminFlowRightSpec,
    expand_pmin_flowright,
    expand_pmin_flowright_from_file,
    parse_pmin_flowright_file,
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
_MINIMAL_CSV = """\
name,enabled,description
PANGUE,true,minimum flow downstream of Ralco
ABANICO,false,disabled in this fixture
"""


def _write_csv(
    tmp_path: Path, content: str = _MINIMAL_CSV, name: str = "p.csv"
) -> Path:
    path = tmp_path / name
    path.write_text(content, encoding="utf-8")
    return path


def _build_planning(
    *,
    central: str = "PANGUE",
    pmin: Any = 50.0,
    rendi: float = 0.8,
    waterway_name: str | None = None,
    junction_b: str | None = "PANGUE_downstream",
    extra_generators: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Build a minimal planning JSON containing a single hydro central."""
    ww_name = waterway_name or f"{central}_gen"
    generator: dict[str, Any] = {"uid": 1, "name": central, "bus": "b1"}
    if pmin is not None:
        generator["pmin"] = pmin
    waterway: dict[str, Any] = {
        "uid": 1,
        "name": ww_name,
        "junction_a": central,
    }
    if junction_b is not None:
        waterway["junction_b"] = junction_b
    return {
        "system": {
            "generator_array": [generator] + list(extra_generators or []),
            "turbine_array": [
                {
                    "uid": 1,
                    "name": central,
                    "generator": central,
                    "waterway": ww_name,
                    "production_factor": rendi,
                },
            ],
            "waterway_array": [waterway],
        },
    }


def _write_planning(tmp_path: Path, planning: dict[str, Any]) -> Path:
    path = tmp_path / "gtopt.json"
    path.write_text(json.dumps(planning), encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# parse_pmin_flowright_file
# ---------------------------------------------------------------------------
class TestParsePminFlowRightFile:
    def test_basic(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        spec = parse_pmin_flowright_file(csv_path)

        assert "PANGUE" in spec
        assert "ABANICO" not in spec  # disabled
        assert spec["PANGUE"] == PminFlowRightSpec(
            name="PANGUE",
            description="minimum flow downstream of Ralco",
        )

    def test_missing_file(self, tmp_path: Path) -> None:
        with pytest.raises(FileNotFoundError):
            parse_pmin_flowright_file(tmp_path / "missing.csv")

    def test_missing_column(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path, "name\nPANGUE\n", name="bad.csv")
        with pytest.raises(ValueError, match="missing required column"):
            parse_pmin_flowright_file(csv_path)

    def test_duplicate_name(self, tmp_path: Path) -> None:
        content = "name,enabled\nPANGUE,true\nPANGUE,true\n"
        csv_path = _write_csv(tmp_path, content, name="dup.csv")
        with pytest.raises(ValueError, match="duplicate"):
            parse_pmin_flowright_file(csv_path)

    def test_bundled_csv_exists(self) -> None:
        assert DEFAULT_PMIN_CSV.is_file()

    def test_bundled_csv_parses(self) -> None:
        spec = parse_pmin_flowright_file(DEFAULT_PMIN_CSV)
        # All 6 rows in the bundled CSV are enabled=true.
        assert {
            "MACHICURA",
            "PANGUE",
            "PILMAIQUEN",
            "ABANICO",
            "ANTUCO",
            "PALMUCHO",
        } <= set(spec.keys())


# ---------------------------------------------------------------------------
# expand_pmin_flowright — happy paths
# ---------------------------------------------------------------------------
class TestExpandPminFlowRight:
    def test_scalar_pmin_yields_scalar_discharge(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=50.0, rendi=0.8)

        n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 1
        gen = planning["system"]["generator_array"][0]
        assert gen["pmin"] == 0.0  # overridden

        flow_rights = planning["system"]["flow_right_array"]
        assert len(flow_rights) == 1
        fr = flow_rights[0]
        assert fr["name"] == "PANGUE_pmin_as_flow_right"
        assert fr["uid"] == DEFAULT_UID_START
        assert fr["junction"] == "PANGUE_downstream"
        assert fr["direction"] == -1
        assert fr["purpose"] == "environmental"
        assert fr["discharge"] == pytest.approx(50.0 / 0.8)  # 62.5
        assert "fmax" not in fr  # fixed-mode
        assert "fail_cost" not in fr  # falls back to global hydro_fail_cost

    def test_2d_pmin_yields_3d_discharge(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=[[10.0, 20.0], [30.0, 40.0]], rendi=2.0)

        n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 1
        fr = planning["system"]["flow_right_array"][0]
        # STBRealFieldSched is 3D: [scenario][stage][block].  We wrap the
        # 2D pmin in a single scenario layer.
        assert fr["discharge"] == [[[5.0, 10.0], [15.0, 20.0]]]

    def test_string_pmin_yields_parquet_reference_and_warns(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin="pmin", rendi=0.8)

        with caplog.at_level(
            logging.WARNING, logger="gtopt_expand.pmin_flowright_expand"
        ):
            n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 1
        fr = planning["system"]["flow_right_array"][0]
        assert fr["discharge"] == "PANGUE_pmin_as_flow_right"
        # The TODO warning is emitted so the parquet-side glue is visible.
        assert any(
            "TODO" in rec.message and "PANGUE_pmin_as_flow_right" in rec.message
            for rec in caplog.records
        )

    def test_uid_increments_per_conversion(self, tmp_path: Path) -> None:
        # Two enabled rows with both centrals present.
        csv_path = _write_csv(
            tmp_path,
            "name,enabled\nPANGUE,true\nABANICO,true\n",
            name="two.csv",
        )
        planning = _build_planning(
            extra_generators=[
                {"uid": 2, "name": "ABANICO", "bus": "b2", "pmin": 5.0},
            ],
        )
        # Add the matching turbine + waterway for ABANICO.
        planning["system"]["turbine_array"].append(
            {
                "uid": 2,
                "name": "ABANICO",
                "generator": "ABANICO",
                "waterway": "ABANICO_gen",
                "production_factor": 1.0,
            }
        )
        planning["system"]["waterway_array"].append(
            {
                "uid": 2,
                "name": "ABANICO_gen",
                "junction_a": "ABANICO",
                "junction_b": "ABANICO_downstream",
            }
        )

        n = expand_pmin_flowright(planning, csv_path=csv_path, uid_start=100)

        assert n == 2
        flow_rights = planning["system"]["flow_right_array"]
        uids = sorted(fr["uid"] for fr in flow_rights)
        assert uids == [100, 101]


# ---------------------------------------------------------------------------
# expand_pmin_flowright — skip / warn paths
# ---------------------------------------------------------------------------
class TestExpandPminFlowRightSkips:
    def test_disabled_row_is_skipped(self, tmp_path: Path) -> None:
        # Only ABANICO is in the CSV, and it is disabled.
        csv_path = _write_csv(
            tmp_path,
            "name,enabled\nABANICO,false\n",
            name="off.csv",
        )
        planning = _build_planning(central="ABANICO", pmin=5.0)

        n = expand_pmin_flowright(planning, csv_path=csv_path)
        assert n == 0
        assert "flow_right_array" not in planning["system"]
        # Generator pmin is left untouched when the row is disabled.
        assert planning["system"]["generator_array"][0]["pmin"] == 5.0

    def test_missing_generator_warns_and_skips(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        csv_path = _write_csv(tmp_path)
        # Build a planning where PANGUE is absent.
        planning = _build_planning(central="OTHER", pmin=5.0)

        with caplog.at_level(
            logging.WARNING, logger="gtopt_expand.pmin_flowright_expand"
        ):
            n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 0
        assert any(
            "PANGUE" in rec.message and "not found" in rec.message
            for rec in caplog.records
        )
        # No FlowRight created.
        assert planning["system"].get("flow_right_array", []) == []

    def test_missing_pmin_skips_silently(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=None)

        n = expand_pmin_flowright(planning, csv_path=csv_path)
        assert n == 0
        # No flow_right_array entry was created.
        assert planning["system"].get("flow_right_array", []) == []

    def test_missing_junction_b_skips(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(junction_b=None)

        with caplog.at_level(
            logging.WARNING, logger="gtopt_expand.pmin_flowright_expand"
        ):
            n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 0
        assert any("junction_b" in rec.message for rec in caplog.records)

    def test_non_positive_rendi_skips(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(rendi=0.0)

        with caplog.at_level(
            logging.WARNING, logger="gtopt_expand.pmin_flowright_expand"
        ):
            n = expand_pmin_flowright(planning, csv_path=csv_path)

        assert n == 0
        assert any("production_factor" in rec.message for rec in caplog.records)

    def test_planning_missing_system_raises(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        with pytest.raises(ValueError, match="missing 'system'"):
            expand_pmin_flowright({}, csv_path=csv_path)


# ---------------------------------------------------------------------------
# expand_pmin_flowright_from_file (round-trip)
# ---------------------------------------------------------------------------
class TestExpandPminFlowRightFromFile:
    def test_roundtrip(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=50.0, rendi=0.8)
        in_path = _write_planning(tmp_path, planning)
        out_path = tmp_path / "out.json"

        n = expand_pmin_flowright_from_file(in_path, out_path, csv_path=csv_path)
        assert n == 1
        assert out_path.is_file()

        result = json.loads(out_path.read_text(encoding="utf-8"))
        assert result["system"]["generator_array"][0]["pmin"] == 0.0
        assert result["system"]["flow_right_array"][0]["discharge"] == pytest.approx(
            62.5
        )

    def test_inplace_edit(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=50.0, rendi=0.8)
        in_path = _write_planning(tmp_path, planning)

        n = expand_pmin_flowright_from_file(in_path, in_path, csv_path=csv_path)
        assert n == 1
        result = json.loads(in_path.read_text(encoding="utf-8"))
        assert result["system"]["generator_array"][0]["pmin"] == 0.0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
class TestPminFlowRightCli:
    def test_cli_basic(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=50.0, rendi=0.8)
        in_path = _write_planning(tmp_path, planning)
        out_path = tmp_path / "out.json"

        rc = cli_main(
            [
                "pmin_as_flowright",
                "--in",
                str(in_path),
                "--out",
                str(out_path),
                "--csv",
                str(csv_path),
            ]
        )
        assert rc == 0
        assert out_path.is_file()

        result = json.loads(out_path.read_text(encoding="utf-8"))
        flow_rights = result["system"]["flow_right_array"]
        assert len(flow_rights) == 1
        assert flow_rights[0]["discharge"] == pytest.approx(62.5)

    def test_cli_uid_start(self, tmp_path: Path) -> None:
        csv_path = _write_csv(tmp_path)
        planning = _build_planning(pmin=50.0, rendi=0.8)
        in_path = _write_planning(tmp_path, planning)
        out_path = tmp_path / "out.json"

        rc = cli_main(
            [
                "pmin_as_flowright",
                "--in",
                str(in_path),
                "--out",
                str(out_path),
                "--csv",
                str(csv_path),
                "--uid-start",
                "9000",
            ]
        )
        assert rc == 0
        result = json.loads(out_path.read_text(encoding="utf-8"))
        assert result["system"]["flow_right_array"][0]["uid"] == 9000

    def test_cli_missing_input(self, tmp_path: Path) -> None:
        rc = cli_main(
            [
                "pmin_as_flowright",
                "--in",
                str(tmp_path / "missing.json"),
                "--out",
                str(tmp_path / "out.json"),
            ]
        )
        assert rc == 2
