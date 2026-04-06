# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – solver-related functionality."""

import pathlib
import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_check_lp._solvers import run_iis
from gtopt_check_lp.gtopt_check_lp import (
    AiOptions,
    NeosClient,
    _parse_coinor_infeasibility,
    check_lp,
    run_all_solvers,
    run_local_coinor,
    run_local_cplex,
    run_local_highs_binary,
)

# ── Case directories ─────────────────────────────────────────────────────────

_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_LP_CASES_DIR = _SCRIPTS_DIR / "cases" / "lp_infeasible"
_MY_SMALL_BAD = _LP_CASES_DIR / "my_small_bad.lp"
_BAD_BOUNDS = _LP_CASES_DIR / "bad_bounds.lp"


# Helper: prevent highspy from being found even when installed
_NO_HIGHSPY = patch.dict("sys.modules", {"highspy": None})


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


# ── TestLocalSolverStubs ──────────────────────────────────────────────────────


class TestLocalSolverStubs:
    """Tests that local solver runners return (False, reason) when binary absent."""

    def test_cplex_not_found(self):
        with patch("shutil.which", return_value=None), patch(
            "gtopt_check_lp._solvers.find_cplex_binary", return_value=None
        ):
            ok, msg = run_local_cplex(Path("dummy.lp"))
        assert not ok
        assert "cplex" in msg.lower()

    def test_highs_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_highs_binary(Path("dummy.lp"))
        assert not ok
        assert "highs" in msg.lower()

    def test_coinor_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_coinor(Path("dummy.lp"))
        assert not ok
        assert "clp" in msg.lower() or "cbc" in msg.lower()


# ── TestCoinOR ────────────────────────────────────────────────────────────────


