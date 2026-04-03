# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_pampl."""

from pathlib import Path

import pytest

from gtopt_check_pampl._checks import (
    Severity,
    check_constraint_names,
    check_domain_clauses,
    check_duplicate_names,
    check_element_references,
    check_inactive_constraints,
    check_operator_usage,
    check_param_declarations,
    check_semicolons,
    check_syntax,
    check_template_variables,
    compute_stats,
    run_all_checks,
)
from gtopt_check_pampl._config import (
    CHECK_DEFAULTS,
    default_config_path,
    is_check_enabled,
    load_config,
    save_config,
)
from gtopt_check_pampl.gtopt_check_pampl import check_pampl, main


# ── Helpers ──────────────────────────────────────────────────────────────────

EXAMPLES_DIR = Path(__file__).resolve().parent.parent / "examples"
VALID_DIR = EXAMPLES_DIR / "valid"
ERRORS_DIR = EXAMPLES_DIR / "errors"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# ── check_syntax ────────────────────────────────────────────────────────────


class TestCheckSyntax:
    def test_valid_file_no_findings(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        findings = check_syntax(source)
        assert not findings

    def test_unbalanced_parens(self):
        source = "constraint x: generator('G1'.generation <= 100;"
        findings = check_syntax(source)
        # Unmatched ')' never occurs here, but bracket count is off
        assert any(f.check_id == "syntax" for f in findings)

    def test_unbalanced_brackets(self):
        source = "param bad[month] = [1, 2, 3;"
        findings = check_syntax(source)
        assert any(
            f.severity == Severity.CRITICAL and "bracket" in f.message.lower()
            for f in findings
        )

    def test_unbalanced_quote(self):
        source = 'constraint x: generator("G1).generation <= 100;'
        findings = check_syntax(source)
        assert any(
            f.severity == Severity.WARNING and "quote" in f.message.lower()
            for f in findings
        )


# ── check_semicolons ───────────────────────────────────────────────────────


class TestCheckSemicolons:
    def test_valid_file_no_findings(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        findings = check_semicolons(source)
        assert not findings

    def test_missing_semicolons(self):
        source = _read(ERRORS_DIR / "missing_semicolons.pampl")
        findings = check_semicolons(source)
        assert len(findings) >= 1
        assert all(f.severity == Severity.CRITICAL for f in findings)

    def test_bare_expression_missing_semi(self):
        source = "generator('G1').generation <= 100"
        findings = check_semicolons(source)
        assert len(findings) >= 1


# ── check_constraint_names ─────────────────────────────────────────────────


class TestCheckConstraintNames:
    def test_valid_names(self):
        source = _read(VALID_DIR / "combined_generation.pampl")
        findings = check_constraint_names(source)
        assert not findings

    def test_missing_name(self):
        source = _read(ERRORS_DIR / "missing_name.pampl")
        findings = check_constraint_names(source)
        assert len(findings) >= 1
        assert any(f.severity == Severity.CRITICAL for f in findings)

    def test_inactive_without_constraint(self):
        source = _read(ERRORS_DIR / "bad_inactive.pampl")
        findings = check_constraint_names(source)
        assert any(
            f.severity == Severity.CRITICAL and "inactive" in f.message.lower()
            for f in findings
        )


# ── check_param_declarations ───────────────────────────────────────────────


class TestCheckParamDeclarations:
    def test_valid_params(self):
        source = _read(VALID_DIR / "params_and_monthly.pampl")
        findings = check_param_declarations(source)
        assert not findings

    def test_param_missing_equals(self):
        source = "param orphan;"
        findings = check_param_declarations(source)
        assert len(findings) >= 1
        assert any(f.severity == Severity.CRITICAL for f in findings)

    def test_wrong_index_dimension(self):
        source = "param x[year] = [1,2,3,4,5,6,7,8,9,10,11,12];"
        findings = check_param_declarations(source)
        assert any(
            f.severity == Severity.WARNING and "year" in f.message
            for f in findings
        )


# ── check_element_references ───────────────────────────────────────────────


class TestCheckElementReferences:
    def test_valid_references(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        findings = check_element_references(source)
        assert not findings

    def test_unknown_element_type(self):
        source = _read(ERRORS_DIR / "unknown_elements.pampl")
        findings = check_element_references(source)
        assert any(
            f.severity == Severity.WARNING and "motor" in f.message
            for f in findings
        )

    def test_unknown_variable(self):
        source = _read(ERRORS_DIR / "unknown_elements.pampl")
        findings = check_element_references(source)
        assert any(
            f.severity == Severity.NOTE and "rpm" in f.message
            for f in findings
        )


# ── check_operator_usage ──────────────────────────────────────────────────


class TestCheckOperatorUsage:
    def test_valid_operators(self):
        source = _read(VALID_DIR / "domain_constraints.pampl")
        findings = check_operator_usage(source)
        assert not findings

    def test_double_equals(self):
        source = _read(ERRORS_DIR / "bad_operators.pampl")
        findings = check_operator_usage(source)
        assert any("==" in f.message for f in findings)

    def test_not_equals(self):
        source = _read(ERRORS_DIR / "bad_operators.pampl")
        findings = check_operator_usage(source)
        assert any("!=" in f.message or "not supported" in f.message for f in findings)


# ── check_domain_clauses ──────────────────────────────────────────────────


class TestCheckDomainClauses:
    def test_valid_for_clause(self):
        source = _read(VALID_DIR / "domain_constraints.pampl")
        findings = check_domain_clauses(source)
        assert not findings

    def test_for_without_in(self):
        source = "constraint x: generator('G1').generation <= 100, for(block);"
        findings = check_domain_clauses(source)
        assert any("in" in f.message for f in findings)


# ── check_template_variables ──────────────────────────────────────────────


class TestCheckTemplateVariables:
    def test_valid_template(self):
        source = _read(VALID_DIR / "renewable_curtailment.tampl")
        findings = check_template_variables(source)
        assert not findings

    def test_bad_template(self):
        source = _read(ERRORS_DIR / "bad_template.tampl")
        findings = check_template_variables(source)
        assert any(f.severity == Severity.CRITICAL for f in findings)

    def test_unmatched_for_endfor(self):
        source = "{% for x in items %}\nsome content\n"
        findings = check_template_variables(source)
        assert any("for" in f.message and "endfor" in f.message for f in findings)


# ── check_duplicate_names ─────────────────────────────────────────────────


class TestCheckDuplicateNames:
    def test_no_duplicates(self):
        source = _read(VALID_DIR / "domain_constraints.pampl")
        findings = check_duplicate_names(source)
        assert not findings

    def test_duplicate_constraint(self):
        source = _read(ERRORS_DIR / "duplicate_names.pampl")
        findings = check_duplicate_names(source)
        assert any("gen_limit" in f.message for f in findings)

    def test_duplicate_param(self):
        source = _read(ERRORS_DIR / "duplicate_names.pampl")
        findings = check_duplicate_names(source)
        assert any("limit" in f.message for f in findings)


# ── check_inactive_constraints ─────────────────────────────────────────────


class TestCheckInactiveConstraints:
    def test_inactive_reported(self):
        source = _read(VALID_DIR / "battery_constraints.pampl")
        findings = check_inactive_constraints(source)
        assert any(
            f.severity == Severity.NOTE and "bat_reserve" in f.message
            for f in findings
        )

    def test_no_inactive(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        findings = check_inactive_constraints(source)
        assert not findings


# ── compute_stats ─────────────────────────────────────────────────────────


class TestComputeStats:
    def test_simple_file_stats(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        stats = compute_stats(source, "simple_gen_limit.pampl")
        assert stats.num_constraints >= 1
        assert stats.num_params == 0
        assert not stats.has_templates
        assert "generator" in stats.element_types_used

    def test_param_file_stats(self):
        source = _read(VALID_DIR / "params_and_monthly.pampl")
        stats = compute_stats(source, "params_and_monthly.pampl")
        assert stats.num_params >= 4
        assert stats.num_constraints >= 1

    def test_template_file_stats(self):
        source = _read(VALID_DIR / "renewable_curtailment.tampl")
        stats = compute_stats(source, "renewable_curtailment.tampl")
        assert stats.has_templates


# ── run_all_checks ────────────────────────────────────────────────────────


class TestRunAllChecks:
    def test_valid_file_clean(self):
        source = _read(VALID_DIR / "simple_gen_limit.pampl")
        findings = run_all_checks(source)
        critical = [f for f in findings if f.severity == Severity.CRITICAL]
        assert not critical

    def test_error_file_has_critical(self):
        source = _read(ERRORS_DIR / "bad_operators.pampl")
        findings = run_all_checks(source)
        critical = [f for f in findings if f.severity == Severity.CRITICAL]
        assert len(critical) >= 1

    def test_enabled_checks_filter(self):
        source = _read(ERRORS_DIR / "unknown_elements.pampl")
        # Only run element_references check
        findings = run_all_checks(source, enabled_checks={"element_references"})
        assert all(f.check_id == "element_references" for f in findings)

    def test_findings_sorted_by_severity(self):
        source = _read(ERRORS_DIR / "bad_operators.pampl")
        findings = run_all_checks(source)
        if len(findings) >= 2:
            sev_order = {Severity.CRITICAL: 0, Severity.WARNING: 1, Severity.NOTE: 2}
            for i in range(len(findings) - 1):
                assert sev_order[findings[i].severity] <= sev_order[
                    findings[i + 1].severity
                ]


# ── Config ────────────────────────────────────────────────────────────────


class TestConfig:
    def test_default_config_path(self):
        assert default_config_path().name == ".gtopt.conf"

    def test_check_defaults(self):
        assert "syntax" in CHECK_DEFAULTS
        assert "semicolons" in CHECK_DEFAULTS
        assert "template_variables" in CHECK_DEFAULTS

    def test_is_check_enabled_default(self):
        cfg: dict[str, str] = {}
        assert is_check_enabled(cfg, "syntax")

    def test_is_check_enabled_explicit(self):
        cfg = {"check_syntax": "false"}
        assert not is_check_enabled(cfg, "syntax")

    def test_load_save_config(self, tmp_path):
        config_path = tmp_path / ".gtopt.conf"
        cfg = load_config(config_path)
        cfg["check_syntax"] = "false"
        save_config(config_path, cfg)
        reloaded = load_config(config_path)
        assert reloaded["check_syntax"] == "false"


# ── CLI integration ───────────────────────────────────────────────────────


class TestCLI:
    def test_main_no_args_returns_2(self):
        assert main([]) == 2

    def test_main_missing_file(self):
        assert main(["nonexistent.pampl"]) == 2

    def test_main_valid_file(self):
        path = str(VALID_DIR / "simple_gen_limit.pampl")
        rc = main([path, "--no-color"])
        assert rc == 0

    def test_main_error_file(self):
        path = str(ERRORS_DIR / "bad_operators.pampl")
        rc = main([path, "--no-color"])
        assert rc == 1

    def test_main_info_mode(self):
        path = str(VALID_DIR / "params_and_monthly.pampl")
        rc = main(["--info", path, "--no-color"])
        assert rc == 0

    def test_main_show_config(self, tmp_path):
        config_path = tmp_path / ".gtopt.conf"
        rc = main(["--show-config", "--config", str(config_path)])
        assert rc == 0

    def test_main_version(self):
        with pytest.raises(SystemExit) as exc_info:
            main(["--version"])
        assert exc_info.value.code == 0

    def test_check_pampl_valid_dir(self):
        files = [str(f) for f in VALID_DIR.glob("*.pampl")]
        assert files  # Ensure we have example files
        rc = check_pampl(files)
        assert rc == 0

    def test_check_pampl_tampl_files(self):
        files = [str(f) for f in VALID_DIR.glob("*.tampl")]
        assert files
        rc = check_pampl(files)
        assert rc == 0

    def test_check_pampl_all_error_files(self):
        files = [str(f) for f in ERRORS_DIR.glob("*.pampl")]
        assert files
        rc = check_pampl(files)
        assert rc == 1  # Should find critical issues
