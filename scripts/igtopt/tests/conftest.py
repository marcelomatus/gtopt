"""Pytest fixtures for igtopt integration tests.

The ``gtopt_bin`` fixture provides the path to the ``gtopt`` binary.
It is backed by ``tools/get_gtopt_binary.py``, which tries (in order):

1. ``GTOPT_BIN`` environment variable.
2. ``gtopt`` on ``PATH``.
3. Standard build-directory paths (``build/standalone/gtopt``, etc.).
4. ``/tmp/gtopt-ci-bin/gtopt`` (previously downloaded CI artifact).
5. Automatic download of the ``gtopt-binary-debug`` CI artifact via the
   ``gh`` CLI or ``GITHUB_TOKEN``.

See ``tools/get_gtopt_binary.py`` for full documentation and the
``--build`` option (build from source as a last resort).
"""

from __future__ import annotations

import pathlib
import sys

# Add tools/ to sys.path so ``get_gtopt_binary`` can be imported as a
# top-level module regardless of how pytest was invoked.
_TOOLS_DIR = pathlib.Path(__file__).resolve().parents[3] / "tools"
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from get_gtopt_binary import make_gtopt_bin_fixture  # noqa: E402  # pylint: disable=wrong-import-position

# Register the fixture so pytest can inject it into test functions.
gtopt_bin = make_gtopt_bin_fixture()
