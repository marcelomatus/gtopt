# SPDX-License-Identifier: BSD-3-Clause
"""Configuration file I/O for gtopt_check_pampl.

Uses the centralized ``~/.gtopt.conf`` via the :mod:`gtopt_config` module.
AI settings are shared from the ``[global]`` section; tool-specific settings
(check toggles) live in ``[gtopt_check_pampl]``.
"""

import sys
from pathlib import Path

from gtopt_config import (
    DEFAULT_CONFIG_PATH,
    get_global,
    get_section,
    load_config as _load_unified_config,
    save_section,
)

_SECTION = "gtopt_check_pampl"

# Each check can be enabled / disabled individually via the config file.
# The key is the check id; the value is "true" or "false".
CHECK_DEFAULTS: dict[str, str] = {
    "syntax": "true",
    "semicolons": "true",
    "constraint_names": "true",
    "param_declarations": "true",
    "element_references": "true",
    "operator_usage": "true",
    "domain_clauses": "true",
    "template_variables": "true",
    "duplicate_names": "true",
    "inactive_constraints": "true",
}

# Keys that are read from [global] (AI + color)
_GLOBAL_KEYS = ("ai_enabled", "ai_provider", "ai_model", "ai_prompt", "color")

# Supported AI providers (shared across all gtopt tools via [global]).
_AI_PROVIDERS = ("claude", "openai", "deepseek", "github")
_AI_DEFAULT_PROVIDER = "claude"


def default_config_path() -> Path:
    """Return the unified config file path: ``~/.gtopt.conf``."""
    return DEFAULT_CONFIG_PATH


def load_config(config_path: Path) -> dict[str, str]:
    """Load gtopt_check_pampl settings from the unified config.

    Reads AI/color settings from ``[global]`` and check toggles from
    ``[gtopt_check_pampl]``.  Returns a flat dict.
    """
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
    ``[gtopt_check_pampl]``.
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

    Prompts for check enable/disable settings and AI configuration, then
    writes the resulting config to the unified ``~/.gtopt.conf``.

    Returns the final config dict.
    """
    bold = "\033[1m" if use_color else ""
    green = "\033[32m" if use_color else ""
    cyan = "\033[36m" if use_color else ""
    reset = "\033[0m" if use_color else ""

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{reset}" if use_color else text

    print()
    print(_c(bold, "─── gtopt_check_pampl first-run setup ───"))
    print(
        "\nThis tool validates gtopt PAMPL/TAMPL constraint files."
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
    print(_c(bold, "Checks:"))
    for check_id, default_val in CHECK_DEFAULTS.items():
        current = cfg.get(f"check_{check_id}", default_val)
        answer = _prompt(f"  {check_id} (true/false)", current)
        if answer.lower() not in ("true", "false", "1", "0", "yes", "no"):
            answer = default_val
        cfg[f"check_{check_id}"] = answer

    # ── AI settings (shared in [global]) ──────────────────────────────────
    print()
    print(_c(bold, "AI diagnostics (shared across all gtopt tools):"))
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
    provider_choices_str = _c(cyan, str(list(_AI_PROVIDERS)))
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
    print(f"  {_c(green, '✓')} Configuration saved to: {config_path}")
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
