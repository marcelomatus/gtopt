# SPDX-License-Identifier: BSD-3-Clause
"""Local and remote solver wrappers for LP infeasibility analysis.

CPLEX command note
------------------
The CPLEX Interactive Optimizer command for the conflict refiner is
``tools conflict``, followed by ``display conflict all`` to show the minimal
infeasible subset.  In CPLEX 22.1+ the conflict refiner lives under the
``tools`` submenu; neither bare ``conflict`` nor ``refineconflict`` are
top-level commands.

To ensure the conflict refiner has a working LP basis, presolve must be
disabled *before* solving (``set preprocessing presolve n``); otherwise CPLEX
terminates in presolve and the conflict refiner has nothing to work with.
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
# Well-known CPLEX installation directories (checked when not on PATH)
# ---------------------------------------------------------------------------

_CPLEX_WELL_KNOWN_DIRS: list[str] = [
    "/opt/cplex/bin/x86-64_linux",
    "/opt/ibm/ILOG/CPLEX_Studio*/cplex/bin/x86-64_linux",
    "/opt/ibm/cplex/bin/x86-64_linux",
]


def find_cplex_binary() -> str | None:
    """Locate the ``cplex`` binary on PATH or in well-known directories.

    Returns the full path as a string, or *None* if not found.
    """
    # 1. Check PATH
    on_path = shutil.which("cplex")
    if on_path:
        return on_path

    # 2. Check well-known installation directories
    import glob as _glob  # noqa: PLC0415

    for pattern in _CPLEX_WELL_KNOWN_DIRS:
        for directory in _glob.glob(pattern):
            candidate = Path(directory) / "cplex"
            if candidate.is_file() and _is_executable(candidate):
                return str(candidate)

    return None


def _is_executable(path: Path) -> bool:
    """Return True if *path* is an executable file."""
    import os  # noqa: PLC0415

    return os.access(path, os.X_OK)


# ---------------------------------------------------------------------------
# Solver detection
# ---------------------------------------------------------------------------


def detect_local_solvers() -> list[str]:
    """Return human-readable labels for every locally available solver."""
    available = []
    if find_cplex_binary():
        available.append("CPLEX")
    cbc_bin = shutil.which("cbc")
    clp_bin = shutil.which("clp")
    if cbc_bin:
        available.append("CBC (COIN-OR)")
    elif clp_bin:
        available.append("CLP (COIN-OR)")

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
    1. Disable presolve so the simplex method (not presolve) detects
       infeasibility and builds an infeasibility certificate.
    2. Optionally set the LP method (barrier, primal, or dual).
    3. Optionally set numerical tolerances.
    4. Re-solve with ``optimize``.
    5. Invoke the CPLEX conflict refiner with ``tools conflict``.
    6. Display the minimal conflict set with ``display conflict all``.

    Note: In CPLEX 22.1+, the conflict refiner lives under the ``tools``
    submenu (``tools conflict``).  Neither bare ``conflict`` nor
    ``refineconflict`` are top-level interactive commands.
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

    # Bump barrier numerical limits so the IIS conflict refiner does not
    # prematurely declare "infeasible due to dual objective limit" when
    # exploring large-magnitude infeasibilities (e.g. SDDP-cut clamps
    # interacting with hard reservoir efin rows).  Two relevant CPLEX
    # parameters:
    #   - `barrier limits growth`   default 1e12 — growth-between-iterations
    #     limit; gets triggered first when the dual objective blows up
    #   - `barrier limits objrange` default 1e20 — absolute dual-objective
    #     range; secondary safeguard
    # Bumping growth to 1e18 (per user request) lets the barrier explore
    # higher magnitudes; matching objrange to 1e30 keeps the secondary
    # safeguard disabled effectively.
    bar_limit_cmd = (
        "set barrier limits growth 1e18\nset barrier limits objrange 1e30\n"
        if effective_algo == "barrier"
        else ""
    )

    return (
        f"read {lp_path}\n"
        "set preprocessing presolve n\n"
        f"{method_cmd}"
        f"{bar_limit_cmd}"
        f"{tol_cmds}"
        "optimize\n"
        "tools conflict\n"
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
    cplex_bin = find_cplex_binary()
    if cplex_bin is None:
        return False, "cplex binary not found on PATH or well-known locations."

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

    def _get_name(  # noqa: PLR0911
        h: "highspy.Highs",
        getter: str,
        idx: int,
    ) -> str:
        """Retrieve a column/row name, handling the (status, name) tuple API."""
        try:
            result = getattr(h, getter)(idx)
            if isinstance(result, tuple):
                return str(result[1])
            return str(result)
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            return str(idx)

    def _bound_label(raw: object) -> str:
        """Map an IIS bound status integer/enum to a readable label."""
        _labels = {
            -1: "dropped",
            0: "null",
            1: "free",
            2: "lower",
            3: "upper",
            4: "boxed",
        }
        val = raw.value if hasattr(raw, "value") else int(raw)  # type: ignore[call-overload]
        return _labels.get(val, str(raw))

    def _try_iis(  # noqa: PLR0912
        h: "highspy.Highs",
        lines: list[str],
        plain_lp_path: Path,
    ) -> bool:
        """Try IIS computation with escalating strategies.

        Returns True if IIS members were found.
        """
        # Escalate: FromLp (2), then FromRay+FromLp (3),
        # then full irreducible (2+4+1 = 7).
        strategies = [2, 3, 7]
        col_idx: list[int] = []
        row_idx: list[int] = []
        iis = None

        for strategy in strategies:
            try:
                h.setOptionValue("iis_strategy", strategy)
            except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                break
            _iis_status, iis = h.getIis()  # pylint: disable=no-value-for-parameter,unpacking-non-sequence
            col_idx = getattr(iis, "col_index_", []) or []
            row_idx = getattr(iis, "row_index_", []) or []
            if col_idx or row_idx:
                break

        if not col_idx and not row_idx:
            return False

        lines.append("\n-- HiGHS IIS (Irreducible Infeasible Subsystem) --")

        if col_idx:
            lines.append(f"  Infeasible variable bounds ({len(col_idx)}):")
            col_bounds = getattr(iis, "col_bound_", [])
            for pos, ci in enumerate(col_idx):
                name = _get_name(h, "getColName", ci)
                bound_info = ""
                if col_bounds and pos < len(col_bounds):
                    bound_info = f"  [bound={_bound_label(col_bounds[pos])}]"
                lines.append(f"    • {name}{bound_info}")

        if row_idx:
            lines.append(f"  Infeasible constraints ({len(row_idx)}):")
            row_bounds = getattr(iis, "row_bound_", [])
            for pos, ri in enumerate(row_idx):
                name = _get_name(h, "getRowName", ri)
                bound_info = ""
                if row_bounds and pos < len(row_bounds):
                    bound_info = f"  [bound={_bound_label(row_bounds[pos])}]"
                lines.append(f"    • {name}{bound_info}")

        # Write the IIS sub-model for easy inspection
        try:
            iis_path = plain_lp_path.with_suffix(".iis.lp")
            h.writeIisModel(str(iis_path))
            lines.append(f"\n  IIS sub-model written to: {iis_path}")
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            pass

        return True

    def _try_feasibility_relaxation(
        h: "highspy.Highs",
        lines: list[str],
    ) -> bool:
        """Fall back to feasibilityRelaxation when IIS is empty."""
        if not hasattr(h, "feasibilityRelaxation"):
            return False

        fr_status = h.feasibilityRelaxation(1.0, 1.0, 1.0)
        if str(fr_status) != "HighsStatus.kOk":
            return False

        sol = h.getSolution()
        row_duals = sol.row_dual
        num_rows = h.getNumRow()

        violations: list[tuple[str, float]] = []
        for i in range(num_rows):
            if abs(row_duals[i]) > 1e-6:
                name = _get_name(h, "getRowName", i)
                violations.append((name, row_duals[i]))

        if not violations:
            return False

        violations.sort(key=lambda x: abs(x[1]), reverse=True)
        lines.append(
            "\n-- HiGHS Feasibility Relaxation (constraints requiring relaxation) --"
        )
        lines.append(f"  {len(violations)} constraint(s) involved in infeasibility:")
        for name, dual in violations[:50]:
            lines.append(f"    • {name}  (dual={dual:+.6g})")
        if len(violations) > 50:
            lines.append(f"    … and {len(violations) - 50} more")
        return True

    def _run(plain_lp_path: Path) -> None:
        lines: list[str] = []
        try:
            h = highspy.Highs()
            h.silent()
            h.readModel(str(plain_lp_path))
            h.setOptionValue("presolve", "off")
            h.run()
            model_status = str(h.getModelStatus())
            lines.append(f"HiGHS model status: {model_status}")

            if "infeasible" not in model_status.lower():
                result_holder.append((True, "\n".join(lines)))
                return

            # Try IIS first, then feasibility relaxation as fallback
            try:
                found = _try_iis(h, lines, plain_lp_path)
                if not found:
                    found = _try_feasibility_relaxation(h, lines)
                if not found:
                    lines.append(
                        "\n-- HiGHS IIS --\n"
                        "  Could not identify specific infeasible members.\n"
                        "  Try running with the HiGHS binary (--solver highs) for\n"
                        "  more detailed output."
                    )
            except (AttributeError, Exception) as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                lines.append(f"\n  IIS/relaxation analysis failed: {exc}")

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
    Run CBC (COIN-OR) to analyse the LP file for infeasibility.

    Accepts plain or gzip-compressed LP files.

    Prefers ``cbc``; falls back to ``clp`` only when ``cbc`` is absent.

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

    # Prefer cbc; fall back to clp only when cbc is absent.
    extra_args = ["statistics"] + tol_args + algo_cmd
    cbc_bin = shutil.which("cbc")
    clp_bin = shutil.which("clp")
    if cbc_bin:
        available = [("cbc", cbc_bin, extra_args)]
    elif clp_bin:
        available = [("clp", clp_bin, extra_args)]
    else:
        return False, "Neither cbc nor clp binary found on PATH."

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
        return False, "Neither cbc nor clp binary found on PATH."

    return True, "\n\n".join(results)


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


def _run_auto_solvers(
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
    """Run only HiGHS and CPLEX (local or NEOS) — the default ``auto`` strategy.

    Unlike :func:`run_all_solvers`, this skips COIN-OR CLP/CBC to keep output
    concise when the user hasn't explicitly requested all solvers.

    Returns ``(any_success, "auto", combined_output)``.
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

    # CPLEX (local) — when available, replaces the NEOS remote call
    has_local_cplex = find_cplex_binary() is not None
    if has_local_cplex:
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

    # NEOS (only if no local CPLEX and email provided)
    if not has_local_cplex and email:
        client = NeosClient(url=neos_url, timeout=max(timeout, 120))
        ok, out = client.submit_and_wait(lp_path, email)
        any_success = any_success or ok
        parts.append(_format_solver_block("NEOS (CPLEX)", ok, out))

    if not parts:
        hint = (
            f"\n{col.c(col._YELLOW, 'Tip:')} "  # noqa: SLF001
            "Install a local solver (CPLEX or HiGHS) or\n"
            "  provide --email <address> to submit to the NEOS server.\n"
            "  HiGHS:   pip install highspy\n"
            "  All solvers: use --solver all"
        )
        return False, "auto", hint

    return any_success, "auto", "\n".join(parts)


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

    # COIN-OR (prefer cbc)
    if shutil.which("cbc") or shutil.which("clp"):
        ok, out = run_local_coinor(
            lp_path,
            timeout=timeout,
            algo=algo,
            optimal_eps=optimal_eps,
            feasible_eps=feasible_eps,
            barrier_eps=barrier_eps,
        )
        any_success = any_success or ok
        parts.append(_format_solver_block("COIN-OR (cbc)", ok, out))

    # CPLEX (local) — when available, replaces the NEOS remote call
    has_local_cplex = find_cplex_binary() is not None
    if has_local_cplex:
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

    # NEOS (only if no local CPLEX and email provided)
    if not has_local_cplex and email:
        client = NeosClient(url=neos_url, timeout=max(timeout, 120))
        ok, out = client.submit_and_wait(lp_path, email)
        any_success = any_success or ok
        parts.append(_format_solver_block("NEOS (CPLEX)", ok, out))

    if not parts:
        hint = (
            f"\n{col.c(col._YELLOW, 'Tip:')} "  # noqa: SLF001
            "Install a local solver (CPLEX, HiGHS, or CBC) or\n"
            "  provide --email <address> to submit to the NEOS server.\n"
            "  CBC:   sudo apt install coinor-cbc\n"
            "  HiGHS: pip install highspy"
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
        ``"coinor"``, ``"neos"``.
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
    if solver == "all":
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

    if solver == "auto":
        return _run_auto_solvers(
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
        return ok, "COIN-OR (cbc)", out

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
