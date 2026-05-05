# SPDX-License-Identifier: BSD-3-Clause
"""Smoke-tests for every CLI script's ``--help`` output.

These tests exist to catch a specific class of bug that's silent until
a user actually types ``-h``:

* argparse's ``_expand_help`` runs ``help_string % params`` on each
  option's help text, where ``params`` carries the standard
  ``%(default)s``, ``%(prog)s`` etc. tokens.  A **bare** ``%`` in the
  help text — e.g. ``"converged at gap=25 % via the CI test"`` — is
  interpreted as a format placeholder, and argparse raises
  ``ValueError: unsupported format character '<X>'`` only at
  ``--help`` invocation time.  None of the unit tests catch this
  because they never call ``parser.format_help()``.

* Module-import errors at the entry-point: a missing dependency, a
  circular import, or a top-level ``raise`` in a renamed helper
  surface here as ``ImportError`` / ``ModuleNotFoundError`` and a red
  test, instead of a confusing traceback the first time a user runs
  ``<script> --help``.

The test mirrors ``[project.scripts]`` in ``scripts/pyproject.toml`` —
keep the two in sync when adding a new CLI.  Each parametrisation
imports the entry-point function and invokes it with
``sys.argv = [<script>, "--help"]``; argparse short-circuits with
``SystemExit(0)`` after printing the help text.
"""

from __future__ import annotations

import importlib
import io
from contextlib import redirect_stderr, redirect_stdout

import pytest

# (script_name, dotted_module, function_name).  Mirrors
# ``[project.scripts]`` in ``scripts/pyproject.toml``.  When adding a
# new CLI to that table, append a row here too.
ENTRY_POINTS: list[tuple[str, str, str]] = [
    ("plp2gtopt", "plp2gtopt.main", "main"),
    ("pp2gtopt", "pp2gtopt.main", "main"),
    ("ts2gtopt", "ts2gtopt.main", "main"),
    ("sddp2gtopt", "sddp2gtopt.main", "main"),
    ("gtopt2pp", "gtopt2pp.main", "main"),
    ("gtopt_compare", "gtopt_compare.main", "main"),
    ("gtopt_check_lp", "gtopt_check_lp.gtopt_check_lp", "main"),
    ("gtopt_check_json", "gtopt_check_json.gtopt_check_json", "main"),
    ("gtopt_check_output", "gtopt_check_output.main", "main"),
    ("gtopt_check_solvers", "gtopt_check_solvers.gtopt_check_solvers", "main"),
    ("gtopt_check_pampl", "gtopt_check_pampl.gtopt_check_pampl", "main"),
    ("gtopt_check_fingerprint", "gtopt_check_fingerprint._cli", "main"),
    ("gtopt_compress_lp", "gtopt_compress_lp.gtopt_compress_lp", "main"),
    ("gtopt_field_extractor", "gtopt_field_extractor", "main"),
    ("gtopt_diagram", "gtopt_diagram", "main"),
    ("gtopt_expand", "gtopt_expand.cli", "main"),
    ("gtopt_monitor", "gtopt_monitor", "main"),
    ("cvs2parquet", "cvs2parquet.cvs2parquet", "main"),
    ("igtopt", "igtopt.igtopt", "main"),
    ("run_gtopt", "run_gtopt.main", "main"),
    ("plp_compress_case", "plp_compress_case.main", "main"),
    ("cen_demanda", "cen_demanda.main", "main"),
    ("gtopt_results_summary", "gtopt_results_summary.main", "main"),
    ("gtopt_timeseries_export", "gtopt_timeseries_export.main", "main"),
]


@pytest.mark.parametrize(
    "script_name,module_path,func_name",
    ENTRY_POINTS,
    ids=[name for name, _, _ in ENTRY_POINTS],
)
def test_help_renders_without_error(
    script_name: str, module_path: str, func_name: str, monkeypatch
) -> None:
    """``<script> --help`` must complete without raising.

    Specifically catches:
    * ``ValueError`` from argparse percent-formatting on bare ``%``
      characters in help strings.
    * ``ModuleNotFoundError`` / import-time errors at the entry-point.
    * Missing/renamed entry-point functions (would raise
      ``AttributeError``).

    Asserts:
    * Exit is via ``SystemExit(0)`` (argparse's --help convention).
    * Stdout contains a recognisable usage line so we know the help
      text actually rendered (and didn't, say, crash inside a
      formatter mid-write).
    """
    try:
        mod = importlib.import_module(module_path)
    except ImportError as exc:
        pytest.fail(
            f"{script_name}: cannot import entry-point module '{module_path}': {exc}"
        )

    main_fn = getattr(mod, func_name, None)
    if main_fn is None:
        pytest.fail(
            f"{script_name}: entry-point function '{func_name}' "
            f"not found in module '{module_path}'"
        )

    monkeypatch.setattr("sys.argv", [script_name, "--help"])

    out_buf = io.StringIO()
    err_buf = io.StringIO()
    try:
        with redirect_stdout(out_buf), redirect_stderr(err_buf):
            with pytest.raises(SystemExit) as exc_info:
                main_fn()
    except ValueError as exc:
        pytest.fail(
            f"{script_name} --help raised ValueError "
            f"(likely a bare '%' in help text): {exc}"
        )

    assert exc_info.value.code in (0, None), (
        f"{script_name} --help exited with code "
        f"{exc_info.value.code}; stderr=\n{err_buf.getvalue()}"
    )
    stdout_text = out_buf.getvalue()
    assert "usage:" in stdout_text.lower(), (
        f"{script_name} --help did not render a usage line; "
        f"stdout=\n{stdout_text[:500]}"
    )