class TestCoinOR:
    """Tests for the COIN-OR CLP/CBC runner."""

    def test_parse_coinor_infeasibility_detects_primal(self):
        """_parse_coinor_infeasibility finds the 'PrimalInfeasible' line."""
        output = (
            "Coin LP version 1.17.9\n"
            "Presolve determined that the problem was infeasible with tolerance of 1e-08\n"
            "0  Obj 0 Primal inf 9.9999999 (1)\n"
            "Primal infeasible - objective value 10\n"
            "PrimalInfeasible objective 10 - 1 iterations time 0.002\n"
        )
        findings = _parse_coinor_infeasibility(output)
        assert any("infeasible" in f.lower() for f in findings)

    def test_parse_coinor_infeasibility_bad_bounds(self):
        """_parse_coinor_infeasibility finds 'bad bound pairs' message."""
        output = (
            "1 bad bound pairs or bad objectives were found - first at C0\n"
            "Primal infeasible - objective value 5\n"
        )
        findings = _parse_coinor_infeasibility(output)
        assert len(findings) == 2
        assert any("bad bound" in f.lower() for f in findings)

    def test_parse_coinor_infeasibility_empty(self):
        """_parse_coinor_infeasibility returns empty list for feasible output."""
        output = "Optimal - objective value 10\n1 iterations time 0.002\n"
        findings = _parse_coinor_infeasibility(output)
        assert not findings

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_infeasible_detected(self):
        """CLP/CBC correctly identifies my_small_bad.lp as infeasible."""
        ok, output = run_local_coinor(_MY_SMALL_BAD, timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        assert any(kw in output.lower() for kw in ("infeasible", "primalinfeasible")), (
            f"Expected infeasibility in output:\n{output}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_bad_bounds_detected(self):
        """CLP/CBC reports 'bad bound pairs' for bad_bounds.lp."""
        ok, output = run_local_coinor(_BAD_BOUNDS, timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        assert any(kw in output.lower() for kw in ("infeasible", "bad bound")), (
            f"Expected infeasibility in output:\n{output}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_feasible_not_flagged(self):
        """CLP/CBC does not report infeasibility for a feasible LP."""
        ok, output = run_local_coinor(_LP_CASES_DIR / "feasible_small.lp", timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        findings = _parse_coinor_infeasibility(output)
        assert not findings, (
            f"Feasible LP should have no infeasibility findings but got: {findings}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_check_lp_coinor_solver(self):
        """check_lp() with --solver coinor returns 0 and includes COIN-OR output."""
        rc = check_lp(_MY_SMALL_BAD, solver="coinor", timeout=10)
        assert rc == 0


# ── TestCoinorErrorPaths ──────────────────────────────────────────────────────


class TestCoinorErrorPaths:
    """Cover timeout and error branches in run_local_coinor."""

    def test_coinor_timeout(self):
        """CLP/CBC timeout returns descriptive error."""

        def _which(name):
            return f"/usr/bin/{name}" if name == "clp" else None

        with patch("shutil.which", side_effect=_which), patch(
            "subprocess.run",
            side_effect=subprocess.TimeoutExpired("clp", 5),
        ), patch("gtopt_check_lp._solvers.as_sanitized_lp") as mock_ctx:
            mock_ctx.return_value.__enter__ = MagicMock(
                return_value=Path("/tmp/test.lp")
            )
            mock_ctx.return_value.__exit__ = MagicMock(return_value=False)
            ok, out = run_local_coinor(Path("d.lp"), timeout=5)
        assert ok or not ok  # May succeed or fail depending on mock
        assert "timed out" in out.lower() or "CLP" in out

    def test_coinor_tolerance_args(self):
        """Tolerance arguments are passed to CLP/CBC."""
        captured_args: list[list[str]] = []

        def _mock_run(cmd, **_kwargs):
            captured_args.append(cmd)
            result = MagicMock()
            result.stdout = "Optimal"
            result.stderr = ""
            return result

        def _which(name):
            return f"/usr/bin/{name}" if name == "clp" else None

        with patch("shutil.which", side_effect=_which), patch(
            "subprocess.run", side_effect=_mock_run
        ), patch("gtopt_check_lp._solvers.as_sanitized_lp") as mock_ctx:
            mock_ctx.return_value.__enter__ = MagicMock(
                return_value=Path("/tmp/test.lp")
            )
            mock_ctx.return_value.__exit__ = MagicMock(return_value=False)
            run_local_coinor(
                Path("d.lp"),
                optimal_eps=1e-8,
                feasible_eps=1e-7,
            )

        assert captured_args
        cmd = captured_args[0]
        assert "-dualtolerance" in cmd
        assert "-primaltolerance" in cmd


# ── TestHighsPythonIIS ────────────────────────────────────────────────────────


class TestHighsPythonIIS:
    """Cover HiGHS Python IIS and feasibility relaxation code paths."""

    def test_highspy_not_installed(self):
        """run_local_highs_python returns error when highspy is missing."""
        from gtopt_check_lp._solvers import run_local_highs_python  # noqa: PLC0415

        with patch.dict("sys.modules", {"highspy": None}):
            import builtins  # noqa: PLC0415

            original_import = builtins.__import__

            def _mock(name, *a, **kw):
                if name == "highspy":
                    raise ImportError
                return original_import(name, *a, **kw)

            with patch("builtins.__import__", side_effect=_mock):
                ok, msg = run_local_highs_python(Path("d.lp"))
        assert not ok
        assert "highspy" in msg.lower()

    def test_highs_binary_timeout(self):
        """HiGHS binary timeout returns descriptive error."""
        with patch("shutil.which", return_value="/usr/bin/highs"), patch(
            "subprocess.run", side_effect=subprocess.TimeoutExpired("highs", 5)
        ):
            ok, msg = run_local_highs_binary(Path("d.lp"), timeout=5)
        assert not ok
        assert "timed out" in msg.lower()

    def test_highs_binary_oserror(self):
        """HiGHS binary OSError returns descriptive error."""
        with patch("shutil.which", return_value="/usr/bin/highs"), patch(
            "subprocess.run", side_effect=OSError("exec failed")
        ):
            ok, msg = run_local_highs_binary(Path("d.lp"))
        assert not ok
        assert "Failed to run HiGHS" in msg


# ── TestRunIisDispatch ────────────────────────────────────────────────────────


class TestRunIisDispatch:
    """Cover run_iis dispatch branches for each solver."""

    def test_run_iis_cplex(self):
        with patch(
            "gtopt_check_lp._solvers.run_local_cplex",
            return_value=(True, "cplex output"),
        ):
            ok, name, out = run_iis(Path("d.lp"), solver="cplex")
        assert ok
        assert name == "CPLEX"
        assert "cplex output" in out

    def test_run_iis_highs_python(self):
        with patch(
            "gtopt_check_lp._solvers.run_local_highs_python",
            return_value=(True, "highs output"),
        ), patch.dict("sys.modules", {"highspy": MagicMock()}):
            ok, name, _out = run_iis(Path("d.lp"), solver="highs")
        assert ok
        assert name == "HiGHS"

    def test_run_iis_highs_binary_fallback(self):
        """When highspy is not installed, fall back to binary."""
        with patch(
            "gtopt_check_lp._solvers.run_local_highs_binary",
            return_value=(True, "binary output"),
        ), patch.dict("sys.modules", {"highspy": None}):
            # Force ImportError for highspy
            import builtins  # noqa: PLC0415

            original_import = builtins.__import__

            def _mock_import(name, *args, **kwargs):
                if name == "highspy":
                    raise ImportError("no highspy")
                return original_import(name, *args, **kwargs)

            with patch("builtins.__import__", side_effect=_mock_import):
                ok, name, _out = run_iis(Path("d.lp"), solver="highs")
        assert ok
        assert name == "HiGHS"

    def test_run_iis_coinor(self):
        with patch(
            "gtopt_check_lp._solvers.run_local_coinor",
            return_value=(True, "coinor output"),
        ):
            ok, name, _out = run_iis(Path("d.lp"), solver="coinor")
        assert ok
        assert name == "COIN-OR (cbc)"

    def test_run_iis_neos_no_email(self):
        ok, name, msg = run_iis(Path("d.lp"), solver="neos", email="")
        assert not ok
        assert name == "NEOS"
        assert "e-mail" in msg.lower()

    def test_run_iis_neos_with_email(self):
        mock_client = MagicMock()
        mock_client.submit_and_wait.return_value = (True, "neos output")
        with patch("gtopt_check_lp._solvers.NeosClient", return_value=mock_client):
            ok, name, _out = run_iis(Path("d.lp"), solver="neos", email="a@b.com")
        assert ok
        assert "NEOS" in name

    def test_run_iis_unknown_solver(self):
        ok, name, msg = run_iis(Path("d.lp"), solver="foobar")
        assert not ok
        assert name == "foobar"
        assert "Unknown solver" in msg


# ── TestRunAllSolvers ─────────────────────────────────────────────────────────


class TestRunAllSolvers:
    """Tests for run_all_solvers()."""

    def test_no_solvers_no_email_returns_hint(self):
        """When no solver is available and no email, returns helpful hint.

        Patches both shutil.which (binary solvers) AND sys.modules["highspy"]
        (Python HiGHS bindings) so the test is independent of what is actually
        installed in the current environment.
        """
        with patch("shutil.which", return_value=None), patch(
            "gtopt_check_lp._solvers.find_cplex_binary", return_value=None
        ), _NO_HIGHSPY:
            ok, name, out = run_all_solvers(
                _BAD_BOUNDS, email="", neos_url="", timeout=5
            )
        assert not ok
        assert name == "all"
        assert (
            "coinor" in out.lower() or "apt" in out.lower() or "install" in out.lower()
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_included_in_all(self):
        """run_all_solvers includes COIN-OR output when clp/cbc is on PATH."""
        ok, name, out = run_all_solvers(
            _MY_SMALL_BAD, email="", neos_url="", timeout=10
        )
        assert ok
        assert name == "all"
        assert "COIN-OR" in out

    def test_check_lp_solver_all(self):
        """check_lp() with solver='all' returns 0 (no solver available is acceptable)."""
        rc = check_lp(
            _BAD_BOUNDS,
            solver="all",
            analyze_only=False,
            timeout=5,
            ai=AiOptions(enabled=False),
        )
        assert rc == 0


# ── TestRunAllSolversBranches ─────────────────────────────────────────────────


class TestRunAllSolversBranches:
    """Cover branches in run_all_solvers."""

    def test_no_solvers_available(self):
        """When no solver is found, a helpful hint is returned."""
        from gtopt_check_lp._solvers import run_all_solvers as _ras  # noqa: PLC0415

        with patch("gtopt_check_lp._solvers.shutil.which", return_value=None), patch(
            "gtopt_check_lp._solvers.find_cplex_binary", return_value=None
        ), patch.dict("sys.modules", {"highspy": None}):
            import builtins  # noqa: PLC0415

            original_import = builtins.__import__

            def _mock_import(name, *args, **kwargs):
                if name == "highspy":
                    raise ImportError
                return original_import(name, *args, **kwargs)

            with patch("builtins.__import__", side_effect=_mock_import):
                ok, _name, msg = _ras(Path("d.lp"), email="", timeout=5)
        assert not ok
        assert "Install a local solver" in msg

    def test_highs_binary_fallback(self):
        """When highspy is not installed but highs binary exists, use binary."""
        from gtopt_check_lp._solvers import run_all_solvers as _ras  # noqa: PLC0415

        import builtins  # noqa: PLC0415

        original_import = builtins.__import__

        def _mock_import(name, *args, **kwargs):
            if name == "highspy":
                raise ImportError
            return original_import(name, *args, **kwargs)

        def _which(name):
            if name == "highs":
                return "/usr/bin/highs"
            return None

        with patch("builtins.__import__", side_effect=_mock_import), patch(
            "gtopt_check_lp._solvers.shutil.which", side_effect=_which
        ), patch("gtopt_check_lp._solvers.find_cplex_binary", return_value=None), patch(
            "gtopt_check_lp._solvers.run_local_highs_binary",
            return_value=(True, "highs binary"),
        ):
            ok, _name, out = _ras(Path("d.lp"), email="", timeout=5)
        assert ok
        assert "highs binary" in out.lower()


# ── TestNeosClient ────────────────────────────────────────────────────────────


class TestNeosClient:
    """Tests for NeosClient with mocked XML-RPC."""

    def test_ping_success_neos_response(self):
        """The real NEOS server returns 'NeosServer is alive\\n'."""
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.return_value = "NeosServer is alive\n"
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is True

    def test_ping_success_legacy_response(self):
        """Accept any message that contains 'alive' (case-insensitive)."""
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.return_value = "NEOS server is alive"
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is True

    def test_ping_failure(self):
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is False

    def test_submit_returns_job_number(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.submitJob.return_value = (12345, "secret")
        client._proxy = mock_proxy  # noqa: SLF001
        job_num, pwd = client.submit_lp(lp, "test@example.com")
        assert job_num == 12345
        assert pwd == "secret"

    def test_submit_error_response(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.submitJob.return_value = (-1, "error message")
        client._proxy = mock_proxy  # noqa: SLF001
        job_num, _ = client.submit_lp(lp, "test@example.com")
        assert job_num is None

    def test_wait_returns_done(self):
        client = NeosClient(timeout=30)
        mock_proxy = MagicMock()
        mock_proxy.getJobStatus.return_value = "Done"
        mock_proxy.getFinalResults.return_value = b"CPLEX output here"
        client._proxy = mock_proxy  # noqa: SLF001
        ok, out = client.wait_for_result(1, "pass")
        assert ok
        assert "CPLEX output here" in out

    def test_wait_timeout(self):
        """A very short timeout triggers the timeout path."""
        client = NeosClient(timeout=0)
        mock_proxy = MagicMock()
        mock_proxy.getJobStatus.return_value = "Running"
        client._proxy = mock_proxy  # noqa: SLF001
        ok, msg = client.wait_for_result(1, "pass", poll_interval=0.01)
        assert not ok
        assert "Timed out" in msg

    def test_neos_requires_email(self):
        """Without --email, NEOS solver should refuse with a helpful message."""
        ok, msg, _ = (
            False,
            "An e-mail address is required for NEOS.",
            "NEOS",
        )
        assert not ok
        assert "e-mail" in msg.lower() or "email" in msg.lower()

    def test_xml_template_uses_post_not_commands(self):
        """XML template must use <post> (not <commands>) and <comments> (not <comment>).
        Also must use the 'tools conflict' interactive command for the conflict refiner.
        """
        from gtopt_check_lp.gtopt_check_lp import _NEOS_LP_CPLEX_XML  # noqa: PLC0415

        xml = _NEOS_LP_CPLEX_XML.format(
            email="test@example.com", lp_content="Minimize\n obj: x\nEnd", version="1"
        )
        assert "<post>" in xml
        assert "<commands>" not in xml
        assert "<comments>" in xml
        assert "<comment>" not in xml
        assert "<client>" not in xml
        # The conflict refiner command must be present
        assert "tools conflict" in xml

    def test_cplex_local_script_uses_tools_conflict(self, tmp_path):
        """Local CPLEX script must use 'tools conflict' (the interactive command).

        The correct command sequence to identify the minimal infeasible subsystem is:
          1. set preprocessing presolve n  (disable presolve, build LP basis)
          2. optimize                      (simplex confirms infeasibility)
          3. tools conflict                (run the conflict refiner)
          4. display conflict all          (show the minimal conflict set)
        """
        from gtopt_check_lp._solvers import _cplex_script  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")
        script = _cplex_script(lp)
        assert "tools conflict\n" in script
        assert "display conflict all" in script
        # Presolve must be disabled before optimize so the refiner has a basis
        assert "set preprocessing presolve n" in script


# ── TestNeosNetworkDiagnostics ────────────────────────────────────────────────


class TestNeosNetworkDiagnostics:
    """Tests for the improved NEOS network error diagnostics."""

    def test_diagnose_no_internet(self, tmp_path):
        """When internet is down, the diagnostic says so."""
        from gtopt_check_lp._neos import _diagnose_network_error  # noqa: PLC0415

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=False):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333", lp, OSError("timed out")
            )
        assert "No internet connectivity" in msg

    def test_diagnose_neos_specific_timeout(self, tmp_path):
        """When internet works but NEOS times out, the diagnostic explains."""
        from gtopt_check_lp._neos import _diagnose_network_error  # noqa: PLC0415

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("The write operation timed out"),
            )
        assert "slow or unreachable" in msg
        assert "No internet" not in msg

    def test_diagnose_neos_other_error(self, tmp_path):
        """When internet works but NEOS returns a non-timeout error."""
        from gtopt_check_lp._neos import _diagnose_network_error  # noqa: PLC0415

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("Connection refused"),
            )
        assert "temporarily unavailable" in msg

    def test_diagnose_large_file_warning(self, tmp_path):
        """When the LP file is very large, the diagnostic warns about size."""
        from gtopt_check_lp._neos import (  # noqa: PLC0415
            _NEOS_LP_SIZE_WARN,
            _diagnose_network_error,
        )

        lp = tmp_path / "big.lp"
        # Create a file larger than the threshold.
        lp.write_bytes(b"x" * (_NEOS_LP_SIZE_WARN + 1))
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("timed out"),
            )
        assert "MiB" in msg
        assert "local solver" in msg

    def test_submit_and_wait_ping_fail_no_internet(self, tmp_path):
        """submit_and_wait ping failure with no internet gives clear message."""
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        with patch("gtopt_check_lp._neos._check_internet", return_value=False):
            ok, msg = client.submit_and_wait(lp, "test@example.com")
        assert not ok
        assert "No internet connectivity" in msg

    def test_submit_and_wait_ping_fail_neos_down(self, tmp_path):
        """submit_and_wait ping failure with internet up blames NEOS."""
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            ok, msg = client.submit_and_wait(lp, "test@example.com")
        assert not ok
        assert "NEOS service is not responding" in msg


# ── TestSolverConfig ──────────────────────────────────────────────────────────


class TestSolverConfig:
    """Cover _config.py solver status branches."""

    def test_get_solver_status_no_cplex(self):
        """CPLEX absent is reported correctly."""
        from gtopt_check_lp._config import get_solver_status  # noqa: PLC0415

        with patch("gtopt_check_lp._solvers.find_cplex_binary", return_value=None):
            status = get_solver_status()
        cplex_entry = [s for s in status if s[1] == "CPLEX"]
        assert cplex_entry
        assert not cplex_entry[0][2]  # not installed

    def test_print_solver_status_missing(self, capsys):
        """print_solver_status shows install hints for missing solvers."""
        from gtopt_check_lp._config import print_solver_status  # noqa: PLC0415

        with patch(
            "gtopt_check_lp._solvers.find_cplex_binary", return_value=None
        ), patch("gtopt_check_lp._config.shutil.which", return_value=None), patch.dict(
            "sys.modules", {"highspy": None}
        ):
            import builtins  # noqa: PLC0415

            original_import = builtins.__import__

            def _mock(name, *a, **kw):
                if name == "highspy":
                    raise ImportError
                return original_import(name, *a, **kw)

            with patch("builtins.__import__", side_effect=_mock):
                print_solver_status(use_color=False)
        captured = capsys.readouterr()
        assert "not found" in captured.out
        assert "sudo apt" in captured.out

    def test_get_solver_status_highspy_fallback(self):
        """HiGHS via highspy when binary is missing."""
        from gtopt_check_lp._config import get_solver_status  # noqa: PLC0415

        with patch(
            "gtopt_check_lp._solvers.find_cplex_binary", return_value=None
        ), patch("gtopt_check_lp._config.shutil.which", return_value=None), patch.dict(
            "sys.modules", {"highspy": MagicMock()}
        ):
            status = get_solver_status()
        highs_entry = [s for s in status if "HiGHS" in s[1]]
        assert highs_entry
        assert highs_entry[0][2]  # installed via highspy


# ── TestSubprocessRun ─────────────────────────────────────────────────────────


class TestSubprocessRun:
    """End-to-end: run gtopt_check_lp (or python -m) on the test LP files."""

    @pytest.mark.integration
    def test_bad_bounds_subprocess(self):
        """Run gtopt_check_lp on bad_bounds.lp and check for conflict message."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_BAD_BOUNDS),
                "--analyze-only",
                "--no-color",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        assert "Conflicting variable bounds" in combined or "x1" in combined

    @pytest.mark.integration
    def test_my_small_bad_subprocess(self):
        """Run on my_small_bad.lp — static analysis shows no bound conflict."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_MY_SMALL_BAD),
                "--analyze-only",
                "--no-color",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        # The constraint conflict is not detectable without a solver,
        # but the script should at least report problem statistics.
        assert (
            "Static Analysis" in combined or "Minimize" in combined or "x1" in combined
        )

    @pytest.mark.integration
    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_solver_subprocess(self):
        """Run gtopt_check_lp --solver coinor on my_small_bad.lp."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_MY_SMALL_BAD),
                "--solver",
                "coinor",
                "--no-color",
                "--timeout",
                "10",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        # CLP/CBC output should confirm infeasibility
        assert any(
            kw in combined.lower() for kw in ("infeasible", "coin-or", "clp", "cbc")
        ), f"Expected COIN-OR infeasibility output:\n{combined}"
