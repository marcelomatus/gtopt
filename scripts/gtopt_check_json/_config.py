# SPDX-License-Identifier: BSD-3-Clause
"""Configuration file I/O for gtopt_check_json.

Uses the centralized ``~/.gtopt.conf`` via the :mod:`gtopt_config` module.
AI settings are shared from the ``[global]`` section; tool-specific settings
(check toggles) live in ``[gtopt_check_json]``.

Backward compatibility: if ``~/.gtopt_check_json.conf`` exists and
``~/.gtopt.conf`` does not, the old file is migrated automatically.
"""

import configparser
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

_SECTION = "gtopt_check_json"
_OLD_CONFIG_PATH = Path.home() / ".gtopt_check_json.conf"

# Each check can be enabled / disabled individually via the config file.
# The key is the check id; the value is "true" or "false".
CHECK_DEFAULTS: dict[str, str] = {
    "uid_uniqueness": "true",
    "name_uniqueness": "true",
    "demand_lmax_nonneg": "true",
    "affluent_nonneg": "true",
    "element_references": "true",
    "bus_connectivity": "true",
    "unreferenced_elements": "true",
    "capacity_adequacy": "true",
    "battery_efficiency": "true",
    "cascade_levels": "true",
    "simulation_mode": "true",
    "sddp_options": "true",
    "cascade_solver_type": "true",
    "boundary_cuts": "true",
    # Engine-side validation: shells out to `gtopt --lp-only` and harvests
    # the C++-side `Validation: …` log lines.  Enabled by default — picks
    # up every C++ validator (referential integrity, positivity,
    # piecewise feasibility, aperture refs, completeness, scenario
    # probability rescale, …) automatically with zero drift.  Set to
    # "false" if you don't have the gtopt binary handy or want a
    # JSON-only static check.
    "engine_validate": "true",
    "ai_system_analysis": "false",
}

# Keys that are read from [global] (AI + color)
_GLOBAL_KEYS = ("ai_enabled", "ai_provider", "ai_model", "ai_prompt", "color")

# Supported AI providers (shared across all gtopt tools via [global]).
_AI_PROVIDERS = ("claude", "openai", "deepseek", "github")
_AI_DEFAULT_PROVIDER = "claude"


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
    """Migrate settings from ``~/.gtopt_check_json.conf`` → ``~/.gtopt.conf``."""
    if not _OLD_CONFIG_PATH.exists():
        return

    parser = configparser.ConfigParser()
    try:
        parser.read(_OLD_CONFIG_PATH, encoding="utf-8")
    except configparser.Error:
        return

    if not parser.has_section(_SECTION):
        return

    tool_values: dict[str, str] = {}
    global_values: dict[str, str] = {}
    for key in parser.options(_SECTION):
        val = parser.get(_SECTION, key)
        if key in _GLOBAL_KEYS:
            global_values[key] = val
        else:
            tool_values[key] = val

    if tool_values:
        save_section(DEFAULT_CONFIG_PATH, _SECTION, tool_values)
    if global_values:
        save_section(DEFAULT_CONFIG_PATH, "global", global_values)


def load_config(config_path: Path) -> dict[str, str]:
    """Load gtopt_check_json settings from the unified config.

    Reads AI/color settings from ``[global]`` and check toggles from
    ``[gtopt_check_json]``.  Returns a flat dict.
    """
    if _OLD_CONFIG_PATH.exists() and not config_path.exists():
        _migrate_old_config()

    cfg_parser = _load_unified_config(config_path)

    cfg: dict[str, str] = {}

    # Global keys (AI + color)
    for key in _GLOBAL_KEYS:
        cfg[key] = get_global(cfg_parser, key)

    # Tool-specific keys (check_* toggles)
    for check_id, default_val in CHECK_DEFAULTS.items():
        key = f"check_{check_id}"
        cfg[key] = get_section(cfg_parser, _SECTION, key, fallback=default_val)

    return cfg


def save_config(config_path: Path, cfg: dict[str, str]) -> None:
    """Write *cfg* to the unified config file.

    Global keys (AI, color) go to ``[global]``; check_* keys to
    ``[gtopt_check_json]``.
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


def is_check_enabled(cfg: dict[str, str], check_id: str) -> bool:
    """Return True if *check_id* is enabled in *cfg*."""
    key = f"check_{check_id}"
    return cfg.get(key, CHECK_DEFAULTS.get(check_id, "true")).lower() in (
        "true",
        "1",
        "yes",
    )


def run_interactive_setup(config_path: Path, use_color: bool = True) -> dict[str, str]:
    """Run the interactive first-run setup wizard.

    Reads the git email as a proposed default, prompts for check
    enable/disable settings and AI configuration, then writes the
    resulting config to the unified ``~/.gtopt.conf``.

    Returns the final config dict.
    """

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{col.RESET}" if use_color else text

    print()
    print(_c(col.BOLD, "─── gtopt_check_json first-run setup ───"))
    print(
        "\nThis tool validates gtopt JSON planning files."
        "\nLet's configure which checks to enable."
    )

    cfg = load_config(config_path)

    # ── Color ──────────────────────────────────────────────────────────────
    current_color = cfg.get("color", "auto")
    color = _prompt("  Color output ('auto', 'always', 'never')", current_color)
    if color not in ("auto", "always", "never"):
        color = "auto"
    cfg["color"] = color

    # ── Checks ─────────────────────────────────────────────────────────────
    print()
    print(_c(col.BOLD, "Checks:"))
    for check_id, default_val in CHECK_DEFAULTS.items():
        current = cfg.get(f"check_{check_id}", default_val)
        answer = _prompt(f"  {check_id} (true/false)", current)
        if answer.lower() not in ("true", "false", "1", "0", "yes", "no"):
            answer = default_val
        cfg[f"check_{check_id}"] = answer

    # ── AI settings (shared in [global]) ──────────────────────────────────
    print()
    print(_c(col.BOLD, "AI diagnostics (shared across all gtopt tools):"))
    print(
        "\n  When an API key is available, gtopt tools can send reports"
        "\n  to an AI provider for expert diagnosis."
        "\n  Supported providers: " + ", ".join(_AI_PROVIDERS)
    )

    current_ai = cfg.get("ai_enabled", "false")
    ai_str = _prompt("  Enable AI diagnostics ('true' or 'false')", current_ai)
    if ai_str.lower() not in ("true", "false", "1", "0", "yes", "no"):
        ai_str = "false"
    cfg["ai_enabled"] = ai_str

    current_ai_provider = cfg.get("ai_provider", _AI_DEFAULT_PROVIDER)
    provider_choices_str = _c(col.CYAN, str(list(_AI_PROVIDERS)))
    ai_provider = _prompt(
        f"  Preferred AI provider {provider_choices_str}", current_ai_provider
    )
    if ai_provider not in _AI_PROVIDERS:
        print(f"  Unknown provider '{ai_provider}', using '{_AI_DEFAULT_PROVIDER}'")
        ai_provider = _AI_DEFAULT_PROVIDER
    cfg["ai_provider"] = ai_provider

    current_ai_model = cfg.get("ai_model", "")
    ai_model = _prompt(
        "  AI model override (leave blank for provider default)",
        current_ai_model,
    )
    cfg["ai_model"] = ai_model

    # ── Save ───────────────────────────────────────────────────────────────
    save_config(config_path, cfg)
    print()
    print(f"  {_c(col.GREEN, '✓')} Configuration saved to: {config_path}")
    print()
    return cfg


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
