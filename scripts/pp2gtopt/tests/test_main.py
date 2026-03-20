# SPDX-License-Identifier: BSD-3-Clause
"""Tests for pp2gtopt.main -- CLI argument handling, dispatch, and error paths."""

import logging
import sys
from unittest.mock import MagicMock, patch

import pytest

from pp2gtopt.main import (
    _NETWORKS,
    _list_networks_and_exit,
    _log_element_counts,
    main,
    run_post_check,
)


# ---------------------------------------------------------------------------
# Shared fixtures / helpers
# ---------------------------------------------------------------------------

_MINIMAL_PLANNING: dict = {
    "system": {
        "name": "test_net",
        "bus_array": [{"uid": 1}],
        "generator_array": [{"uid": 1}],
        "demand_array": [],
        "line_array": [{"uid": 1}, {"uid": 2}],
    },
    "simulation": {
        "block_array": [{"uid": 1}],
        "stage_array": [{"uid": 1}],
        "scenario_array": [{"uid": 1}],
    },
}


# ---------------------------------------------------------------------------
# _list_networks_and_exit  (lines 74-80)
# ---------------------------------------------------------------------------


class TestListNetworksAndExit:
    def test_prints_all_networks_and_exits_zero(self, capsys):
        with pytest.raises(SystemExit) as exc:
            _list_networks_and_exit()
        assert exc.value.code == 0
        out = capsys.readouterr().out
        for name in _NETWORKS:
            assert name in out

    def test_marks_default_network(self, capsys):
        with pytest.raises(SystemExit):
            _list_networks_and_exit()
        assert "(default)" in capsys.readouterr().out


# ---------------------------------------------------------------------------
# _log_element_counts -- ImportError fallback path  (lines 89-101)
# ---------------------------------------------------------------------------


class TestLogElementCounts:
    def test_fallback_logger_when_info_module_missing(self, caplog):
        """Forcing ImportError on gtopt_check_json._info triggers logger fallback."""
        # Setting a sys.modules entry to None makes `from X import Y` raise ImportError.
        with patch.dict("sys.modules", {"gtopt_check_json._info": None}):
            with caplog.at_level(logging.INFO, logger="pp2gtopt.main"):
                _log_element_counts(_MINIMAL_PLANNING)
        assert "Buses" in caplog.text
        assert "Generators" in caplog.text
        assert "test_net" in caplog.text


# ---------------------------------------------------------------------------
# run_post_check  (lines 128-130, 138-150)
# ---------------------------------------------------------------------------


class TestRunPostCheck:
    def test_skips_gracefully_when_check_json_unavailable(self):
        """run_post_check returns silently when gtopt_check_json is absent."""
        blocked = {
            "gtopt_check_json._checks": None,
            "gtopt_check_json._terminal": None,
            "gtopt_check_json._info": None,
        }
        with patch.dict("sys.modules", blocked):
            # Should not raise
            run_post_check(_MINIMAL_PLANNING)

    def test_prints_ok_when_no_findings(self):
        """run_post_check calls print_status when checks pass."""
        mock_checks = MagicMock()
        mock_checks.run_all_checks.return_value = []
        mock_terminal = MagicMock()

        modules = {
            "gtopt_check_json._checks": mock_checks,
            "gtopt_check_json._terminal": mock_terminal,
            "gtopt_check_json._info": MagicMock(),
        }
        with patch.dict("sys.modules", modules):
            run_post_check(_MINIMAL_PLANNING)
        mock_terminal.print_status.assert_called_once()

    def test_tallies_finding_severities(self):
        """run_post_check counts critical/warning/note findings (lines 138-150)."""
        from enum import Enum

        class Severity(Enum):
            CRITICAL = "critical"
            WARNING = "warning"
            NOTE = "note"

        findings = []
        for sev, cid in [
            (Severity.CRITICAL, "C1"),
            (Severity.WARNING, "W1"),
            (Severity.NOTE, "N1"),
        ]:
            f = MagicMock()
            f.severity = sev
            f.check_id = cid
            f.message = f"{sev.value} msg"
            findings.append(f)

        mock_checks = MagicMock()
        mock_checks.run_all_checks.return_value = findings
        mock_checks.Severity = Severity
        mock_terminal = MagicMock()

        modules = {
            "gtopt_check_json._checks": mock_checks,
            "gtopt_check_json._terminal": mock_terminal,
            "gtopt_check_json._info": MagicMock(),
        }
        with patch.dict("sys.modules", modules):
            run_post_check(_MINIMAL_PLANNING)
        mock_terminal.print_summary.assert_called_once_with(1, 1, 1)
        assert mock_terminal.print_finding.call_count == 3


# ---------------------------------------------------------------------------
# main() -- CLI dispatch  (lines 128-130, 226-227)
# ---------------------------------------------------------------------------


class TestMainDispatch:
    def test_no_check_flag_skips_post_check(self, tmp_path):
        out = tmp_path / "out.json"
        with patch.object(sys, "argv", ["pp2gtopt", "-o", str(out), "--no-check"]):
            with patch("pp2gtopt.main.run_post_check") as mock_check:
                main()
        mock_check.assert_not_called()
        assert out.exists()

    def test_default_check_calls_run_post_check(self, tmp_path):
        out = tmp_path / "out.json"
        with patch.object(sys, "argv", ["pp2gtopt", "-o", str(out)]):
            with patch("pp2gtopt.main.run_post_check") as mock_check:
                main()
        mock_check.assert_called_once()

    def test_file_source_uses_stem_as_name(self, tmp_path):
        pytest.importorskip("pandapower", reason="pandapower not installed")
        import pandapower as pp  # pylint: disable=import-outside-toplevel
        import pandapower.networks as pn  # pylint: disable=import-outside-toplevel

        net = pn.case9()
        src = tmp_path / "mycase.json"
        pp.to_json(net, str(src))

        out = tmp_path / "mycase_gtopt.json"
        with patch.object(
            sys, "argv", ["pp2gtopt", "-f", str(src), "-o", str(out), "--no-check"]
        ):
            main()
        assert out.exists()

    def test_clean_formatter_import_error_handled(self, tmp_path):
        """main() handles missing CleanFormatter gracefully (lines 226-227)."""
        out = tmp_path / "out.json"
        with patch.dict("sys.modules", {"gtopt_check_json._terminal": None}):
            with patch.object(sys, "argv", ["pp2gtopt", "-o", str(out), "--no-check"]):
                main()
        assert out.exists()
