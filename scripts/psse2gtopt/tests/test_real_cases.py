"""Validation against vendored real-world PSS/E cases.

The parse + convert assertions run everywhere (pure Python).  The
end-to-end gtopt solve is marked ``integration`` and skips when no
gtopt binary is available.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from psse2gtopt.gtopt_writer import build_planning
from psse2gtopt.raw_parser import parse_raw


def _planning_for(raw: Path) -> dict:
    return build_planning(parse_raw(raw), name=raw.stem)


def test_ieee14_convert_schema(ieee14_raw: Path) -> None:
    planning = _planning_for(ieee14_raw)
    system = planning["system"]
    assert len(system["bus_array"]) == 14  # no 3-winding star buses
    assert len(system["generator_array"]) == 5
    assert len(system["demand_array"]) == 11
    # 16 branches + 4 two-winding transformers.
    assert len(system["line_array"]) == 20
    names = {b["name"] for b in system["bus_array"]}
    for line in system["line_array"]:
        assert line["bus_a"] in names and line["bus_b"] in names


def test_ieee39_convert_schema(ieee39_raw: Path) -> None:
    planning = _planning_for(ieee39_raw)
    system = planning["system"]
    assert len(system["bus_array"]) == 39
    assert len(system["generator_array"]) == 14
    assert len(system["demand_array"]) == 19
    # 34 branches + 12 two-winding transformers.
    assert len(system["line_array"]) == 46


@pytest.mark.parametrize(
    ("fixture_name", "expected_served"),
    [("ieee14.raw", 223.7), ("ieee39.raw", 5856.8)],
)
@pytest.mark.integration
def test_real_case_solves(
    data_dir: Path,
    gtopt_bin: str,
    tmp_path: Path,
    fixture_name: str,
    expected_served: float,
) -> None:
    """gtopt builds and solves the converted DC OPF to optimality."""
    raw = data_dir / fixture_name
    if not raw.is_file():
        pytest.skip(f"fixture not found: {raw}")
    planning = _planning_for(raw)
    json_file = tmp_path / "case.json"
    json_file.write_text(json.dumps(planning), encoding="utf-8")
    out_dir = tmp_path / "out"

    subprocess.run(
        [gtopt_bin, "-s", str(json_file), "-d", str(out_dir), "-l", "ERROR"],
        check=True,
        timeout=120,
    )

    status = json.loads((out_dir / "solver_status.json").read_text(encoding="utf-8"))
    assert status["status"] == "done"

    import pandas as pd  # pylint: disable=import-outside-toplevel

    load = pd.read_csv(out_dir / "Demand" / "load_sol_s0_p0.csv")
    served = load[load.columns[-1]].sum()
    assert served == pytest.approx(expected_served, abs=1.0)
