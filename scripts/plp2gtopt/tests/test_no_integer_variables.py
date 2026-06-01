# SPDX-License-Identifier: BSD-3-Clause
"""PLP-fidelity invariant: plp2gtopt MUST NOT emit MIP / commitment artefacts.

PLP itself is a pure linear program: no integer variables, no unit
commitment, no startup/shutdown costs, no min-up/min-down. Anything
plp2gtopt emits should preserve that property.  This module locks down
the invariant by sweeping the converted planning JSON for every key
gtopt interprets as a MIP-inducing primitive.

If gtopt ever adds a new commitment-style field, extend
``_FORBIDDEN_SYSTEM_KEYS`` / ``_FORBIDDEN_ENTITY_FIELDS`` here so the
guard keeps biting.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Iterable

import pytest

from plp2gtopt.plp2gtopt import convert_plp_case

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"

# Top-level ``system.<key>`` entries that ONLY exist to carry MIP /
# integer-commitment primitives.  plp2gtopt must never produce these.
_FORBIDDEN_SYSTEM_KEYS = (
    "commitment_array",
    "simple_commitment_array",
)

# Per-entity fields that, if set to a truthy value, switch a column
# (or rolling cap accumulator) into integer / binary mode in gtopt.
# Maps the entity's array key → fields that must be absent (or False).
_FORBIDDEN_ENTITY_FIELDS = {
    "battery_array": ("commitment", "integer_expmod"),
    "converter_array": ("commitment",),
    "generator_array": ("commitment", "integer_expmod"),
}

# Commitment-only attributes that should never appear ANYWHERE in the
# emitted JSON (these only have meaning inside a ``Commitment`` primitive
# which plp2gtopt must not emit at all).
_FORBIDDEN_NESTED_KEYS = frozenset(
    {
        "startup_cost",
        "shutdown_cost",
        "max_starts",
        "min_starts",
        "max_starts_window",
        "min_uptime",
        "min_downtime",
    }
)


def _make_opts(input_dir: Path, tmp_path: Path, case_name: str) -> dict:
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": input_dir,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
        "layout": "wide",
    }


def _walk_dicts(obj: Any) -> Iterable[dict]:
    """Yield every dict found at any depth inside ``obj``."""
    if isinstance(obj, dict):
        yield obj
        for v in obj.values():
            yield from _walk_dicts(v)
    elif isinstance(obj, list):
        for item in obj:
            yield from _walk_dicts(item)


def _assert_no_mip_artefacts(data: dict, case_name: str) -> None:
    sys_ = data.get("system", {})

    # 1. No top-level commitment arrays.
    for key in _FORBIDDEN_SYSTEM_KEYS:
        assert key not in sys_, (
            f"{case_name}: system.{key} present in plp2gtopt output — "
            f"PLP is pure LP, no commitment primitives expected"
        )

    # 2. No per-entity commitment / integer_expmod flags set truthy.
    for array_key, forbidden in _FORBIDDEN_ENTITY_FIELDS.items():
        for entity in sys_.get(array_key, []) or []:
            for field in forbidden:
                value = entity.get(field)
                # An explicitly-emitted ``False`` is also undesirable
                # (means the converter touched the field at all).
                assert value is None or value is False, (
                    f"{case_name}: {array_key}[name={entity.get('name')!r}]."
                    f"{field}={value!r} — MIP-inducing flag emitted"
                )

    # 3. No commitment-attribute keys anywhere in the tree.
    for nested in _walk_dicts(data):
        offending = sorted(set(nested) & _FORBIDDEN_NESTED_KEYS)
        assert not offending, (
            f"{case_name}: commitment-only attribute(s) {offending} present "
            f"in emitted dict — PLP has no startup/shutdown/min-up/down/max-starts"
        )

    # 4. No "is_integer" : true marker anywhere (LP layer field that
    #    upstream JSON should never set).
    for nested in _walk_dicts(data):
        if nested.get("is_integer") is True:
            pytest.fail(f"{case_name}: dict {nested!r} carries is_integer=True")


@pytest.mark.integration
@pytest.mark.parametrize(
    "case_dirname",
    [
        "plp_min_1bus",
        "plp_min_2bus",
        "plp_min_battery",
        "plp_min_bess",
        "plp_min_ess",
        "plp_min_hydro",
        "plp_min_mance",
    ],
)
def test_no_mip_artefacts_in_default_conversion(
    case_dirname: str, tmp_path: Path
) -> None:
    """Converting each minimal case must not produce MIP artefacts."""
    input_dir = _CASES_DIR / case_dirname
    if not input_dir.exists():
        pytest.skip(f"Fixture {case_dirname} not present in this checkout")

    opts = _make_opts(input_dir, tmp_path, case_dirname)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    _assert_no_mip_artefacts(data, case_dirname)


@pytest.mark.integration
def test_no_user_constraint_array_without_expand_water_rights(tmp_path: Path) -> None:
    """Without --expand-water-rights, no inline UserConstraints are emitted.

    PLP has no native UCs; gtopt_expand (irrigation agreements) IS the
    only legitimate source of ``user_constraint_array`` in plp2gtopt
    output and it is opt-in via ``--expand-water-rights``.
    """
    input_dir = _CASES_DIR / "plp_min_1bus"
    if not input_dir.exists():
        pytest.skip("Fixture plp_min_1bus not present")

    opts = _make_opts(input_dir, tmp_path, "plp_min_1bus")
    # Be explicit: expand_water_rights defaults to False but pin it here
    # so the test is robust to future default flips.
    opts["expand_water_rights"] = False
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys_ = data.get("system", {})
    assert not sys_.get("user_constraint_array"), (
        "user_constraint_array emitted without --expand-water-rights — "
        "PLP has no UCs, plp2gtopt must not generate them in default mode"
    )
    assert not sys_.get("user_constraint_files"), (
        "user_constraint_files emitted without --expand-water-rights"
    )
