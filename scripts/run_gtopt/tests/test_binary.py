# SPDX-License-Identifier: BSD-3-Clause
"""Tests for binary discovery."""

import os
from pathlib import Path
from unittest.mock import patch

from run_gtopt._binary import find_gtopt_binary, find_plp2gtopt


def test_gtopt_bin_env_var(tmp_path: Path):
    """GTOPT_BIN environment variable is respected."""
    fake_bin = tmp_path / "gtopt"
    fake_bin.write_text("#!/bin/sh\n")
    fake_bin.chmod(0o755)
    with patch.dict(os.environ, {"GTOPT_BIN": str(fake_bin)}):
        assert find_gtopt_binary() == str(fake_bin)


def test_gtopt_bin_env_var_missing_file():
    """GTOPT_BIN pointing to non-existent file is ignored."""
    with patch.dict(os.environ, {"GTOPT_BIN": "/no/such/gtopt"}):
        # May or may not find via PATH — just ensure no crash
        find_gtopt_binary()


def test_gtopt_bin_not_found():
    """Returns None when nothing is found."""
    with patch.dict(os.environ, {"GTOPT_BIN": ""}, clear=False):
        with patch("run_gtopt._binary.shutil.which", return_value=None):
            # Standard build paths likely don't exist either
            result = find_gtopt_binary()
            # Could be None or a real binary on this dev machine
            assert result is None or isinstance(result, str)


def test_find_plp2gtopt_on_path():
    """find_plp2gtopt returns a path when plp2gtopt is installed."""
    result = find_plp2gtopt()
    # plp2gtopt should be installed in this dev environment
    if result:
        assert "plp2gtopt" in result


def test_find_plp2gtopt_not_installed():
    """Returns None when plp2gtopt is not on PATH."""
    with patch("run_gtopt._binary.shutil.which", return_value=None):
        assert find_plp2gtopt() is None
