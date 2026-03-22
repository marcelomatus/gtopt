# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the shared configuration module."""

from pathlib import Path

from gtopt_config import get_global, get_section, load_config, save_section


def test_load_defaults():
    """Loading a non-existent file returns defaults."""
    cfg = load_config(Path("/tmp/nonexistent_gtopt_test.conf"))
    assert get_global(cfg, "ai_provider") == "claude"
    assert get_global(cfg, "ai_enabled") == "false"


def test_load_and_save(tmp_path: Path):
    """Round-trip: save a section, reload, read it back."""
    conf = tmp_path / ".gtopt.conf"
    save_section(conf, "my_tool", {"check_foo": "true", "threshold": "42"})
    cfg = load_config(conf)
    assert get_section(cfg, "my_tool", "check_foo") == "true"
    assert get_section(cfg, "my_tool", "threshold") == "42"


def test_global_preserved_after_save(tmp_path: Path):
    """Saving a tool section preserves the [global] section."""
    conf = tmp_path / ".gtopt.conf"
    save_section(conf, "tool_a", {"key": "val"})
    cfg = load_config(conf)
    assert get_global(cfg, "ai_provider") == "claude"


def test_missing_section_fallback():
    """get_section returns fallback for missing sections."""
    cfg = load_config(Path("/tmp/nonexistent_gtopt_test.conf"))
    assert get_section(cfg, "no_such_tool", "key", "default") == "default"
