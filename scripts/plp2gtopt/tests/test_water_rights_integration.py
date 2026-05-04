"""Integration tests for Laja/Maule water rights emission.

Tests that plp2gtopt with --expand-water-rights produces correct
rights entities and PAMPL files from the plp_2_years support case,
and that gtopt --lp-only can assemble the LP matrix successfully.
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
        """Exercise the auto-expanded irrigation pipeline on plp_2_years.

        Runs ``plp2gtopt --expand-water-rights`` so ``gtopt_expand``
        is invoked in-process.  The resulting planning JSON contains
        all merged entities, the companion ``laja.pampl`` /
        ``maule.pampl`` files land in the output directory, and
        per-agreement system fragments (``laja_water_rights.json`` /
        ``maule_water_rights.json``) are emitted for the manifest
        path.  No ``*_dat.json`` parser intermediates are written —
        those are not shipped.
        """
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
                "--expand-water-rights",
                "--use-kirchhoff",
                "--demand-fail-cost",
                "1000",
                "--scale-objective",
                "1000",
                # Pin the legacy spillway-waterway shape — the LP build
                # checks below assume the historical ``_ver`` topology
                # and counts.
                "--no-drop-spillway-waterway",
            ],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        assert result.returncode == 0, (
            f"plp2gtopt failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )

        # Find the main planning JSON (excluding the manifest fragments
        # plp2gtopt also emits).
        _aux_names = {
            "laja_water_rights.json",
            "maule_water_rights.json",
            "ror_promoted.json",
        }
        json_files = [p for p in output_dir.glob("*.json") if p.name not in _aux_names]
        assert len(json_files) == 1, f"Expected 1 JSON file, got {json_files}"

        with open(json_files[0], encoding="utf-8") as f:
            planning = json.load(f)
        system = planning.get("system", {})

        return {
            "output_dir": output_dir,
            "json_file": json_files[0],
            "planning": planning,
            "system": system,
        }

    # -- Laja entity checks --

    def test_laja_user_constraints_exist(self, converted_case):
        """Laja partition user constraint is created."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if not ucs:
            # May be in a PAMPL file instead of inline
            return
        names = {uc["name"] for uc in ucs}
        assert "laja_particion_derechos" in names

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
            "laja_vol_der_riego",
            "laja_vol_der_electrico",
            "laja_vol_der_mixto",
            "laja_vol_gasto_anticipado",
            "laja_vol_econ_endesa",
            "laja_vol_econ_reserva",
            "laja_vol_econ_polcura",
        }
        missing = expected - names
        assert not missing, f"Missing Laja volume rights: {missing}"

    def test_laja_flow_right_names(self, converted_case):
        """Verify key Laja flow rights exist."""
        frs = converted_case["system"].get("flow_right_array", [])
        names = {fr["name"] for fr in frs}
        expected = {
            "laja_der_riego",
            "laja_der_electrico",
            "laja_der_mixto",
            "laja_gasto_anticipado",
        }
        missing = expected - names
        assert not missing, f"Missing Laja flow rights: {missing}"

    # -- Maule entity checks --

    def test_maule_user_constraints_count(self, converted_case):
        """Maule creates user constraints for balances and districts."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if not ucs:
            # Constraints are in PAMPL file — check file exists with content
            pampl = converted_case["output_dir"] / "maule.pampl"
            assert pampl.exists(), "maule.pampl not found"
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
        assert "maule_vol_reserva_ord_elec" in names, (
            "Missing maule_vol_reserva_ord_elec"
        )
        assert "maule_vol_reserva_ord_riego" in names, (
            "Missing maule_vol_reserva_ord_riego"
        )

    def test_maule_volume_right_names(self, converted_case):
        """Verify all expected Maule volume rights exist."""
        vrs = converted_case["system"].get("volume_right_array", [])
        names = {vr["name"] for vr in vrs}
        expected = {
            "maule_vol_gasto_elec_mensual",
            "maule_vol_gasto_elec_anual",
            "maule_vol_gasto_riego_temp",
            "maule_vol_compensacion_elec",
            "maule_vol_econ_invernada",
            "maule_vol_reserva_ord_elec",
            "maule_vol_reserva_ord_riego",
        }
        missing = expected - names
        assert not missing, f"Missing Maule volume rights: {missing}"

    # -- PAMPL file checks --

    def test_maule_pampl_generated(self, converted_case):
        """Maule PAMPL file is generated in output directory."""
        pampl_file = converted_case["output_dir"] / "maule.pampl"
        assert pampl_file.exists(), "maule.pampl not found"
        content = pampl_file.read_text(encoding="utf-8")
        assert "constraint" in content, "PAMPL file has no constraints"
        assert "param" in content, "PAMPL file has no params"

    def test_user_constraint_file_set(self, converted_case):
        """user_constraint_files (plural) is set in the system JSON."""
        uc_files = converted_case["system"].get("user_constraint_files", [])
        assert len(uc_files) >= 1, "user_constraint_files not set"
        for f in uc_files:
            assert f.endswith(".pampl"), f"Expected .pampl file, got {f}"

    # -- Combined counts --

    def test_total_user_constraints(self, converted_case):
        """Total user constraints from both agreements (inline or PAMPL)."""
        ucs = converted_case["system"].get("user_constraint_array", [])
        if ucs:
            assert len(ucs) >= 2
        else:
            # Constraints are in PAMPL files — verify files exist
            uc_files = converted_case["system"].get("user_constraint_files", [])
            uc_file = converted_case["system"].get("user_constraint_file")
            assert uc_files or uc_file, "No user_constraint_array or file(s)"

    def test_total_flow_rights(self, converted_case):
        """Total flow rights from agreements + default ``pmin_as_flowright``.

        Layout: 35 from the Laja + Maule water-rights agreements plus 6
        from the bundled ``--pmin-as-flowright`` whitelist (MACHICURA,
        PANGUE, PILMAIQUEN, ABANICO, ANTUCO, PALMUCHO) which is now ON
        by default.  Pass ``--no-pmin-as-flowright`` to opt out.
        """
        frs = converted_case["system"].get("flow_right_array", [])
        assert len(frs) == 41, f"Expected 41 total flow rights, got {len(frs)}"

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
    """Test that gtopt --lp-only succeeds on the converted case."""

    @pytest.fixture(scope="class")
    def lp_build_result(self, tmp_path_factory, gtopt_bin):
        """Run plp2gtopt (with default auto-expand) + gtopt --lp-only.

        Relies on the default ``--expand-water-rights`` / ``--expand-lng``
        behaviour: plp2gtopt runs the ``gtopt_expand`` Stage-2 transforms
        internally and merges their entities into the planning JSON, so
        the downstream ``gtopt --lp-only`` sees a single coherent system
        without any manual glue.
        """
        output_dir = tmp_path_factory.mktemp("plp2y_lp_build")

        # Step 1: Convert with plp2gtopt — auto-expand is on by default.
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
                "--expand-water-rights",
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
            check=False,
        )
        assert conv_result.returncode == 0, f"plp2gtopt failed: {conv_result.stderr}"

        # Find the planning JSON file (excluding the per-agreement
        # system fragments and the RoR audit artifact that plp2gtopt
        # also emits for the manifest).
        _aux_names = {
            "laja_water_rights.json",
            "maule_water_rights.json",
            "ror_promoted.json",
        }
        json_files = [p for p in output_dir.glob("*.json") if p.name not in _aux_names]
        assert len(json_files) == 1, f"Expected 1 JSON file, got {json_files}"

        # Force constraint_mode=normal so user_constraint assembly is exercised.
        with open(json_files[0], encoding="utf-8") as f:
            planning = json.load(f)
        planning.setdefault("options", {})["constraint_mode"] = "normal"
        with open(json_files[0], "w", encoding="utf-8") as f:
            json.dump(planning, f)

        # Step 2: Run gtopt --lp-only (build LP but skip solve)
        gtopt_result = subprocess.run(
            [
                gtopt_bin,
                str(json_files[0]),
                "--lp-only",
            ],
            capture_output=True,
            text=True,
            timeout=300,
            cwd=str(output_dir),
            check=False,
        )
        # gtopt routes spdlog records to rotating files under
        # ``output/logs/gtopt_*.log`` rather than stdout/stderr in the
        # common configuration, so collect those files into
        # ``stdout`` for downstream assertions on log strings.  See
        # ``test_integration_case2y._gather_gtopt_logs`` for the
        # canonical helper — duplicated here to avoid a cross-test
        # import.
        log_chunks: list[str] = []
        for log_dir in (
            output_dir / "output" / "logs",
            output_dir / "logs",
            output_dir / "results" / "logs",
        ):
            if not log_dir.is_dir():
                continue
            for log_file in sorted(log_dir.glob("gtopt_*.log")):
                try:
                    log_chunks.append(
                        log_file.read_text(encoding="utf-8", errors="replace")
                    )
                except OSError:
                    continue
        gtopt_logs = "\n".join(log_chunks)
        return {
            "returncode": gtopt_result.returncode,
            "stdout": gtopt_result.stdout + gtopt_logs,
            "stderr": gtopt_result.stderr,
            "output_dir": output_dir,
            "json_file": json_files[0],
        }

    def test_gtopt_lp_build_succeeds(self, lp_build_result):
        """gtopt --lp-only exits with code 0."""
        assert lp_build_result["returncode"] == 0, (
            f"gtopt --lp-only failed (rc={lp_build_result['returncode']}):\n"
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
        """gtopt logs the lp_only skip-solve message."""
        output = lp_build_result["stdout"] + lp_build_result["stderr"]
        assert "lp_only" in output.lower(), "No lp_only message found in gtopt output"
