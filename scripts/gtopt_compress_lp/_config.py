# SPDX-License-Identifier: BSD-3-Clause
"""Configuration file I/O and interactive first-run setup for gtopt_compress_lp.

Uses the centralized ``~/.gtopt.conf`` via the :mod:`gtopt_config` module.
Color settings are shared from ``[global]``; tool-specific settings
(compressor, extra_args) live in ``[gtopt_compress_lp]``.

Config keys
-----------
compressor
    Which external program to use for compression.  May be a bare name
    (resolved via PATH) or an absolute path.  Special value ``"auto"``
    (default) picks the first available tool from the recommended list.
    Other recognised values: ``"gzip"``, ``"zstd"``, ``"lz4"``, ``"bzip2"``,
    ``"xz"``, ``"lzma"``.
extra_args
    Additional command-line arguments inserted before the file name when the
    compressor is invoked.  Empty by default.
"""

from __future__ import annotations

import configparser
import shutil
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

_SECTION = "gtopt_compress_lp"
_OLD_CONFIG_PATH = Path.home() / ".gtopt_compress_lp.conf"

# Tool-specific keys and their defaults
_TOOL_DEFAULTS: dict[str, str] = {
    "compressor": "auto",
    "extra_args": "",
}

# Keys that are read from [global]
_GLOBAL_KEYS = ("color",)

# ---------------------------------------------------------------------------
# Compression tool catalogue
# ---------------------------------------------------------------------------

# Each entry: (binary, label, recommended_speed, apt_hint, notes)
# recommended_speed: "very fast", "fast", "moderate", "slow"
_COMPRESSOR_CATALOGUE: list[tuple[str, str, str, str | None, str]] = [
    (
        "zstd",
        "Zstandard (zstd)",
        "very fast",
        "sudo apt install zstd",
        "Excellent ratio and speed balance — recommended default.",
    ),
    (
        "lz4",
        "LZ4",
        "very fast",
        "sudo apt install lz4",
        "Best for low-latency debug file writing; lowest CPU overhead.",
    ),
    (
        "gzip",
        "gzip",
        "fast",
        "sudo apt install gzip",
        "Universally available; good ratio; slightly slower than zstd/lz4.",
    ),
    (
        "bzip2",
        "bzip2",
        "moderate",
        "sudo apt install bzip2",
        "Better ratio than gzip but significantly slower.",
    ),
    (
        "xz",
        "XZ / LZMA",
        "slow",
        "sudo apt install xz-utils",
        "Best compression ratio; not recommended for large LP files.",
    ),
]


def default_config_path() -> Path:
    """Return the unified config file path: ``~/.gtopt.conf``."""
    return DEFAULT_CONFIG_PATH


def _migrate_old_config() -> None:
    """Migrate settings from ``~/.gtopt_compress_lp.conf`` → ``~/.gtopt.conf``."""
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
    """Load gtopt_compress_lp settings from the unified config.

    Reads color from ``[global]`` and tool-specific settings from
    ``[gtopt_compress_lp]``.  Returns a flat dict.
    """
    if _OLD_CONFIG_PATH.exists() and not config_path.exists():
        _migrate_old_config()

    cfg_parser = _load_unified_config(config_path)

    cfg: dict[str, str] = {}

    # Global keys (color)
    for key in _GLOBAL_KEYS:
        cfg[key] = get_global(cfg_parser, key)

    # Tool-specific keys
    for key, default in _TOOL_DEFAULTS.items():
        cfg[key] = get_section(cfg_parser, _SECTION, key, fallback=default)

    return cfg


