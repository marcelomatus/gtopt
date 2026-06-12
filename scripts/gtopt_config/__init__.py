# SPDX-License-Identifier: BSD-3-Clause
"""Shared configuration and CLI helpers for all gtopt scripts.

Reads a unified ``~/.gtopt.conf`` INI file with a ``[global]`` section
for AI settings and per-tool sections (``[gtopt_check_json]``,
``[gtopt_check_lp]``, ``[gtopt_check_output]``, ``[run_gtopt]``, etc.).

Usage::

    from gtopt_config import load_config, get_global, get_section
    from gtopt_config import add_common_arguments, configure_logging

    cfg = load_config()                    # reads ~/.gtopt.conf
    ai_provider = get_global(cfg, "ai_provider")   # from [global]
    check_enabled = get_section(cfg, "gtopt_check_json", "check_uid_uniqueness")

    parser = argparse.ArgumentParser(...)
    add_common_arguments(parser)           # -V, -l, --no-color
    args = parser.parse_args()
    configure_logging(args)
"""

from ._cli import (
    add_color_argument,
    add_common_arguments,
    add_log_level_argument,
    add_version_argument,
    configure_logging,
    get_version,
    resolve_color,
)
from ._config import (
    DEFAULT_CONFIG_PATH,
    get_global,
    get_section,
    load_config,
    save_section,
)

__all__ = [
    "DEFAULT_CONFIG_PATH",
    "add_color_argument",
    "add_common_arguments",
    "add_log_level_argument",
    "add_version_argument",
    "configure_logging",
    "get_global",
    "get_section",
    "get_version",
    "load_config",
    "resolve_color",
    "save_section",
]
