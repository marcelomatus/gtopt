# SPDX-License-Identifier: BSD-3-Clause
"""Shared configuration for all gtopt scripts.

Reads a unified ``~/.gtopt.conf`` INI file with a ``[global]`` section
for AI settings and per-tool sections (``[gtopt_check_json]``,
``[gtopt_check_lp]``, ``[gtopt_check_output]``, ``[run_gtopt]``, etc.).

Usage::

    from gtopt_config import load_config, get_global, get_section

    cfg = load_config()                    # reads ~/.gtopt.conf
    ai_provider = get_global(cfg, "ai_provider")   # from [global]
    check_enabled = get_section(cfg, "gtopt_check_json", "check_uid_uniqueness")
"""

from ._config import (
    DEFAULT_CONFIG_PATH,
    get_global,
    get_section,
    load_config,
    save_section,
)

__all__ = [
    "DEFAULT_CONFIG_PATH",
    "get_global",
    "get_section",
    "load_config",
    "save_section",
]
