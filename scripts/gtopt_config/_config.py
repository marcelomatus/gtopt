# SPDX-License-Identifier: BSD-3-Clause
"""Unified configuration file I/O for all gtopt scripts.

File format (INI)::

    [global]
    ai_enabled  = false
    ai_provider = claude
    ai_model    =

    [gtopt_check_json]
    check_uid_uniqueness = true
    ...

    [gtopt_check_output]
    check_load_shedding = true
    ...

    [plp2gtopt]
    compression       = zstd
    compression_level = 9
    output_format     = parquet
    input_format      = parquet
    solver_type       = sddp
    demand_fail_cost  = 1000.0
    scale_objective   = 1000.0
    discount_rate     = 0.0
    rsv_scale_mode    = auto

    [run_gtopt]
    default_threads = 0
    ...
"""

from __future__ import annotations

import configparser
import logging
from pathlib import Path

log = logging.getLogger(__name__)

DEFAULT_CONFIG_PATH = Path.home() / ".gtopt.conf"

# Supported AI providers (shared across all tools).
AI_PROVIDERS = ("claude", "openai", "deepseek", "github")

_GLOBAL_DEFAULTS: dict[str, str] = {
    "ai_enabled": "false",
    "ai_provider": "claude",
    "ai_model": "",
    "ai_prompt": "",
    "color": "auto",
}


def load_config(config_path: Path | None = None) -> configparser.ConfigParser:
    """Load the unified config file.

    Returns a :class:`ConfigParser` with at least a ``[global]`` section
    populated with defaults.  Missing file or parse errors are silently
    ignored (defaults are returned).
    """
    path = config_path or DEFAULT_CONFIG_PATH
    parser = configparser.ConfigParser()

    # Seed the [global] section with defaults
    parser["global"] = dict(_GLOBAL_DEFAULTS)

    if path.exists():
        try:
            parser.read(path, encoding="utf-8")
        except configparser.Error as exc:
            log.warning("error reading %s: %s", path, exc)

    # Ensure defaults are present even after reading
    for key, val in _GLOBAL_DEFAULTS.items():
        if not parser.has_option("global", key):
            parser.set("global", key, val)

    return parser


def get_global(cfg: configparser.ConfigParser, key: str, fallback: str = "") -> str:
    """Read a value from the ``[global]`` section."""
    return cfg.get("global", key, fallback=fallback)


def get_section(
    cfg: configparser.ConfigParser,
    section: str,
    key: str,
    fallback: str = "",
) -> str:
    """Read a value from a tool-specific section, falling back to *fallback*."""
    if cfg.has_section(section) and cfg.has_option(section, key):
        return cfg.get(section, key)
    return fallback


def save_section(
    config_path: Path,
    section: str,
    values: dict[str, str],
) -> None:
    """Merge *values* into *section* of the config file and write it back.

    Preserves all other sections and keys.
    """
    parser = load_config(config_path)
    if not parser.has_section(section):
        parser.add_section(section)
    for key, val in values.items():
        parser.set(section, key, val)

    try:
        config_path.parent.mkdir(parents=True, exist_ok=True)
        with config_path.open("w", encoding="utf-8") as fh:
            fh.write(
                "# gtopt unified configuration file\n"
                "# Shared by all gtopt scripts (run_gtopt, gtopt_check_json,\n"
                "# gtopt_check_lp, gtopt_check_output, etc.)\n"
                "#\n"
                "# [global] section contains AI and color settings.\n"
                "# Per-tool sections override or add tool-specific keys.\n\n"
            )
            parser.write(fh)
    except OSError as exc:
        log.warning("cannot write config %s: %s", config_path, exc)
