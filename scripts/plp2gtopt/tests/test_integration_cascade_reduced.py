"""Integration test for plp2gtopt --method cascade-reduced.

Skipped unless ``GTOPT_BIN`` is set (or ``gtopt`` is on ``$PATH``) — the
test invokes the C++ binary to confirm the JSON shape with
``system_file`` fields is accepted end-to-end.

Pipeline:
  1. Patch an IEEE-57 case to declare a cascade with two ``system_file``
     levels.
  2. Run the reducer directly to emit the L1/L2 artefacts alongside the
     main JSON.
  3. Invoke gtopt on the main JSON; assert exit 0 + log file produced.

Note: IEEE-57 has a single phase so the cascade short-circuits to
monolithic (``num_phases < 2`` in
``source/planning_method.cpp``).  The actual in-cascade ``system_file``
swap path is exercised by the C++ tests in
``test_cascade_method.cpp`` / ``test_cascade_cut_inheritance.cpp``;
this test just covers the JSON-parse + reducer-emit end of the pipeline.

To run locally:
    GTOPT_BIN=$(python tools/get_gtopt_binary.py) \\
        pytest -m integration scripts/plp2gtopt/tests/test_integration_cascade_reduced.py -v
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.integration


def _gtopt_bin() -> str | None:
    explicit = os.environ.get("GTOPT_BIN")
    if explicit and Path(explicit).exists():
        return explicit
    return shutil.which("gtopt")


@pytest.fixture(scope="module")
def gtopt_bin() -> str:
    bin_path = _gtopt_bin()
    if bin_path is None:
        pytest.skip("GTOPT_BIN not set and `gtopt` not on PATH")
    return bin_path


@pytest.fixture
def ieee57_path() -> Path:
    p = Path(__file__).resolve().parents[3] / "cases" / "ieee_57b" / "ieee_57b.json"
    if not p.exists():
        pytest.skip(f"missing case: {p}")
    return p


def test_cascade_reduced_drives_gtopt_with_swapped_systems(
    tmp_path: Path, ieee57_path: Path, gtopt_bin: str
) -> None:
    """End-to-end: emit reduced JSONs + run gtopt with system_file levels."""
    case = json.loads(ieee57_path.read_text(encoding="utf-8"))
    # C++ side accepts only sddp/monolithic/cascade — the cascade-reduced
    # flavour is selected by the level_array shape (system_file fields),
    # not a new MethodType value.
    case["options"]["method"] = "cascade"
    case["options"]["cascade_options"] = {
        "model_options": case["options"].get("model_options", {}),
        "sddp_options": {"max_iterations": 3, "convergence_tol": 0.05},
        "level_array": [
            {
                "uid": 1,
                "name": "uninodal",
                "model_options": {"use_single_bus": True},
                "sddp_options": {
                    "max_iterations": 2,
                    "num_apertures": 1,
                    "aperture_selection_mode": "head",
                },
            },
            {
                "uid": 2,
                "name": "reduced_transport_K10",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": False,
                    "use_line_losses": False,
                },
                "system_file": "ieee_57.L1.json",
                "sddp_options": {
                    "max_iterations": 1,
                    "num_apertures": 1,
                    "aperture_selection_mode": "head",
                },
                "transition": {"inherit_optimality_cuts": -1},
            },
            {
                "uid": 3,
                "name": "reduced_dcopf_K20",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": True,
                    "use_line_losses": False,
                },
                "system_file": "ieee_57.L2.json",
                "sddp_options": {"max_iterations": 1},
                "transition": {"inherit_optimality_cuts": -1},
            },
            {
                "uid": 4,
                "name": "full_network",
                "sddp_options": {"max_iterations": 1},
                "transition": {"inherit_optimality_cuts": -1},
            },
        ],
    }
    main_path = tmp_path / "ieee_57.json"
    main_path.write_text(json.dumps(case), encoding="utf-8")

    # pylint: disable=import-outside-toplevel
    from gtopt_reduce_network import load_case as load_red_case
    from gtopt_reduce_network import save_case as save_red_case
    from gtopt_reduce_network._reduce import ReduceConfig, reduce_case

    cfg_l1 = ReduceConfig(target_buses=10, transport_only=True, loss_mode="off")
    cfg_l2 = ReduceConfig(
        target_buses=20,
        transport_only=False,
        loss_mode="uplift",
        loss_uplift_pct=3.0,
        distance="ptdf",
    )
    res_l1 = reduce_case(load_red_case(main_path), cfg_l1)
    save_red_case(res_l1.case, tmp_path / "ieee_57.L1.json")
    res_l2 = reduce_case(load_red_case(main_path), cfg_l2)
    save_red_case(res_l2.case, tmp_path / "ieee_57.L2.json")
    assert (tmp_path / "ieee_57.L1.json").exists()
    assert (tmp_path / "ieee_57.L2.json").exists()

    out_dir = tmp_path / "out"
    out_dir.mkdir()
    proc = subprocess.run(
        [gtopt_bin, "-s", str(main_path), "-d", str(out_dir)],
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
        cwd=tmp_path,
    )
    assert proc.returncode == 0, (
        f"gtopt failed with code {proc.returncode}\n"
        f"stdout:\n{proc.stdout[:4000]}\n"
        f"stderr:\n{proc.stderr[:4000]}"
    )
    log_files = list((out_dir / "logs").glob("gtopt_*.log"))
    assert log_files, "gtopt did not produce a log file"
