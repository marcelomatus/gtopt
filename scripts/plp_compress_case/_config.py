# SPDX-License-Identifier: BSD-3-Clause
"""Configuration for plp_compress_case.

Uses the centralized ``~/.gtopt.conf`` via the :mod:`gtopt_config` module.
Color settings are shared from ``[global]``; tool-specific settings
(codec, codec_args, split_mb, xz_level) live in ``[plp_compress_case]``.

Config keys
-----------
codec
    Compression program to use.  Default ``"xz"``.
codec_args
    Extra arguments for the codec command.  Default ``"-T0"``
    (multi-threaded xz).
split_mb
    Compressed files larger than this (in MB) are split into numbered
    parts.  Default ``"10"``.
"""

from __future__ import annotations

from pathlib import Path

from gtopt_config import (
    DEFAULT_CONFIG_PATH,
    get_global,
    get_section,
    load_config as _load_unified_config,
    save_section,
)

_SECTION = "plp_compress_case"

_TOOL_DEFAULTS: dict[str, str] = {
    "codec": "xz",
    "codec_args": "-T0",
    "split_mb": "10",
}

_GLOBAL_KEYS = ("color",)


def default_config_path() -> Path:
    """Return the unified config file path: ``~/.gtopt.conf``."""
    return DEFAULT_CONFIG_PATH


def load_config(config_path: Path | None = None) -> dict[str, str]:
    """Load plp_compress_case settings from the unified config.

    Reads color from ``[global]`` and tool-specific settings from
    ``[plp_compress_case]``.  Returns a flat dict.
    """
    cfg_parser = _load_unified_config(config_path or DEFAULT_CONFIG_PATH)

    cfg: dict[str, str] = {}

    for key in _GLOBAL_KEYS:
        cfg[key] = get_global(cfg_parser, key)

    for key, default in _TOOL_DEFAULTS.items():
        cfg[key] = get_section(cfg_parser, _SECTION, key, fallback=default)

    return cfg


def save_config(config_path: Path, cfg: dict[str, str]) -> None:
    """Write *cfg* to the unified config file.

    Global keys (color) go to ``[global]``; the rest to
    ``[plp_compress_case]``.
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
