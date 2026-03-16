# SPDX-License-Identifier: BSD-3-Clause
"""Local and remote solver wrappers for LP infeasibility analysis.

CPLEX command note
------------------
The CPLEX Interactive Optimizer command ``conflict`` does **not** exist in
CPLEX v20.1+ (the interactive session reports "Command 'conflict' does not
exist").  The correct command is ``refineconflict``, followed by
``display conflict all`` to show the minimal infeasible subset.

To ensure the conflict refiner has a working LP basis, presolve must be
disabled *before* solving (``set preprocessing presolve 0``); otherwise CPLEX
terminates in presolve and ``refineconflict`` has nothing to work with.
"""

import logging
import re
import shutil
import subprocess
import tempfile
import threading
from pathlib import Path

from . import _colors as col
from ._compress import as_plain_lp, as_sanitized_lp
from ._neos import NeosClient

log = logging.getLogger(__name__)

_NEOS_DEFAULT_URL = "https://neos-server.org:3333"


# ---------------------------------------------------------------------------
# Solver detection
# ---------------------------------------------------------------------------


def detect_local_solvers() -> list[str]:
    """Return human-readable labels for every locally available solver."""
    available = []
    for binary, label in [
        ("cplex", "CPLEX"),
        ("clp", "CLP (COIN-OR)"),
        ("cbc", "CBC (COIN-OR)"),
        ("glpsol", "GLPK"),
    ]:
        if shutil.which(binary):
            available.append(label)

    # highspy Python package counts as HiGHS
    highs_bin = shutil.which("highs")
    has_highspy = False
    try:
        import highspy  # noqa: F401, PLC0415  # pylint: disable=unused-import

        has_highspy = True
    except ImportError:
        pass

    if highs_bin or has_highspy:
        available.append("HiGHS (Python)" if has_highspy else "HiGHS (binary)")

    return available


# ---------------------------------------------------------------------------
# CPLEX
# ---------------------------------------------------------------------------


def _cplex_script(
    lp_path: Path,
    algo: str = "",
    optimal_eps: float = 0.0,
    feasible_eps: float = 0.0,
    barrier_eps: float = 0.0,
) -> str:
    """Return CPLEX interactive commands to find and display the conflict.

    Parameters
    ----------
    lp_path:
        Path to the LP file.
    algo:
        LP algorithm hint: ``"barrier"``, ``"primal"``, ``"dual"``, or
        ``""`` (default → barrier for best infeasibility diagnostics).
    optimal_eps:
        Dual (optimality) tolerance; 0.0 means use CPLEX default.
    feasible_eps:
        Primal (feasibility) tolerance; 0.0 means use CPLEX default.
    barrier_eps:
        Barrier convergence tolerance; 0.0 means use CPLEX default.

    Command sequence rationale
    --------------------------
    ``conflict`` is **not** a valid CPLEX Interactive Optimizer command in
    v20.1+ — the session reports "Command 'conflict' does not exist".

    The correct approach:
    1. Disable presolve so the simplex method (not presolve) detects
       infeasibility and builds an infeasibility certificate.
    2. Optionally set the LP method (barrier, primal, or dual).
    3. Optionally set numerical tolerances.
    4. Re-solve with ``optimize``.
    5. Invoke the CPLEX conflict refiner with ``refineconflict``.
    6. Display the minimal conflict set with ``display conflict all``.
    """
    # Map algo names to CPLEX lpmethod values:
    #   0 = automatic, 1 = primal simplex, 2 = dual simplex, 4 = barrier
    effective_algo = algo.lower() if algo else "barrier"
    cplex_method_cmds = {
        "barrier": "set lpmethod 4\n",
        "primal": "set lpmethod 1\n",
        "dual": "set lpmethod 2\n",
        "default": "",
    }
    method_cmd = cplex_method_cmds.get(effective_algo, "")

    tol_cmds = ""
    if optimal_eps > 0:
        tol_cmds += f"set simplex tolerances dual {optimal_eps}\n"
    if feasible_eps > 0:
        tol_cmds += f"set simplex tolerances primal {feasible_eps}\n"
    if barrier_eps > 0:
        tol_cmds += f"set barrier convergetol {barrier_eps}\n"

    return (
        f"read {lp_path}\n"
        "set preprocessing presolve 0\n"
        f"{method_cmd}"
        f"{tol_cmds}"
        "optimize\n"
        "refineconflict\n"
        "display conflict all\n"
        "quit\n"
    )


