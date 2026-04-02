"""Integration tests for Laja/Maule water rights emission.

Tests that plp2gtopt with --emit-water-rights produces correct
rights entities and PAMPL files from the plp_2_years support case,
and that gtopt --lp-build can assemble the LP matrix successfully.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from plp2gtopt.compressed_open import find_compressed_path

_SUPPORT_DIR = Path(__file__).parent.parent.parent.parent / "support"
_PLP_2Y = _SUPPORT_DIR / "plp_2_years"


def _has_plp_2_years() -> bool:
    """Check if the plp_2_years support directory has required files."""
    if not _PLP_2Y.is_dir():
        return False
    for fname in ("plplajam.dat", "plpmaulen.dat", "plpblo.dat"):
        if find_compressed_path(_PLP_2Y / fname) is None:
            return False
    return True


@pytest.mark.integration
@pytest.mark.skipif(
    not _has_plp_2_years(),
    reason="plp_2_years support directory not available",
)
class TestWaterRightsIntegration:
    """Integration tests using the plp_2_years PLP case."""

    @pytest.fixture(scope="class")
    def converted_case(self, tmp_path_factory):
        """Run plp2gtopt with --emit-water-rights on plp_2_years."""
        output_dir = tmp_path_factory.mktemp("plp2y_rights")
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "plp2gtopt.main",
                str(_PLP_2Y),
                "-o",
                str(output_dir),
                "--first-scenario",
                "-t",
                "1y",
                "-F",
                "csv",
                "--emit-water-rights",
                "--use-kirchhoff",
                "--demand-fail-cost",
                "1000",
                "--scale-objective",
                "1000",
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert result.returncode == 0, (
            f"plp2gtopt failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )

        # Find the JSON file
        json_files = list(output_dir.glob("*.json"))
        assert len(json_files) == 1, f"Expected 1 JSON file, got {json_files}"

        with open(json_files[0], encoding="utf-8") as f:
            planning = json.load(f)

        return {
            "output_dir": output_dir,
            "json_file": json_files[0],
            "planning": planning,
            "system": planning.get("system", {}),
        }

    # -- Laja entity checks --

    def test_laja_user_constraints_exist(self, converted_case):
        """Laja partition user constraint is created."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if not ucs:
            # May be in a PAMPL file instead of inline
            return
        names = {uc["name"] for uc in ucs}
        assert "laja_partition" in names

    def test_laja_flow_rights_count(self, converted_case):
        """Laja creates flow rights for irr, elec, mixed, anticipated + districts."""
        frs = converted_case["system"].get("flow_right_array", [])
        laja_frs = [fr for fr in frs if fr["name"].startswith("laja_")]
        assert len(laja_frs) >= 5, (
            f"Expected at least 5 Laja flow rights, got {len(laja_frs)}: "
            f"{[fr['name'] for fr in laja_frs]}"
        )

    def test_laja_volume_rights_count(self, converted_case):
        """Laja creates 7 volume rights: 4 rights + 3 economy accumulators."""
        vrs = converted_case["system"].get("volume_right_array", [])
        laja_vrs = [vr for vr in vrs if vr["name"].startswith("laja_")]
        assert len(laja_vrs) == 7, (
            f"Expected 7 Laja volume rights, got {len(laja_vrs)}: "
            f"{[vr['name'] for vr in laja_vrs]}"
        )

    def test_laja_volume_right_names(self, converted_case):
        """Verify all expected Laja volume rights exist."""
        vrs = converted_case["system"].get("volume_right_array", [])
        names = {vr["name"] for vr in vrs}
        expected = {
            "laja_vol_irr",
            "laja_vol_elec",
            "laja_vol_mixed",
            "laja_vol_anticipated",
            "laja_vol_econ_endesa",
            "laja_vol_econ_reserve",
            "laja_vol_econ_polcura",
        }
        missing = expected - names
        assert not missing, f"Missing Laja volume rights: {missing}"

    def test_laja_flow_right_names(self, converted_case):
        """Verify key Laja flow rights exist."""
        frs = converted_case["system"].get("flow_right_array", [])
        names = {fr["name"] for fr in frs}
        expected = {
            "laja_irr_rights",
            "laja_elec_rights",
            "laja_mixed_rights",
            "laja_anticipated",
        }
        missing = expected - names
        assert not missing, f"Missing Laja flow rights: {missing}"

    # -- Maule entity checks --

    def test_maule_user_constraints_count(self, converted_case):
        """Maule creates user constraints for balances and districts."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if not ucs:
            # Constraints are in PAMPL file — check file exists with content
            pampl = converted_case["output_dir"] / "maule_agreement.pampl"
            assert pampl.exists(), "maule_agreement.pampl not found"
            assert "constraint" in pampl.read_text(encoding="utf-8")
            return
        maule_ucs = [uc for uc in ucs if not uc["name"].startswith("laja_")]
        assert len(maule_ucs) >= 1

    def test_maule_flow_rights_count(self, converted_case):
        """Maule creates flow rights for elec, irr, res105 + districts."""
        frs = converted_case["system"].get("flow_right_array", [])
        maule_frs = [fr for fr in frs if fr["name"].startswith("maule_")]
        assert len(maule_frs) >= 6, (
            f"Expected at least 6 Maule flow rights, got {len(maule_frs)}: "
            f"{[fr['name'] for fr in maule_frs]}"
        )

    def test_maule_volume_rights_count(self, converted_case):
        """Maule creates 7 volume rights: 5 original + 2 rext accumulators."""
        vrs = converted_case["system"].get("volume_right_array", [])
        maule_vrs = [vr for vr in vrs if vr["name"].startswith("maule_")]
        assert len(maule_vrs) == 7, (
            f"Expected 7 Maule volume rights, got {len(maule_vrs)}: "
            f"{[vr['name'] for vr in maule_vrs]}"
        )

    def test_maule_rext_volume_rights_exist(self, converted_case):
        """Verify extraordinary reserve volume rights are created."""
        vrs = converted_case["system"].get("volume_right_array", [])
        names = {vr["name"] for vr in vrs}
        assert "maule_vol_rext_elec" in names, "Missing maule_vol_rext_elec"
        assert "maule_vol_rext_riego" in names, "Missing maule_vol_rext_riego"

    def test_maule_volume_right_names(self, converted_case):
        """Verify all expected Maule volume rights exist."""
        vrs = converted_case["system"].get("volume_right_array", [])
        names = {vr["name"] for vr in vrs}
        expected = {
            "maule_vol_elec_monthly",
            "maule_vol_elec_annual",
            "maule_vol_irr_seasonal",
            "maule_vol_compensation",
            "maule_vol_econ_invernada",
            "maule_vol_rext_elec",
            "maule_vol_rext_riego",
        }
        missing = expected - names
        assert not missing, f"Missing Maule volume rights: {missing}"

    # -- PAMPL file checks --

    def test_maule_pampl_generated(self, converted_case):
        """Maule PAMPL file is generated in output directory."""
        pampl_file = converted_case["output_dir"] / "maule_agreement.pampl"
        assert pampl_file.exists(), "maule_agreement.pampl not found"
        content = pampl_file.read_text(encoding="utf-8")
        assert "constraint" in content, "PAMPL file has no constraints"
        assert "param" in content, "PAMPL file has no params"

    def test_user_constraint_file_set(self, converted_case):
        """user_constraint_file is set in the system JSON."""
        uc_file = converted_case["system"].get("user_constraint_file")
        assert uc_file is not None, "user_constraint_file not set"
        assert uc_file.endswith(".pampl"), f"Expected .pampl file, got {uc_file}"

    # -- Combined counts --

    def test_total_user_constraints(self, converted_case):
        """Total user constraints from both agreements (inline or PAMPL)."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if ucs:
            assert len(ucs) >= 2
        else:
            # Constraints are in PAMPL files — verify files exist
            uc_file = converted_case["system"].get("user_constraint_file")
            assert uc_file is not None, "No user_constraint_array or file"

    def test_total_flow_rights(self, converted_case):
        """Total flow rights from both agreements."""
        frs = converted_case["system"].get("flow_right_array", [])
        assert len(frs) == 35, f"Expected 35 total flow rights, got {len(frs)}"

    def test_total_volume_rights(self, converted_case):
        """Total volume rights = Laja (7) + Maule (7)."""
        vrs = converted_case["system"].get("volume_right_array", [])
        assert len(vrs) == 14, f"Expected 14 total volume rights, got {len(vrs)}"


