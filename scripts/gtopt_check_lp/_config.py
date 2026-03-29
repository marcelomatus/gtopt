# SPDX-License-Identifier: BSD-3-Clause
"""Configuration file I/O and interactive first-run setup for gtopt_check_lp.

Uses the centralized ``~/.gtopt.conf`` via the :mod:`gtopt_config` module.
AI settings are shared from the ``[global]`` section; tool-specific settings
(email, solver, timeout, neos_url) live in ``[gtopt_check_lp]``.

Backward compatibility: if ``~/.gtopt_check_lp.conf`` exists and
``~/.gtopt.conf`` does not, the old file is migrated automatically.
"""

import shutil
import subprocess
import sys
from pathlib import Path

from gtopt_config import (
    DEFAULT_CONFIG_PATH,
    get_global,
    get_section,
    load_config as _load_unified_config,
    save_section,
)

from . import _colors as col
from ._ai import _AI_DEFAULT_MODEL, _AI_DEFAULT_PROVIDER, _AI_PROVIDERS

_SECTION = "gtopt_check_lp"
_OLD_CONFIG_PATH = Path.home() / ".gtopt_check_lp.conf"

# Tool-specific keys and their defaults
_TOOL_DEFAULTS: dict[str, str] = {
    "email": "",
    "solver": "auto",
    "timeout": "5",
    "neos_url": "https://neos-server.org:3333",
}

# Keys that are read from [global] (AI + color)
_GLOBAL_KEYS = ("ai_enabled", "ai_provider", "ai_model", "ai_prompt", "color")


def default_config_path() -> Path:
    """Return the unified config file path: ``~/.gtopt.conf``."""
    return DEFAULT_CONFIG_PATH


