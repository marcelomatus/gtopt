# SPDX-License-Identifier: BSD-3-Clause
"""Surface-pin test for :class:`gtopt_diagram.TopologyBuilder`.

Refactor safety net for the upcoming split into mixin modules
(``_topology_ids``, ``_topology_network``, ``_topology_hydro``).  The
current ``TopologyBuilder`` is a single 1100-LoC class; after the split
its public API will be inherited from multiple bases.  Python's MRO
will silently mask a method if a mixin is reordered or a name is
typoed; this test enumerates every method present today and asserts it
remains callable through the class.

If a method is intentionally renamed or removed, update
``EXPECTED_METHODS`` in the same commit.
"""

from __future__ import annotations

import pytest

from gtopt_diagram import TopologyBuilder

# Every method present on ``TopologyBuilder`` pre-split.  Grouped by
# the domain the upcoming mixin will own; ordering is documentation
# only.
EXPECTED_METHODS: tuple[str, ...] = (
    # Construction + properties
    "__init__",
    "eff_agg",
    "eff_vthresh",
    "auto_info",
    # Element-id helpers (-> _topology_ids mixin)
    "_make_id",
    "_bid",
    "_gid",
    "_did",
    "_batid",
    "_cid",
    "_jid",
    "_rid",
    "_tid",
    "_fid",
    "_filtid",
    "_rzid",
    "_rpid",
    "_gpid",
    "_dpid",
    "_vrid",
    "_frid",
    "_pid",
    "_lngid",
    # Lookup helpers
    "_resolve_field",
    "_find",
    "_find_node_id",
    "_bus_node_id",
    "_gen_kind",
    "_count_elements",
    # Top-level orchestration
    "build",
    "_compute_focus_set",
    # Network domain (-> _topology_network mixin)
    "_buses",
    "_generators",
    "_gen_individual",
    "_gen_agg_bus",
    "_gen_agg_type",
    "_gen_agg_global",
    "_demands",
    "_lines",
    "_batteries",
    "_converters",
    "_generator_profiles",
    "_demand_profiles",
    # Hydro domain (-> _topology_hydro mixin)
    "_junctions",
    "_waterways",
    "_reservoirs",
    "_turbines",
    "_flows",
    "_seepages",
    "_volume_rights",
    "_flow_rights",
    "_reservoir_efficiencies",
    "_pumps",
    "_lng_terminals",
    "_reserve_zones",
    "_reserve_provisions",
)


@pytest.mark.parametrize("method_name", EXPECTED_METHODS)
def test_topology_builder_has_method(method_name: str) -> None:
    """Each entry in ``EXPECTED_METHODS`` must resolve on ``TopologyBuilder``.

    Mixin reorderings or typos during the split would drop one of these
    names; this catches it before the SVG renderer's first run.
    """
    assert hasattr(TopologyBuilder, method_name), (
        f"TopologyBuilder is missing method '{method_name}'.  "
        "If renamed/removed intentionally, update EXPECTED_METHODS in "
        "tests/test_topology_builder_surface.py to match."
    )


def test_topology_builder_method_count_lower_bound() -> None:
    """Mass-deletion guard: every listed method must be reachable."""
    actual_names = {
        name
        for name in dir(TopologyBuilder)
        if not name.startswith("__") or name == "__init__"
    }
    missing = set(EXPECTED_METHODS) - actual_names
    assert not missing, f"TopologyBuilder dropped methods: {sorted(missing)}"


def test_topology_builder_mro_pin() -> None:
    """Document the expected MRO so any base reordering surfaces in diff.

    Pre-refactor, ``TopologyBuilder`` has only ``object`` as base.
    After the split this will list every mixin in resolution order;
    update this assertion in the same commit as the split.
    """
    mro_names = [cls.__name__ for cls in TopologyBuilder.__mro__]
    assert mro_names[0] == "TopologyBuilder"
    assert mro_names[-1] == "object"
    assert len(mro_names) >= 2