def save_config(config_path: Path, cfg: dict[str, str]) -> None:
    """Write *cfg* to the unified config file.

    Global keys (color) go to ``[global]``; the rest to
    ``[gtopt_compress_lp]``.
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
# Compressor availability helpers
# ---------------------------------------------------------------------------


def get_compressor_status() -> list[tuple[str, str, str, bool, str | None, str]]:
    """Return installation status for every known compressor.

    Each entry is ``(binary, label, speed, installed, apt_hint, notes)``.
    """
    return [
        (binary, label, speed, shutil.which(binary) is not None, hint, notes)
        for binary, label, speed, hint, notes in _COMPRESSOR_CATALOGUE
    ]


def pick_auto_compressor() -> str | None:
    """Return the first available compressor binary name, or None."""
    for binary, _label, _speed, installed, _hint, _notes in get_compressor_status():
        if installed:
            return binary
    return None


def print_compressor_status(use_color: bool = True) -> None:
    """Print a table of available and missing compression tools."""

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{col._RESET}" if use_color else text  # noqa: SLF001

    print(_c(col._BOLD, "\nCompression tool availability:"))  # noqa: SLF001
    print()
    any_missing = False

    for _binary, label, speed, installed, _hint, notes in get_compressor_status():
        speed_tag = f"[{speed}]"
        if installed:
            mark = _c(col._GREEN, "✓")  # noqa: SLF001
            print(f"  {mark} {label:28s}  {_c(col._CYAN, speed_tag):16s}  {notes}")  # noqa: SLF001
        else:
            mark = _c(col._RED, "✗")  # noqa: SLF001
            print(
                f"  {mark} {label:28s}  {_c(col._YELLOW, speed_tag):16s}  "  # noqa: SLF001
                f"(not found on PATH)  {notes}"
            )
            any_missing = True

    if any_missing:
        print()
        print(_c(col._YELLOW, "  To install missing tools:"))  # noqa: SLF001
        for _b, label, _speed, installed, hint, _notes in get_compressor_status():
            if not installed and hint:
                print(f"    {hint:40s}   # {label}")

    print()
    _green = col._GREEN  # noqa: SLF001
    _cyan = col._CYAN  # noqa: SLF001
    _rec1 = _c(_green, "1st choice")
    _rec2 = _c(_green, "2nd choice")
    _rec3 = _c(_cyan, "3rd choice")
    print(
        "  Recommended for LP files  (text, highly compressible):\n"
        f"    {_rec1}: zstd  — best speed/ratio balance for debug files\n"
        f"    {_rec2}: lz4   — fastest writes, larger files\n"
        f"    {_rec3}: gzip  — universally available fallback"
    )


# ---------------------------------------------------------------------------
# Interactive first-run setup
# ---------------------------------------------------------------------------


def _prompt(prompt_text: str, default: str = "") -> str:
    """Prompt for input with an optional default."""
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

    Checks tool availability, recommends the best available compressor,
    prompts the user for preferences, and saves the config to the
    unified ``~/.gtopt.conf``.

    Returns the final config dict.
    """

    def _c(code: str, text: str) -> str:
        return f"{code}{text}{col._RESET}" if use_color else text  # noqa: SLF001

    print()
    print(_c(col._BOLD, "─── gtopt_compress_lp first-run setup ───"))  # noqa: SLF001
    print(
        "\nThis tool compresses LP debug files produced by gtopt."
        "\nLet's configure the compression tool for your system."
    )

    cfg = load_config(config_path)

    # ── Show compression tools ─────────────────────────────────────────────
    print_compressor_status(use_color=use_color)

    # ── Compressor selection ───────────────────────────────────────────────
    auto = pick_auto_compressor()
    current = cfg.get("compressor", "auto")
    if current == "auto":
        proposed = auto or "auto"
    else:
        proposed = current

    if auto:
        print(
            f"  Best available tool: {_c(col._GREEN, auto)}\n"  # noqa: SLF001
        )

    valid_choices = ["auto"] + [b for b, *_ in _COMPRESSOR_CATALOGUE]
    compressor = _prompt(
        f"  Compressor to use {_c(col._CYAN, str(valid_choices))}",  # noqa: SLF001
        proposed,
    )
    if compressor not in valid_choices and shutil.which(compressor) is None:
        print(
            f"  '{compressor}' not found on PATH, falling back to 'auto'",
            file=sys.stderr,
        )
        compressor = "auto"
    cfg["compressor"] = compressor

    # ── Extra args ─────────────────────────────────────────────────────────
    current_extra = cfg.get("extra_args", "")
    extra = _prompt(
        "  Extra arguments for compressor (leave blank for none)", current_extra
    )
    cfg["extra_args"] = extra

    # ── Color (shared in [global]) ────────────────────────────────────────
    current_color = cfg.get("color", "auto")
    color = _prompt("  Color output ('auto', 'always', 'never')", current_color)
    if color not in ("auto", "always", "never"):
        color = "auto"
    cfg["color"] = color

    # ── Save ───────────────────────────────────────────────────────────────
    save_config(config_path, cfg)
    print()
    print(f"  {_c(col._GREEN, '✓')} Configuration saved to: {config_path}")  # noqa: SLF001
    print()
    return cfg