def run_local_cplex(
    lp_path: Path,
    timeout: int = 120,
    algo: str = "",
    optimal_eps: float = 0.0,
    feasible_eps: float = 0.0,
    barrier_eps: float = 0.0,
) -> tuple[bool, str]:
    """
    Run the local ``cplex`` binary to identify the infeasible conflict.

    Accepts plain or gzip-compressed LP files.

    Parameters
    ----------
    algo:
        LP algorithm: ``"barrier"`` (default when empty), ``"primal"``,
        ``"dual"``, or ``"default"`` (CPLEX automatic).
    optimal_eps:
        Dual (optimality) tolerance; 0.0 means use CPLEX default.
    feasible_eps:
        Primal (feasibility) tolerance; 0.0 means use CPLEX default.
    barrier_eps:
        Barrier convergence tolerance; 0.0 means use CPLEX default.

    Returns ``(success, output)`` where *success* is True when CPLEX was
    found and executed without crashing.
    """
    cplex_bin = shutil.which("cplex")
    if cplex_bin is None:
        return False, "cplex binary not found on PATH."

    log.debug("Running CPLEX from: %s", cplex_bin)

    script_path = ""
    try:
        with as_plain_lp(lp_path) as plain_path:
            script = _cplex_script(
                plain_path,
                algo=algo,
                optimal_eps=optimal_eps,
                feasible_eps=feasible_eps,
                barrier_eps=barrier_eps,
            )
            with tempfile.NamedTemporaryFile(
                mode="w", suffix=".cplex_cmds", delete=False, encoding="utf-8"
            ) as tf:
                tf.write(script)
                script_path = tf.name

            result = subprocess.run(
                [cplex_bin, "-f", script_path],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
        return True, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return False, f"CPLEX timed out after {timeout}s."
    except OSError as exc:
        return False, f"Failed to run CPLEX: {exc}"
    finally:
        if script_path:
            Path(script_path).unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# HiGHS (binary)
# ---------------------------------------------------------------------------


def run_local_highs_binary(lp_path: Path, timeout: int = 120) -> tuple[bool, str]:
    """
    Run the ``highs`` binary to solve the LP and report infeasibility info.

    Accepts plain or gzip-compressed LP files.

    Returns ``(success, output)``.
    """
    highs_bin = shutil.which("highs")
    if highs_bin is None:
        return False, "highs binary not found on PATH."

    log.debug("Running HiGHS from: %s", highs_bin)
    try:
        with as_plain_lp(lp_path) as plain_path:
            result = subprocess.run(
                [
                    highs_bin,
                    "--model_file",
                    str(plain_path),
                    "--presolve",
                    "off",
                    "--solver",
                    "simplex",
                ],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
        return True, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return False, f"HiGHS timed out after {timeout}s."
    except OSError as exc:
        return False, f"Failed to run HiGHS: {exc}"


# ---------------------------------------------------------------------------
# HiGHS (Python/highspy)
# ---------------------------------------------------------------------------


def run_local_highs_python(lp_path: Path, timeout: int = 30) -> tuple[bool, str]:
    """
    Use the ``highspy`` Python package to find the IIS (if installed).

    Accepts plain or gzip-compressed LP files.  When the file is compressed
    it is first decompressed to a temporary plain ``.lp`` file.

    Runs in a background thread so that the *timeout* (in seconds) is
    respected even if highspy blocks internally.

    When the IIS is available (highspy ≥ 1.7) the output includes the names
    of the infeasible constraints and variable-bound members.  When the IIS
    is unavailable the raw model status and solution quality are reported.

    Returns ``(success, output)``.
    """
    try:
        import highspy  # noqa: PLC0415
    except ImportError:
        return False, "highspy Python package not installed."

    log.debug("Using highspy for IIS analysis (timeout=%ds)", timeout)

    result_holder: list[tuple[bool, str]] = []

    def _run(plain_lp_path: Path) -> None:  # noqa: PLR0912
        lines: list[str] = []
        try:
            h = highspy.Highs()
            h.silent()
            h.readModel(str(plain_lp_path))
            h.run()
            model_status = str(h.getModelStatus())
            lines.append(f"HiGHS model status: {model_status}")

            # Try IIS via conflict analysis (available in highspy >= 1.7)
            try:
                iis = h.getIis()
                col_idx = getattr(iis, "col_index", []) or []
                row_idx = getattr(iis, "row_index", []) or []

                if col_idx or row_idx:
                    lines.append("\n-- HiGHS IIS (Irreducible Infeasible Subsystem) --")

                    if col_idx:
                        lines.append(f"  Infeasible variable bounds ({len(col_idx)}):")
                        # Try to retrieve column names for readability
                        try:
                            num_cols = h.getNumCol()
                            col_names = [h.getColName(c) for c in range(num_cols)]
                        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                            col_names = []
                        for ci in col_idx:
                            name = col_names[ci] if ci < len(col_names) else str(ci)
                            bound_type = ""
                            if hasattr(iis, "col_bound") and ci < len(iis.col_bound):
                                bound_type = f"  [{iis.col_bound[ci]}]"
                            lines.append(f"    • {name}{bound_type}")

                    if row_idx:
                        lines.append(f"  Infeasible constraints ({len(row_idx)}):")
                        # Try to retrieve row names for readability
                        try:
                            num_rows = h.getNumRow()
                            row_names = [h.getRowName(r) for r in range(num_rows)]
                        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                            row_names = []
                        for ri in row_idx:
                            name = row_names[ri] if ri < len(row_names) else str(ri)
                            bound_type = ""
                            if hasattr(iis, "row_bound") and ri < len(iis.row_bound):
                                bound_type = f"  [{iis.row_bound[ri]}]"
                            lines.append(f"    • {name}{bound_type}")
                else:
                    lines.append(
                        "\n-- HiGHS IIS --\n"
                        "  IIS computation returned no members.\n"
                        "  The infeasibility may have been detected by presolve.\n"
                        "  Try running with the HiGHS binary (--solver highs) for\n"
                        "  more detailed output."
                    )
            except (AttributeError, Exception) as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                lines.append(f"\n  IIS not available (highspy < 1.7?): {exc}")

            result_holder.append((True, "\n".join(lines)))
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            result_holder.append((False, f"highspy error: {exc}"))

    with as_plain_lp(lp_path) as plain_path:
        worker = threading.Thread(target=_run, args=(plain_path,), daemon=True)
        worker.start()
        worker.join(timeout=timeout)
    if worker.is_alive():
        log.debug("highspy timed out after %ds", timeout)
        return False, f"highspy timed out after {timeout}s."
    if result_holder:
        return result_holder[0]
    return False, "highspy worker exited without a result."


# ---------------------------------------------------------------------------
# COIN-OR (CLP / CBC)
# ---------------------------------------------------------------------------

_COINOR_INFEAS_PATTERNS = [
    re.compile(r"Primal infeasible", re.IGNORECASE),
    re.compile(r"PrimalInfeasible", re.IGNORECASE),
    re.compile(r"Presolve determined that the problem was infeasible", re.IGNORECASE),
    re.compile(r"Analysis indicates model infeasible or unbounded", re.IGNORECASE),
    re.compile(r"bad bound pairs or bad objectives were found", re.IGNORECASE),
    re.compile(r"Linear relaxation infeasible", re.IGNORECASE),
]


def parse_coinor_infeasibility(output: str) -> list[str]:
    """
    Extract human-readable infeasibility hints from CLP/CBC output.

    Returns a list of matched diagnostic lines (empty when the LP is feasible
    or the output contains no recognisable markers).
    """
    findings: list[str] = []
    for line in output.splitlines():
        for pat in _COINOR_INFEAS_PATTERNS:
            if pat.search(line):
                findings.append(line.strip())
                break
    return findings


# Backward-compat alias used by the monolithic module and tests.
_parse_coinor_infeasibility = parse_coinor_infeasibility


def run_local_coinor(
    lp_path: Path,
    timeout: int = 10,
    algo: str = "",
    optimal_eps: float = 0.0,
    feasible_eps: float = 0.0,
    barrier_eps: float = 0.0,
) -> tuple[bool, str]:
    """
    Run CLP or CBC (COIN-OR) to analyse the LP file for infeasibility.

    Accepts plain or gzip-compressed LP files.

    Tries ``clp`` first (pure LP, lighter) and falls back to ``cbc``.

    Parameters
    ----------
    algo:
        LP algorithm: ``"barrier"`` (default when empty), ``"primal"``,
        ``"dual"``, or ``"default"`` (solver automatic).
    optimal_eps:
        Dual (optimality) tolerance; 0.0 means use the solver default.
        Passed as ``-dualtolerance`` to CLP/CBC.
    feasible_eps:
        Primal (feasibility) tolerance; 0.0 means use the solver default.
        Passed as ``-primaltolerance`` to CLP/CBC.
    barrier_eps:
        Barrier convergence tolerance; 0.0 means use the solver default.
        Passed as ``-dualbound`` is not applicable; barrier tol is not
        directly exposed via CLP CLI, so this parameter is accepted but
        silently ignored for CLP/CBC.

    Returns ``(success, output)`` where *success* is True when at least one
    COIN-OR binary was found and executed without crashing.
    """
    # Map algo names to COIN-OR command-line flags.
    # CLP/CBC accept these commands before "solve":
    #   "barrier"  → solve with interior point method
    #   "primalS"  → solve with primal simplex
    #   "dualS"    → solve with dual simplex
    effective_algo = algo.lower() if algo else "barrier"
    coinor_algo_args: dict[str, list[str]] = {
        "barrier": ["barrier"],
        "primal": ["primalS"],
        "dual": ["dualS"],
        "default": ["solve"],
    }
    algo_cmd = coinor_algo_args.get(effective_algo, ["barrier"])

    # Build tolerance flags (placed before the solve command).
    tol_args: list[str] = []
    if optimal_eps > 0:
        tol_args += ["-dualtolerance", str(optimal_eps)]
    if feasible_eps > 0:
        tol_args += ["-primaltolerance", str(feasible_eps)]
    # barrier_eps has no direct CLP/CBC command-line equivalent; ignore.

    # Determine which solvers are available before touching the file.
    available: list[tuple[str, str, list[str]]] = []
    for binary_name, extra_args in [
        ("clp", ["statistics"] + tol_args + algo_cmd),
        ("cbc", ["statistics"] + tol_args + algo_cmd),
    ]:
        binary = shutil.which(binary_name)
        if binary is not None:
            available.append((binary_name, binary, extra_args))

    if not available:
        return False, "Neither clp nor cbc binary found on PATH."

    results: list[str] = []
    with as_sanitized_lp(lp_path) as plain_path:
        for binary_name, binary, extra_args in available:
            log.debug("Running COIN-OR %s from: %s", binary_name.upper(), binary)
            try:
                proc = subprocess.run(
                    [binary, str(plain_path)] + extra_args,
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                    check=False,
                )
                raw = proc.stdout + proc.stderr
                findings = parse_coinor_infeasibility(raw)

                section_lines = [f"--- {binary_name.upper()} output ---", raw]
                if findings:
                    section_lines.insert(1, "\nKey infeasibility findings:")
                    section_lines[2:2] = [f"  ✗ {f}" for f in findings]
                    section_lines.append("")
                results.append("\n".join(section_lines))

            except subprocess.TimeoutExpired:
                results.append(
                    f"--- {binary_name.upper()} ---\n"
                    f"{binary_name.upper()} timed out after {timeout}s."
                )
            except OSError as exc:
                log.debug("Failed to run %s: %s", binary_name, exc)
                continue

    if not results:
        return False, "Neither clp nor cbc binary found on PATH."

    return True, "\n\n".join(results)


# ---------------------------------------------------------------------------
# GLPK
# ---------------------------------------------------------------------------


def run_local_glpk(lp_path: Path, timeout: int = 120) -> tuple[bool, str]:
    """
    Run ``glpsol`` (GLPK) to confirm LP infeasibility.

    Accepts plain or gzip-compressed LP files.

    GLPK does not support IIS finding directly, but confirms infeasibility
    and reports which constraints are violated at termination.

    Note: the ``--ranges`` flag is intentionally omitted — it triggers a
    sensitivity-analysis report that is not possible for infeasible problems
    and produces a misleading "Cannot produce sensitivity analysis report"
    warning in the output.

    Returns ``(success, output)``.
    """
    glpsol_bin = shutil.which("glpsol")
    if glpsol_bin is None:
        return False, "glpsol binary not found on PATH."

    log.debug("Running GLPK from: %s", glpsol_bin)
    try:
        with as_sanitized_lp(lp_path) as plain_path:
            result = subprocess.run(
                [glpsol_bin, "--lp", str(plain_path)],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
        return True, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return False, f"glpsol timed out after {timeout}s."
    except OSError as exc:
        return False, f"Failed to run glpsol: {exc}"


# ---------------------------------------------------------------------------
# Dispatcher: run_iis / run_all_solvers
# ---------------------------------------------------------------------------


def _section_header(solver_name: str) -> str:
    """Return a section separator for solver output."""
    width = 68
    label = f"── {solver_name} "
    fill = "─" * max(0, width - len(label))
    return f"\n┌{label}{fill}"


def _format_solver_block(name: str, _success: bool, output: str) -> str:
    """Format a single solver's output block."""
    return f"{_section_header(name)}\n{output}"


def run_all_solvers(
    lp_path: Path,
    *,
    algo: str = "",
    optimal_eps: float = 0.0,
    feasible_eps: float = 0.0,
    barrier_eps: float = 0.0,
    email: str = "",
    neos_url: str = _NEOS_DEFAULT_URL,
    timeout: int = 120,
) -> tuple[bool, str, str]:
    """
    Run every available solver against *lp_path* and combine their output.

    Parameters
    ----------
    algo:
        LP algorithm passed to COIN-OR and CPLEX solvers.  When empty,
        ``"barrier"`` is used as the default for these solvers.
    optimal_eps:
        Dual (optimality) tolerance passed to COIN-OR and CPLEX; 0.0 uses
        solver defaults.
    feasible_eps:
        Primal (feasibility) tolerance passed to COIN-OR and CPLEX; 0.0
        uses solver defaults.
    barrier_eps:
        Barrier convergence tolerance passed to CPLEX; 0.0 uses solver
        defaults.

    Returns ``(any_success, solver_name, combined_output)``.
    """
    parts: list[str] = []
    any_success = False

    # HiGHS (Python preferred over binary for richer IIS output)
    highs_bin = shutil.which("highs")
    has_highspy = False
    try:
        import highspy  # noqa: F401, PLC0415  # pylint: disable=unused-import

        has_highspy = True
    except ImportError:
        pass

    if has_highspy:
        ok, out = run_local_highs_python(lp_path, timeout=min(timeout, 30))
        any_success = any_success or ok
        parts.append(_format_solver_block("HiGHS (Python)", ok, out))
    elif highs_bin:
        ok, out = run_local_highs_binary(lp_path, timeout=timeout)
        any_success = any_success or ok
        parts.append(_format_solver_block("HiGHS (binary)", ok, out))

    # COIN-OR
    if shutil.which("clp") or shutil.which("cbc"):
        ok, out = run_local_coinor(
            lp_path,
            timeout=timeout,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
        )
        any_success = any_success or ok
        parts.append(_format_solver_block("COIN-OR (clp/cbc)", ok, out))

    # GLPK — skipped in "all" mode because glpsol cannot parse the CPLEX LP
    # format produced by COIN-OR / gtopt (it fails with "missing variable
    # name").  Users who need GLPK can still request it with --solver glpk.

    # CPLEX (local)
    if shutil.which("cplex"):
        ok, out = run_local_cplex(
            lp_path,
            timeout=timeout,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
        )
        any_success = any_success or ok
        parts.append(_format_solver_block("CPLEX (local)", ok, out))

    # NEOS (if email provided)
    if email:
        client = NeosClient(url=neos_url, timeout=max(timeout, 120))
        ok, out = client.submit_and_wait(lp_path, email)
        any_success = any_success or ok
        parts.append(_format_solver_block("NEOS (CPLEX)", ok, out))

    if not parts:
        hint = (
            f"\n{col.c(col._YELLOW, 'Tip:')} "  # noqa: SLF001
            "Install a local solver (CPLEX, HiGHS, or CLP/CBC) or\n"
            "  provide --email <address> to submit to the NEOS server.\n"
            "  CLP/CBC: sudo apt install coinor-clp coinor-cbc\n"
            "  HiGHS:   pip install highspy"
        )
        return False, "all", hint

    return any_success, "all", "\n".join(parts)


def run_iis(
    lp_path: Path,
    solver: str = "all",
    algo: str = "",
    optimal_eps: float = 0.0,
    feasible_eps: float = 0.0,
    barrier_eps: float = 0.0,
    email: str = "",
    neos_url: str = _NEOS_DEFAULT_URL,
    timeout: int = 120,
) -> tuple[bool, str, str]:
    """
    Find the IIS for *lp_path* using the requested *solver*.

    Parameters
    ----------
    solver:
        One of ``"all"`` (default — runs every available solver),
        ``"auto"`` (first available), ``"cplex"``, ``"highs"``,
        ``"coinor"``, ``"glpk"``, ``"neos"``.
    algo:
        LP algorithm passed to COIN-OR and CPLEX: ``"barrier"`` (default
        when empty), ``"primal"``, ``"dual"``, or ``"default"``.
    optimal_eps:
        Dual (optimality) tolerance; 0.0 uses solver defaults.
    feasible_eps:
        Primal (feasibility) tolerance; 0.0 uses solver defaults.
    barrier_eps:
        Barrier convergence tolerance; 0.0 uses solver defaults.

    Returns
    -------
    ``(success, solver_name, output)``
    """
    if solver in ("all", "auto"):
        return run_all_solvers(
            lp_path,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
            email=email,
            neos_url=neos_url,
            timeout=timeout,
        )

    solver_lower = solver.lower()

    if solver_lower == "cplex":
        ok, out = run_local_cplex(
            lp_path,
            timeout=timeout,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
        )
        return ok, "CPLEX", out

    if solver_lower == "highs":
        try:
            import highspy  # noqa: F401, PLC0415  # pylint: disable=unused-import

            ok, out = run_local_highs_python(lp_path, timeout=min(timeout, 30))
        except ImportError:
            ok, out = run_local_highs_binary(lp_path, timeout=timeout)
        return ok, "HiGHS", out

    if solver_lower == "coinor":
        ok, out = run_local_coinor(
            lp_path,
            timeout=timeout,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
        )
        return ok, "COIN-OR (clp/cbc)", out

    if solver_lower == "glpk":
        ok, out = run_local_glpk(lp_path, timeout=timeout)
        return ok, "GLPK", out

    if solver_lower == "neos":
        if not email:
            return (
                False,
                "NEOS",
                "NEOS requires an e-mail address (--email EMAIL).",
            )
        client = NeosClient(url=neos_url, timeout=max(timeout, 120))
        ok, out = client.submit_and_wait(lp_path, email)
        return ok, "NEOS (CPLEX)", out

    return False, solver, f"Unknown solver '{solver}'."


# Public alias used in the monolithic module and tests
run_all_solvers_fn = run_all_solvers
