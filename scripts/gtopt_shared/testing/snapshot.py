# SPDX-License-Identifier: BSD-3-Clause
"""JSON snapshot helpers for converter regression tests (issue #507).

Centralises the ``_canonicalise`` / ``_assert_snapshot`` /
``PYTEST_UPDATE_GOLDEN`` dance that was previously copy-pasted across
the 8+ converter test files landed in S0.  One implementation, one
refresh procedure, one ``PYTEST_UPDATE_GOLDEN`` env var.

Two assertion helpers:

* :func:`assert_snapshot` — pass a Python ``object`` (dict / list /
  any json-serialisable payload); it gets canonicalised and compared
  against ``<golden_dir>/<name>.json``.
* :func:`assert_golden_file` — pass a file path the converter just
  wrote; the file's JSON content is canonicalised and compared.

Both support refresh via ``PYTEST_UPDATE_GOLDEN=1`` and skip with a
helpful message when the golden file is absent.

Usage::

    from gtopt_shared.testing import assert_snapshot

    _GOLDEN_DIR = Path(__file__).parent / "fixtures" / "entities"

    def test_build_bus_array_snapshot() -> None:
        nodes = (NodeSpec(object_id=1, name="A"),)
        assert_snapshot("build_bus_array", build_bus_array(nodes), _GOLDEN_DIR)
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Callable

import pytest


_REFRESH_ENV_VAR = "PYTEST_UPDATE_GOLDEN"
_REFRESH_CMD_HINT = f"{_REFRESH_ENV_VAR}=1 python -m pytest <test_module> -q"


def canonicalise_json(payload: Any) -> str:
    """Return the canonical text representation used for snapshot equality.

    Keys are sorted, indent is 2 spaces, ``ensure_ascii=False`` (UTF-8
    safe), and a trailing newline is appended so diff tools render the
    last line correctly.
    """
    return json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


def _read_json_file(path: Path) -> Any:
    """Load JSON from ``path`` — separate function so test scrub hooks can
    monkeypatch the read."""
    return json.loads(path.read_text(encoding="utf-8"))


def _do_compare(
    *,
    name: str,
    canonical: str,
    golden_dir: Path,
    refresh_target: str | None,
) -> None:
    """Shared compare-or-refresh path used by both helpers."""
    golden_path = golden_dir / f"{name}.json"
    target = refresh_target or "<test_module>"

    if os.environ.get(_REFRESH_ENV_VAR):
        golden_dir.mkdir(parents=True, exist_ok=True)
        golden_path.write_text(canonical, encoding="utf-8")
        pytest.skip(
            f"golden fixture written to {golden_path}; re-run without "
            f"{_REFRESH_ENV_VAR} to verify"
        )

    if not golden_path.exists():
        pytest.skip(
            f"golden fixture missing: {golden_path}; create with "
            f"{_REFRESH_ENV_VAR}=1 python -m pytest {target} -q"
        )

    expected = golden_path.read_text(encoding="utf-8")
    assert canonical == expected, (
        f"{name} entity output changed; if intentional, refresh with "
        f"{_REFRESH_ENV_VAR}=1 python -m pytest {target} -q"
    )


def assert_snapshot(
    name: str,
    payload: object,
    golden_dir: Path,
    *,
    refresh_target: str | None = None,
) -> None:
    """Assert ``payload`` matches the named JSON golden under ``golden_dir``.

    Refresh with ``PYTEST_UPDATE_GOLDEN=1``.  Skips the test when the
    golden file is missing (with a clear message instructing the
    refresh).

    Args:
        name: Stem of the golden file (``<name>.json``).
        payload: Any json-serialisable object.
        golden_dir: Directory the golden file lives in.  Created on
            refresh if missing.
        refresh_target: Optional pytest invocation hint to include in
            the refresh / skip messages.  When ``None``, falls back to
            ``"<test_module>"``.
    """
    canonical = canonicalise_json(payload)
    _do_compare(
        name=name,
        canonical=canonical,
        golden_dir=golden_dir,
        refresh_target=refresh_target,
    )


def assert_golden_file(
    name: str,
    file_path: Path,
    golden_dir: Path,
    *,
    refresh_target: str | None = None,
    scrub: Callable[[Any], Any] | None = None,
) -> None:
    """Assert the JSON file at ``file_path`` matches the named golden.

    Args:
        name: Stem of the golden file (``<name>.json``).
        file_path: Path the converter just wrote.  The function reads
            its JSON content and canonicalises before comparing.
        golden_dir: Directory the golden file lives in.
        refresh_target: Optional pytest invocation hint to include in
            messages.
        scrub: Optional callable that takes the parsed JSON dict and
            returns a mutated copy with non-deterministic values
            replaced (e.g. igtopt embeds the tmp_path in
            ``options.input_directory``).  Called BEFORE canonicalisation.
    """
    assert file_path.exists(), f"converter did not produce {file_path}"
    data = _read_json_file(file_path)
    if scrub is not None:
        data = scrub(data)
    canonical = canonicalise_json(data)
    _do_compare(
        name=name,
        canonical=canonical,
        golden_dir=golden_dir,
        refresh_target=refresh_target,
    )