def read_git_email() -> str:
    """Read the user e-mail from the git global configuration.

    Returns an empty string when git is not installed or no email is set.
    """
    git_bin = shutil.which("git")
    if git_bin is None:
        return ""
    try:
        result = subprocess.run(
            [git_bin, "config", "--global", "user.email"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        return result.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def _migrate_old_config() -> None:
    """Migrate settings from ``~/.gtopt_check_lp.conf`` → ``~/.gtopt.conf``.

    Only runs when the old file exists and the unified file either doesn't
    exist or doesn't have a [gtopt_check_lp] section yet.
    """
    if not _OLD_CONFIG_PATH.exists():
        return

    import configparser  # noqa: PLC0415

    parser = configparser.ConfigParser()
    try:
        parser.read(_OLD_CONFIG_PATH, encoding="utf-8")
    except configparser.Error:
        return

    if not parser.has_section(_SECTION):
        return

    # Collect tool-specific values
    tool_values: dict[str, str] = {}
    global_values: dict[str, str] = {}
    for key in parser.options(_SECTION):
        val = parser.get(_SECTION, key)
        if key in _GLOBAL_KEYS:
            global_values[key] = val
        else:
            tool_values[key] = val

    # Write to unified config
    if tool_values:
        save_section(DEFAULT_CONFIG_PATH, _SECTION, tool_values)
    if global_values:
        save_section(DEFAULT_CONFIG_PATH, "global", global_values)


def load_config(config_path: Path) -> dict[str, str]:
    """Load gtopt_check_lp settings from the unified config.

    Reads AI/color settings from ``[global]`` and tool-specific settings
    from ``[gtopt_check_lp]``.  Returns a flat dict compatible with the
    existing code.
    """
    # Auto-migrate old config file on first access
    if _OLD_CONFIG_PATH.exists() and not config_path.exists():
        _migrate_old_config()

    cfg_parser = _load_unified_config(config_path)

    cfg: dict[str, str] = {}

    # Global keys (AI + color)
    for key in _GLOBAL_KEYS:
        cfg[key] = get_global(cfg_parser, key)

    # Tool-specific keys
    for key, default in _TOOL_DEFAULTS.items():
        cfg[key] = get_section(cfg_parser, _SECTION, key, fallback=default)

    return cfg


def save_config(config_path: Path, cfg: dict[str, str]) -> None:
    """Write *cfg* to the unified config file.

    Global keys (AI, color) go to ``[global]``; the rest to
    ``[gtopt_check_lp]``.
    """
    global_values: dict[str, str] = {}
    tool_values: dict[str, str] = {}
    for key, val in cfg.items():
        if key in _GLOBAL_KEYS:
            global_values[key] = val
        else:
            tool_values[key] = val

    if global_values:
        save_section(config_path, "global", global_values)
    if tool_values:
        save_section(config_path, _SECTION, tool_values)


# ---------------------------------------------------------------------------
# Solver availability helpers
# ---------------------------------------------------------------------------

_SOLVER_BINARIES: list[tuple[str, str, str | None]] = [
    ("highs", "HiGHS", "sudo apt install highs"),
    ("cbc", "COIN-OR CBC", "sudo apt install coinor-cbc"),
]


def get_solver_status() -> list[tuple[str, str, bool, str | None]]:
    """Return installation status for every known solver.

    Each entry is ``(binary, label, installed, apt_hint)``.
    CPLEX is checked first via :func:`find_cplex_binary` which also
    searches well-known installation directories (e.g. ``/opt/cplex``).
    """
    from ._solvers import find_cplex_binary  # noqa: PLC0415

    status: list[tuple[str, str, bool, str | None]] = []

    # CPLEX — uses find_cplex_binary() to check PATH + well-known dirs
    cplex_found = find_cplex_binary() is not None
    status.append(("cplex", "CPLEX", cplex_found, None))

    for binary, label, hint in _SOLVER_BINARIES:
        installed = shutil.which(binary) is not None
        if binary == "highs" and not installed:
            try:
                import highspy  # noqa: F401, PLC0415  # pylint: disable=unused-import

                installed = True
                hint = None
            except ImportError:
                pass
        status.append((binary, label, installed, hint))
    return status


def print_solver_status(use_color: bool = True) -> None:
    """Print a table of available and missing solvers with installation hints."""

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{col._RESET}" if use_color else text  # noqa: SLF001

    print(_c(col._BOLD, "\nSolver availability:"))  # noqa: SLF001
    any_missing = False
    for _binary, label, installed, hint in get_solver_status():
        if installed:
            mark = _c(col._GREEN, "✓")  # noqa: SLF001
            print(f"  {mark} {label}")
        else:
            mark = _c(col._RED, "✗")  # noqa: SLF001
            print(f"  {mark} {label}  (not found on PATH)")
            any_missing = True

    if any_missing:
        print()
        print(_c(col._YELLOW, "  To install missing open-source solvers:"))  # noqa: SLF001
        for _binary, label, installed, hint in get_solver_status():
            if not installed and hint:
                print(f"    {hint}   # {label}")
        print()
        print(
            "  CPLEX is a commercial solver; download from "
            "https://www.ibm.com/products/ilog-cplex-optimization-studio"
        )
        print("  HiGHS Python: pip install highspy")


# ---------------------------------------------------------------------------
# Interactive first-run setup
# ---------------------------------------------------------------------------


def _prompt(prompt_text: str, default: str = "") -> str:
    """Prompt the user for input, showing a default if provided."""
    if not sys.stdin.isatty():
        return default
    full_prompt = f"{prompt_text} [{default}]: " if default else f"{prompt_text}: "
    try:
        answer = input(full_prompt).strip()
        return answer if answer else default
    except (EOFError, KeyboardInterrupt):
        return default


def run_interactive_setup(config_path: Path, use_color: bool = True) -> dict[str, str]:
    """Run the interactive first-run setup wizard.

    Reads the git email as a proposed default, checks solver availability,
    adds AI configuration questions, and writes the resulting config to
    the unified ``~/.gtopt.conf``.

    Returns the final config dict.
    """

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{col._RESET}" if use_color else text  # noqa: SLF001

    print()
    print(_c(col._BOLD, "─── gtopt_check_lp first-run setup ───"))  # noqa: SLF001
    print(
        "\nThis tool analyzes infeasible LP files generated by gtopt."
        "\nLet's configure it for your system."
    )

    cfg = load_config(config_path)

    # ── Email ──────────────────────────────────────────────────────────────
    git_email = read_git_email()
    current_email = cfg.get("email", "")
    proposed_email = current_email or git_email

    print()
    if git_email and not current_email:
        print(f"  Found git user.email: {_c(col._CYAN, git_email)}")  # noqa: SLF001
    elif current_email:
        print(f"  Current email: {_c(col._CYAN, current_email)}")  # noqa: SLF001

    email = _prompt(
        "  E-mail address (used for NEOS server submissions)", proposed_email
    )
    cfg["email"] = email

    # ── Solver ─────────────────────────────────────────────────────────────
    print()
    print_solver_status(use_color=use_color)

    current_solver = cfg.get("solver", "auto")
    solver_choices = ("all", "auto", "cplex", "highs", "coinor", "neos")
    solver_hint = "  Preferred solver " + _c(col._CYAN, str(list(solver_choices)))  # noqa: SLF001
    solver = _prompt(solver_hint, current_solver)
    if solver not in solver_choices:
        print(f"  Unknown solver '{solver}', using 'all'")
        solver = "all"
    cfg["solver"] = solver

    # ── Timeout ────────────────────────────────────────────────────────────
    current_timeout = cfg.get("timeout", "5")
    timeout_str = _prompt("  Default solver timeout in seconds", current_timeout)
    try:
        int(timeout_str)
        cfg["timeout"] = timeout_str
    except ValueError:
        print(f"  Invalid timeout '{timeout_str}', keeping {current_timeout}")
        cfg["timeout"] = current_timeout

    # ── Color ──────────────────────────────────────────────────────────────
    current_color = cfg.get("color", "auto")
    color = _prompt("  Color output ('auto', 'always', 'never')", current_color)
    if color not in ("auto", "always", "never"):
        color = "auto"
    cfg["color"] = color

    # ── AI settings (shared in [global]) ──────────────────────────────────
    print()
    print(_c(col._BOLD, "AI diagnostics (shared across all gtopt tools):"))  # noqa: SLF001
    print(
        "\n  When an API key is available, gtopt tools can send reports"
        "\n  to an AI provider for expert diagnosis."
        "\n  Supported providers: " + ", ".join(_AI_PROVIDERS)
    )

    current_ai_enabled = cfg.get("ai_enabled", "true")
    ai_enabled_str = _prompt(
        "  Enable AI diagnostics by default ('true' or 'false')", current_ai_enabled
    )
    if ai_enabled_str.lower() not in ("true", "false", "1", "0", "yes", "no"):
        ai_enabled_str = "true"
    cfg["ai_enabled"] = ai_enabled_str

    current_ai_provider = cfg.get("ai_provider", _AI_DEFAULT_PROVIDER)
    provider_choices_str = _c(col._CYAN, str(list(_AI_PROVIDERS)))  # noqa: SLF001
    ai_provider = _prompt(
        f"  Preferred AI provider {provider_choices_str}", current_ai_provider
    )
    if ai_provider not in _AI_PROVIDERS:
        print(f"  Unknown provider '{ai_provider}', using '{_AI_DEFAULT_PROVIDER}'")
        ai_provider = _AI_DEFAULT_PROVIDER
    cfg["ai_provider"] = ai_provider

    default_model = _AI_DEFAULT_MODEL.get(ai_provider, "")
    current_ai_model = cfg.get("ai_model", "")
    ai_model = _prompt(
        f"  AI model override (leave blank for default: {default_model!r})",
        current_ai_model,
    )
    cfg["ai_model"] = ai_model

    # ── Save ───────────────────────────────────────────────────────────────
    save_config(config_path, cfg)
    print()
    print(f"  {_c(col._GREEN, '✓')} Configuration saved to: {config_path}")  # noqa: SLF001
    print()

    return cfg