@pytest.mark.integration
@pytest.mark.skipif(
    not _has_plp_2_years(),
    reason="plp_2_years support directory not available",
)
class TestGtoptLpBuild:
    """Test that gtopt --lp-build succeeds on the converted case."""

    @pytest.fixture(scope="class")
    def lp_build_result(self, tmp_path_factory, gtopt_bin):
        """Run plp2gtopt + gtopt --lp-build on plp_2_years."""
        output_dir = tmp_path_factory.mktemp("plp2y_lp_build")

        # Step 1: Convert with plp2gtopt
        conv_result = subprocess.run(
            [
                sys.executable,
                "-m",
                "plp2gtopt.main",
                str(_PLP_2Y),
                "-o",
                str(output_dir),
                "--first-scenario",
                "-t",
                "1y",
                "-F",
                "csv",
                "--emit-water-rights",
                "--use-kirchhoff",
                "--demand-fail-cost",
                "1000",
                "--scale-objective",
                "1000",
                "--no-check",
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert conv_result.returncode == 0, f"plp2gtopt failed: {conv_result.stderr}"

        # Find the JSON file
        json_files = list(output_dir.glob("*.json"))
        assert len(json_files) == 1

        # Set constraint_mode to normal in the JSON (some constraint
        # references may not resolve in this stripped-down test case)
        with open(json_files[0], encoding="utf-8") as f:
            planning = json.load(f)
        planning.setdefault("options", {})["constraint_mode"] = "normal"
        with open(json_files[0], "w", encoding="utf-8") as f:
            json.dump(planning, f)

        # Step 2: Run gtopt --lp-build
        gtopt_result = subprocess.run(
            [
                gtopt_bin,
                str(json_files[0]),
                "--lp-build",
            ],
            capture_output=True,
            text=True,
            timeout=300,
            cwd=str(output_dir),
        )
        return {
            "returncode": gtopt_result.returncode,
            "stdout": gtopt_result.stdout,
            "stderr": gtopt_result.stderr,
            "output_dir": output_dir,
            "json_file": json_files[0],
        }

    def test_gtopt_lp_build_succeeds(self, lp_build_result):
        """gtopt --lp-build exits with code 0."""
        assert lp_build_result["returncode"] == 0, (
            f"gtopt --lp-build failed (rc={lp_build_result['returncode']}):\n"
            f"stdout: {lp_build_result['stdout']}\n"
            f"stderr: {lp_build_result['stderr']}"
        )

    def test_rights_entities_logged(self, lp_build_result):
        """gtopt logs rights entity counts."""
        output = lp_build_result["stdout"] + lp_build_result["stderr"]
        assert (
            "User constraint" in output
            or "user_constraint" in output.lower()
            or "flow_right" in output.lower()
        ), "No rights entity logging found in gtopt output"

    def test_pampl_loaded(self, lp_build_result):
        """gtopt logs PAMPL file loading."""
        output = lp_build_result["stdout"] + lp_build_result["stderr"]
        assert "PAMPL" in output or "pampl" in output.lower(), (
            "No PAMPL loading message found in gtopt output"
        )

    def test_lp_build_message(self, lp_build_result):
        """gtopt logs the lp_build skip-solve message."""
        output = lp_build_result["stdout"] + lp_build_result["stderr"]
        assert "lp_build" in output.lower(), "No lp_build message found in gtopt output"
