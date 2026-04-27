# SPDX-License-Identifier: BSD-3-Clause
"""Surface-pin test for ``GTOptWriter``.

This file exists as a refactor safety net for an upcoming split of
:mod:`plp2gtopt.gtopt_writer` into mixin classes (one per domain:
simulation, hydro, network, misc).  After the split, ``GTOptWriter``
will pick up its public API via multiple base classes.  Python's MRO
will silently mask a method if a mixin is reordered or a name is
typoed; this test enumerates every method that exists today and asserts
it remains callable through the class.

If a method is intentionally renamed or removed in a follow-up commit,
delete the corresponding entry from ``EXPECTED_METHODS`` along with the
change.
"""

from __future__ import annotations

import pytest

from plp2gtopt.gtopt_writer import GTOptWriter

# Public + private methods expected to remain on ``GTOptWriter`` itself
# (or on one of its bases) after the split.  Order is documentation only;
# the assertion is on membership.
EXPECTED_METHODS: tuple[str, ...] = (
    # Construction / dunder
    "__init__",
    # Helpers (will likely move into a private mixin)
    "_normalize_method",
    "_build_default_cascade_options",
    "_merge_entities",
    "_reservoir_names",
    "_falla_by_bus",
    "_load_alias_file",
    "_build_stage_to_phase_map",
    "_load_variable_scales_file",
    "_dump_ror_promoted",
    "_dump_water_rights_fragment",
    "_expand_laja",
    "_expand_maule",
    "_expand_lng",
    # Simulation domain
    "process_options",
    "process_stage_blocks",
    "process_scenarios",
    "process_apertures",
    "process_indhor",
    # Hydro domain
    "process_junctions",
    "process_water_rights",
    "process_lng",
    "process_pumped_storage",
    "process_afluents",
    "process_flow_turbines",
    "process_ror_spec",
    "classify_pasada_centrals",
    # Network domain
    "process_centrals",
    "process_demands",
    "process_buses",
    "process_lines",
    "process_battery",
    # Misc / output assembly
    "process_generator_profiles",
    "process_boundary_cuts",
    "process_variable_scales",
    "to_json",
    "write",
)


@pytest.mark.parametrize("method_name", EXPECTED_METHODS)
def test_gtopt_writer_has_method(method_name: str) -> None:
    """Every entry in ``EXPECTED_METHODS`` must resolve on ``GTOptWriter``.

    Mixin reorderings or accidental renames during the split would drop
    one of these names; this catches it before any plp2gtopt run fails.
    """
    assert hasattr(GTOptWriter, method_name), (
        f"GTOptWriter is missing public method '{method_name}'.  "
        "If this method was intentionally renamed/removed, update "
        "EXPECTED_METHODS in tests/test_writer_surface.py to match."
    )


def test_gtopt_writer_method_count_lower_bound() -> None:
    """Lower-bound check: ``GTOptWriter`` exposes at least the listed methods.

    Catches mass deletions where many methods disappear at once (e.g.
    a base class accidentally forgotten from the inheritance list).
    """
    actual_names = {
        name
        for name in dir(GTOptWriter)
        if not name.startswith("__") or name == "__init__"
    }
    missing = set(EXPECTED_METHODS) - actual_names
    assert not missing, f"GTOptWriter dropped methods: {sorted(missing)}"


def test_gtopt_writer_mro_pin() -> None:
    """Document the expected MRO so a base reordering surfaces in diff.

    Pre-refactor, ``GTOptWriter`` is a flat class with only ``object`` as
    base.  After the mixin split this will list every mixin in the
    chosen resolution order.  The expected length is updated in lockstep
    with the refactor; deviations are caught immediately.
    """
    mro_names = [cls.__name__ for cls in GTOptWriter.__mro__]
    assert mro_names[0] == "GTOptWriter"
    assert mro_names[-1] == "object"
    # Post-split chain: GTOptWriter → TimeMixin → GenerationMixin →
    # HydroMixin → NetworkMixin → BoundaryMixin → object (length 7).
    # Reordering or losing a mixin would surface here.
    assert mro_names == [
        "GTOptWriter",
        "TimeMixin",
        "GenerationMixin",
        "HydroMixin",
        "NetworkMixin",
        "BoundaryMixin",
        "object",
    ]
