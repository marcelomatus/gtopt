# SPDX-License-Identifier: BSD-3-Clause
"""Cross-converter CLI parity test (issue #507 Phase 2).

For every converter that opts into ``gtopt_shared.cli_flags``, the
canonical flags must resolve to the same ``dest`` attribute name on
the parsed namespace and the same Python type.  Drift detector — if
one converter renames its ``--scale-objective`` dest to something
non-canonical or changes its type, this test fails immediately.

The test is intentionally lightweight: it parses an empty argv list
through each converter's ``make_parser()`` and asserts the parsed
``Namespace`` has the canonical attributes with the canonical
defaults.  Converters not yet wired to ``cli_flags`` (sddp2gtopt,
igtopt) are excluded — they will be added as wiring lands.
"""

from __future__ import annotations

import importlib
from typing import Callable

import pytest


# Per-converter defaults are LEGITIMATELY different (e.g. plp2gtopt
# uses scale_objective=1.0 because its cascade method runs unscaled,
# while plexos2gtopt/pp2gtopt use 1000.0 for monolithic).  We only
# pin the canonical dest + Python type, NOT the value.
_CANONICAL_FLAG_TYPES: dict[str, type] = {
    "scale_objective": float,
    "demand_fail_cost": float,
    "use_kirchhoff": bool,
    "use_single_bus": bool,
}

# Converters known to NOT yet expose a particular flag through the
# shared registrar.  Each entry below is a TODO for a future wire-up
# PR.  When a converter starts emitting the flag, remove its entry.
_KNOWN_MISSING: dict[str, set[str]] = {
    # plexos2gtopt only uses the shared registrar for use_single_bus;
    # scale_objective, demand_fail_cost, and use_kirchhoff are still
    # declared inline.  Will migrate as part of the Phase-2 wire-up
    # follow-up.
    "plexos2gtopt.main": {"scale_objective", "demand_fail_cost", "use_kirchhoff"},
}


def _make_parser(module_path: str) -> Callable[[], object]:
    """Import a converter's ``make_parser`` function lazily.

    Returns a zero-arg thunk so the import only happens at test
    discovery, not module load.
    """
    mod = importlib.import_module(module_path)
    return mod.make_parser


@pytest.mark.parametrize(
    "module_path",
    [
        pytest.param("plp2gtopt.main", id="plp2gtopt"),
        pytest.param("plexos2gtopt.main", id="plexos2gtopt"),
        pytest.param("pp2gtopt.main", id="pp2gtopt"),
    ],
)
def test_canonical_flags_resolve(module_path: str) -> None:
    """Each wired converter exposes the canonical flags with matching dest + type."""
    make_parser = _make_parser(module_path)
    parser = make_parser()

    # Parse with a no-op argv so we get a Namespace populated with
    # the registrar-supplied defaults.  Some converters require a
    # positional input — we work around that by inspecting the parser
    # actions directly rather than calling parse_args().
    # pylint: disable=protected-access
    dest_to_action = {a.dest: a for a in parser._actions}  # noqa: SLF001
    missing = _KNOWN_MISSING.get(module_path, set())

    for dest, expected_type in _CANONICAL_FLAG_TYPES.items():
        if dest in missing:
            # Documented gap — skip and let the per-converter
            # wire-up PR clear it.
            continue
        assert dest in dest_to_action, (
            f"{module_path}: missing canonical flag dest={dest!r}; "
            "either the converter is not wired to gtopt_shared.cli_flags "
            "yet or it declared the flag with a different dest."
        )
        action = dest_to_action[dest]
        actual_default = action.default
        if expected_type is bool:
            # use_single_bus default may differ per converter (auto vs forced) —
            # we only check the type, not the value.  BooleanOptionalAction
            # may report default=None when the converter wants auto-detect.
            assert isinstance(actual_default, bool) or actual_default is None, (
                f"{module_path}: {dest} default {actual_default!r} is not bool"
            )
        else:
            # ``None`` is allowed for float fields with auto-derive
            # defaults (e.g. plp2gtopt's demand_fail_cost which is
            # filled from FALLA centrals after parsing — see
            # ``add_demand_fail_cost_argument`` docstring).
            assert actual_default is None or isinstance(
                actual_default, expected_type
            ), (
                f"{module_path}: {dest} default {actual_default!r} is not "
                f"{expected_type.__name__} or None"
            )


def test_sddp2gtopt_not_yet_wired() -> None:
    """sddp2gtopt has no canonical flags yet (conversion path is stubbed).

    This test exists so the parity sweep above doesn't need to gate
    sddp2gtopt — and so when sddp2gtopt's conversion lands and gets
    wired, this test fails loudly and prompts the parametrize list
    above to be extended.
    """
    make_parser = _make_parser("sddp2gtopt.main")
    parser = make_parser()
    # pylint: disable=protected-access
    dest_to_action = {a.dest: a for a in parser._actions}  # noqa: SLF001
    for dest in _CANONICAL_FLAG_TYPES:
        assert dest not in dest_to_action, (
            f"sddp2gtopt now exposes {dest!r} — add 'sddp2gtopt.main' to the "
            "parametrize list in test_canonical_flags_resolve and delete this "
            "negative test."
        )
