"""Tests for tools/migrate_legacy_options_to_model_options.py.

Targets the std::format brace-escape helpers added in §11 to make
the migrator safe against raw-string literals embedded inside
`std::format(R"(...)" , …)` calls — the tricky path that initially
shipped broken (output unescaped braces, then the format string
became invalid C++).

Run:  cd tools && python -m pytest test_migrate_legacy_options.py -q
"""

from __future__ import annotations

import json
import pathlib
import sys

# Import the migrator script under test (sits next to this file).
sys.path.insert(0, str(pathlib.Path(__file__).parent))

# pylint: disable=wrong-import-position
import migrate_legacy_options_to_model_options as M  # noqa: E402


# ── fmt_body_to_json ────────────────────────────────────────────────────


def test_fmt_body_to_json_returns_none_when_no_doubled_braces() -> None:
    """Plain JSON should bypass the std::format unescape path so the
    caller can `json.loads` the body directly."""
    body = '{"a": 1, "b": [1, 2, 3]}'
    assert M.fmt_body_to_json(body) is None


def test_fmt_body_to_json_unescapes_doubled_braces() -> None:
    body = '{{"a": 1, "b": {{"c": 2}}}}'
    result = M.fmt_body_to_json(body)
    assert result is not None
    json_body, placeholders = result
    assert placeholders == []
    assert json.loads(json_body) == {"a": 1, "b": {"c": 2}}


def test_fmt_body_to_json_captures_positional_placeholders() -> None:
    """Bare `{}` and indexed `{N}` placeholders get replaced with
    quoted sentinel strings so the body becomes parseable JSON."""
    body = '{{"x": {}, "y": {0}, "z": {1:>3}}}'
    result = M.fmt_body_to_json(body)
    assert result is not None
    json_body, placeholders = result
    obj = json.loads(json_body)
    # Each placeholder becomes a quoted string and survives parsing.
    assert isinstance(obj["x"], str)
    assert isinstance(obj["y"], str)
    assert isinstance(obj["z"], str)
    # The placeholder texts are captured verbatim, in order.
    assert placeholders == ["{}", "{0}", "{1:>3}"]


# ── json_to_fmt_body ────────────────────────────────────────────────────


def test_json_to_fmt_body_escapes_braces_when_no_placeholders() -> None:
    """When the source was a std::format literal but contained no
    `{...}` placeholders, the output still needs `{{`/`}}` so the
    JSON object syntax doesn't trip std::basic_format_string."""
    json_str = '{"a": 1}'
    assert M.json_to_fmt_body(json_str, placeholders=[]) == '{{"a": 1}}'


def test_json_to_fmt_body_restores_placeholders_verbatim() -> None:
    result = M.fmt_body_to_json('{{"x": {}, "y": {0}}}')
    assert result is not None
    json_body, placeholders = result
    restored = M.json_to_fmt_body(json_body, placeholders)
    assert restored == '{{"x": {}, "y": {0}}}'


def test_fmt_round_trip_preserves_indexed_placeholders() -> None:
    """The round-trip is the property that matters in production: a
    body fed through fmt_body_to_json + json_to_fmt_body must come
    back byte-for-byte identical."""
    body = '{{"a": {0}, "b": {1:>5}, "nested": {{"v": {}}}}}'
    result = M.fmt_body_to_json(body)
    assert result is not None
    json_body, placeholders = result
    assert M.json_to_fmt_body(json_body, placeholders) == body


# ── End-to-end migration on a std::format-style literal ────────────────


def test_migrate_planning_options_under_fmt_escapes() -> None:
    """The full pipeline used by `process_cpp_file` on a real
    std::format snippet: unescape, migrate, re-escape.  Verifies the
    moved key (1) lives inside `model_options` and (2) keeps its
    positional `{}` placeholder."""
    src_body = (
        '{{"options": {{"demand_fail_cost": {}, "use_kirchhoff": true}},'
        '"system": {{"name": "x"}}}}'
    )
    result = M.fmt_body_to_json(src_body)
    assert result is not None
    json_body, placeholders = result
    obj = json.loads(json_body)
    migrated, moved = M.migrate_planning_options(obj)
    assert "demand_fail_cost" in moved
    assert "demand_fail_cost" not in migrated["options"]
    # Placeholder rides along inside the nested entry as a sentinel string.
    nested = migrated["options"]["model_options"]["demand_fail_cost"]
    assert isinstance(nested, str) and nested.startswith("__GTOPT_FMT_PLACEHOLDER_")
    rewritten = M.json_to_fmt_body(json.dumps(migrated), placeholders)
    # `{}` placeholder is restored verbatim under the new path.
    assert '"demand_fail_cost": {}' in rewritten
    # Outer braces are still doubled for std::format consumption.
    assert rewritten.startswith("{{")
    assert rewritten.endswith("}}")


def test_migrate_renames_reserve_fail_cost_to_reserve_shortage_cost() -> None:
    """§11.10 rename surfaces in the migrated dict regardless of the
    fmt-escape path."""
    obj = {"options": {"reserve_fail_cost": 500}}
    migrated, moved = M.migrate_planning_options(obj)
    assert moved == ["reserve_fail_cost"]
    assert "reserve_fail_cost" not in migrated["options"]
    assert migrated["options"]["model_options"]["reserve_shortage_cost"] == 500


def test_migrate_preserves_insertion_order_for_positional_placeholders() -> None:
    """Dropped the post-§11 reordering precisely because reordering
    silently misaligns `{}` placeholders with their format args.
    Lock that in: the order of moved keys inside `model_options`
    matches the order they appeared at the top level."""
    obj = {
        "options": {
            "scale_objective": 1000,  # 1st
            "scale_theta": 0.001,  # 2nd
            "use_kirchhoff": True,  # 3rd
        }
    }
    migrated, _ = M.migrate_planning_options(obj)
    assert list(migrated["options"]["model_options"].keys()) == [
        "scale_objective",
        "scale_theta",
        "use_kirchhoff",
    ]
