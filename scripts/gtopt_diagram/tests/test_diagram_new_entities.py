# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_diagram – Turbine Mode B and newly-rendered entities.

Covers:
- Turbine Mode B with junction_b unset (terminal drain)
- Turbine Mode B with junction_b set (between-junctions cascade)
- Mode B priority over a stale ``waterway`` field
- Fuel nodes + Fuel → Generator edges
- Emission / EmissionZone / EmissionSource family
- DecisionVariable nodes
- UserConstraint nodes
- Commitment / SimpleCommitment overlays on generators
- CarrierConverter nodes wiring from_node → cconv → to_node
- New helper predicates in ``_classify``
"""

from __future__ import annotations

from gtopt_diagram import gtopt_diagram as gd
from gtopt_diagram._classify import (
    turbine_is_builtin_waterway,
    turbine_is_terminal_drain,
    turbine_waterway_refs,
)


def _build(planning, subsystem="full", aggregate="none", compact=False):
    fo = gd.FilterOptions(aggregate=aggregate, compact=compact)
    return gd.TopologyBuilder(planning, subsystem=subsystem, opts=fo).build()


def _pairs(model):
    return {(e.src, e.dst) for e in model.edges}


def _node_ids(model):
    return {n.node_id for n in model.nodes}


def _assert_no_dangling(model):
    ids = _node_ids(model)
    for e in model.edges:
        assert e.src in ids, f"Dangling edge src '{e.src}'"
        assert e.dst in ids, f"Dangling edge dst '{e.dst}'"


# ---------------------------------------------------------------------------
# _classify predicates
# ---------------------------------------------------------------------------


class TestTurbineModePredicates:
    """Verify the Mode-B classifier helpers."""

    def test_legacy_waterway_mode_is_not_builtin(self):
        t = {"uid": 1, "name": "T1", "waterway": 5, "generator": 1}
        assert not turbine_is_builtin_waterway(t)
        assert not turbine_is_terminal_drain(t)

    def test_flow_mode_is_not_builtin(self):
        t = {"uid": 1, "name": "T1", "flow": 5, "generator": 1}
        assert not turbine_is_builtin_waterway(t)
        assert not turbine_is_terminal_drain(t)

    def test_junctions_mode_b_terminal(self):
        t = {"uid": 1, "name": "T1", "junction_a": 5, "generator": 1}
        assert turbine_is_builtin_waterway(t)
        assert turbine_is_terminal_drain(t)

    def test_junctions_mode_b_cascade(self):
        t = {
            "uid": 1,
            "name": "T1",
            "junction_a": 5,
            "junction_b": 6,
            "generator": 1,
        }
        assert turbine_is_builtin_waterway(t)
        assert not turbine_is_terminal_drain(t)

    def test_flow_beats_junctions(self):
        """When both ``flow`` and ``junction_a`` are set, Mode B is suppressed."""
        t = {
            "uid": 1,
            "name": "T1",
            "flow": 9,
            "junction_a": 5,
            "junction_b": 6,
            "generator": 1,
        }
        assert not turbine_is_builtin_waterway(t)

    def test_turbine_waterway_refs_skips_mode_b(self):
        """Mode B turbines must not contribute to waterway-suppression set."""
        sys = {
            "turbine_array": [
                {"uid": 1, "name": "Legacy", "waterway": 10, "generator": 1},
                {
                    "uid": 2,
                    "name": "ModeB_with_stale_waterway",
                    "junction_a": 5,
                    "waterway": 99,
                    "generator": 2,
                },
            ]
        }
        refs = turbine_waterway_refs(sys)
        assert refs == {10}


# ---------------------------------------------------------------------------
# Mode B Turbine — terminal drain (junction_b unset)
# ---------------------------------------------------------------------------

_MODE_B_TERMINAL = {
    "system": {
        "name": "modeb_terminal",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "junction_array": [{"uid": 1, "name": "J_intake"}],
        "generator_array": [{"uid": 1, "name": "G_term", "bus": 1, "pmax": 50}],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T_term",
                "junction_a": 1,
                "generator": 1,
                "production_factor": 2.0,
                "capacity": 50,
            }
        ],
    }
}


class TestTurbineModeBTerminal:
    """Terminal-drain Mode B turbine (no junction_b)."""

    def test_turbine_node_exists(self):
        model = _build(_MODE_B_TERMINAL)
        assert "turb_T_term_1" in _node_ids(model)

    def test_intake_edge_to_turbine(self):
        """junction_a → turbine edge must exist."""
        model = _build(_MODE_B_TERMINAL)
        assert ("junc_J_intake_1", "turb_T_term_1") in _pairs(model)

    def test_turbine_uses_drain_kind_variant(self):
        """Terminal-drain turbine renders with ``turbine_drain`` kind
        (not a separate marker node)."""
        model = _build(_MODE_B_TERMINAL)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["turb_T_term_1"].kind == "turbine_drain"

    def test_no_separate_drain_marker(self):
        """No ``terminal_drain`` marker node should be emitted — the
        turbine_drain kind variant signals the drain itself."""
        model = _build(_MODE_B_TERMINAL)
        drains = [n for n in model.nodes if n.kind == "terminal_drain"]
        assert not drains
        # Also no dangling ``drain_*`` node id.
        assert not any(n.node_id.startswith("drain_") for n in model.nodes)

    def test_no_synthetic_ocean_junction(self):
        """No fake ocean junction is introduced."""
        model = _build(_MODE_B_TERMINAL)
        ids = _node_ids(model)
        junc_ids = {nid for nid in ids if nid.startswith("junc_")}
        assert junc_ids == {"junc_J_intake_1"}

    def test_turbine_to_generator_edge_still_drawn(self):
        model = _build(_MODE_B_TERMINAL)
        assert ("turb_T_term_1", "gen_G_term_1") in _pairs(model)

    def test_tooltip_flags_terminal_drain(self):
        """The turbine's tooltip names it as a terminal drain."""
        model = _build(_MODE_B_TERMINAL)
        by_id = {n.node_id: n for n in model.nodes}
        assert "terminal drain" in by_id["turb_T_term_1"].tooltip.lower()

    def test_no_dangling(self):
        _assert_no_dangling(_build(_MODE_B_TERMINAL))


# ---------------------------------------------------------------------------
# Mode B Turbine — cascade (junction_b set)
# ---------------------------------------------------------------------------

_MODE_B_CASCADE = {
    "system": {
        "name": "modeb_cascade",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "junction_array": [
            {"uid": 1, "name": "J_up"},
            {"uid": 2, "name": "J_dn"},
        ],
        "generator_array": [{"uid": 1, "name": "G_casc", "bus": 1, "pmax": 80}],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T_casc",
                "junction_a": 1,
                "junction_b": 2,
                "generator": 1,
                "production_factor": 1.5,
            }
        ],
    }
}


class TestTurbineModeBCascade:
    """Between-junctions cascade Mode B turbine."""

    def test_intake_edge(self):
        model = _build(_MODE_B_CASCADE)
        assert ("junc_J_up_1", "turb_T_casc_1") in _pairs(model)

    def test_tailrace_edge(self):
        """turbine → junction_b edge must exist."""
        model = _build(_MODE_B_CASCADE)
        assert ("turb_T_casc_1", "junc_J_dn_2") in _pairs(model)

    def test_no_terminal_drain_in_cascade(self):
        """A cascade turbine must NOT emit a terminal_drain marker."""
        model = _build(_MODE_B_CASCADE)
        drains = [n for n in model.nodes if n.kind == "terminal_drain"]
        assert not drains

    def test_turbine_to_generator(self):
        model = _build(_MODE_B_CASCADE)
        assert ("turb_T_casc_1", "gen_G_casc_1") in _pairs(model)

    def test_no_dangling(self):
        _assert_no_dangling(_build(_MODE_B_CASCADE))


# ---------------------------------------------------------------------------
# Mode B ignores stale waterway ref
# ---------------------------------------------------------------------------

_MODE_B_STALE_WATERWAY = {
    "system": {
        "name": "modeb_stale_ww",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "junction_array": [
            {"uid": 1, "name": "J_in"},
            {"uid": 2, "name": "J_other_a"},
            {"uid": 3, "name": "J_other_b"},
        ],
        "waterway_array": [
            {
                "uid": 99,
                "name": "W_stale",
                "junction_a": 2,
                "junction_b": 3,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 50}],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T_modeb",
                "junction_a": 1,
                "waterway": 99,  # stale — must be ignored
                "generator": 1,
                "production_factor": 1.0,
            }
        ],
    }
}


class TestModeBStaleWaterway:
    """Mode B must ignore any leftover ``waterway`` ref."""

    def test_stale_waterway_arc_is_drawn(self):
        """The stale Waterway is not suppressed (it's an independent arc)."""
        model = _build(_MODE_B_STALE_WATERWAY)
        # Mode B → its junction_a uses no Waterway, so the stale W_stale
        # arc J_other_a → J_other_b is NOT suppressed.
        assert ("junc_J_other_a_2", "junc_J_other_b_3") in _pairs(model)

    def test_turbine_intake_uses_junction_a_not_waterway(self):
        model = _build(_MODE_B_STALE_WATERWAY)
        # Edge to T_modeb comes from junction_a (J_in), not from the
        # stale waterway endpoints.
        assert ("junc_J_in_1", "turb_T_modeb_1") in _pairs(model)


# ---------------------------------------------------------------------------
# Fuel
# ---------------------------------------------------------------------------

_FUEL_PLANNING = {
    "system": {
        "name": "fuel_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G_gas", "bus": 1, "pmax": 100, "fuel": "ng"},
            {"uid": 2, "name": "G_coal", "bus": 1, "pmax": 50, "fuel": "co"},
        ],
        "fuel_array": [
            {"uid": 1, "name": "ng", "price": 4.5, "heat_content": 1.0},
            {"uid": 2, "name": "co", "price": 2.0, "heat_content": 7.0},
        ],
    }
}


class TestFuelDiagram:
    def test_fuel_nodes_created(self):
        model = _build(_FUEL_PLANNING)
        kinds = [n.kind for n in model.nodes]
        assert kinds.count("fuel") == 2

    def test_fuel_node_ids(self):
        model = _build(_FUEL_PLANNING)
        assert {"fuel_ng_1", "fuel_co_2"}.issubset(_node_ids(model))

    def test_fuel_to_generator_edges(self):
        model = _build(_FUEL_PLANNING)
        pairs = _pairs(model)
        assert ("fuel_ng_1", "gen_G_gas_1") in pairs
        assert ("fuel_co_2", "gen_G_coal_2") in pairs

    def test_fuel_label_has_price(self):
        model = _build(_FUEL_PLANNING)
        ng = next(n for n in model.nodes if n.node_id == "fuel_ng_1")
        assert "4.5" in ng.label or "price" in ng.label

    def test_no_fuel_array_skips_method(self):
        model = _build({"system": {"name": "no_fuel"}})
        assert not [n for n in model.nodes if n.kind == "fuel"]

    def test_no_dangling(self):
        _assert_no_dangling(_build(_FUEL_PLANNING))


# ---------------------------------------------------------------------------
# Emission family
# ---------------------------------------------------------------------------

_EMISSION_PLANNING = {
    "system": {
        "name": "emission_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G_th", "bus": 1, "pmax": 100},
        ],
        "emission_array": [
            {"uid": 1, "name": "co2"},
        ],
        "emission_zone_array": [
            {
                "uid": 1,
                "name": "global_co2",
                "emissions": [{"emission": 1, "weight": 1.0}],
                "cap": 1000,
            }
        ],
        "emission_source_array": [
            {
                "uid": 1,
                "name": "G_th_co2",
                "generator": 1,
                "zone": 1,
                "emission": 1,
                "rate": 0.45,
            }
        ],
    }
}


class TestEmissionDiagram:
    def test_emission_node_created(self):
        model = _build(_EMISSION_PLANNING)
        assert "em_co2_1" in _node_ids(model)

    def test_emission_zone_node_created(self):
        model = _build(_EMISSION_PLANNING)
        assert "emz_global_co2_1" in _node_ids(model)

    def test_emission_source_node_created(self):
        model = _build(_EMISSION_PLANNING)
        assert "emsrc_G_th_co2_1" in _node_ids(model)

    def test_zone_covers_emission_edge(self):
        model = _build(_EMISSION_PLANNING)
        assert ("emz_global_co2_1", "em_co2_1") in _pairs(model)

    def test_generator_to_source_edge(self):
        model = _build(_EMISSION_PLANNING)
        assert ("gen_G_th_1", "emsrc_G_th_co2_1") in _pairs(model)

    def test_source_to_zone_edge(self):
        model = _build(_EMISSION_PLANNING)
        assert ("emsrc_G_th_co2_1", "emz_global_co2_1") in _pairs(model)

    def test_zone_label_has_cap(self):
        model = _build(_EMISSION_PLANNING)
        ez = next(n for n in model.nodes if n.node_id == "emz_global_co2_1")
        assert "1000" in ez.label or "cap" in ez.label

    def test_no_dangling(self):
        _assert_no_dangling(_build(_EMISSION_PLANNING))


# ---------------------------------------------------------------------------
# DecisionVariable
# ---------------------------------------------------------------------------

_DV_PLANNING = {
    "system": {
        "name": "dv_test",
        "decision_variable_array": [
            {
                "uid": 1,
                "name": "alpha_fcf",
                "lower_bound": 0,
                "cost": 1.0,
                "cost_type": "raw",
            },
            {"uid": 2, "name": "free_dv"},
        ],
    }
}


class TestDecisionVariableDiagram:
    def test_dv_nodes_created(self):
        model = _build(_DV_PLANNING, subsystem="electrical")
        kinds = [n.kind for n in model.nodes]
        assert kinds.count("decision_variable") == 2

    def test_dv_node_ids(self):
        model = _build(_DV_PLANNING, subsystem="electrical")
        assert {"dv_alpha_fcf_1", "dv_free_dv_2"}.issubset(_node_ids(model))

    def test_dv_label_has_cost_type(self):
        model = _build(_DV_PLANNING, subsystem="electrical")
        a = next(n for n in model.nodes if n.node_id == "dv_alpha_fcf_1")
        assert "raw" in a.label or "cost" in a.label

    def test_dv_compact_label(self):
        model = _build(_DV_PLANNING, subsystem="electrical", compact=True)
        a = next(n for n in model.nodes if n.node_id == "dv_alpha_fcf_1")
        assert "[DecVar]" not in a.label


# ---------------------------------------------------------------------------
# UserConstraint
# ---------------------------------------------------------------------------

_UC_PLANNING = {
    "system": {
        "name": "uc_test",
        "user_constraint_array": [
            {
                "uid": 1,
                "name": "energy_cap",
                "expression": "sum{b in B} gen(G1,b) <= 100",
                "penalty": 5000,
            },
            {
                "uid": 2,
                "name": "ramp_lim",
                "expression": "gen(G1,b) - gen(G1,b-1) <= 30",
            },
        ],
    }
}


class TestUserConstraintDiagram:
    def test_uc_nodes_created(self):
        model = _build(_UC_PLANNING, subsystem="electrical")
        assert sum(1 for n in model.nodes if n.kind == "user_constraint") == 2

    def test_uc_node_ids(self):
        model = _build(_UC_PLANNING, subsystem="electrical")
        ids = _node_ids(model)
        assert "uc_energy_cap_1" in ids
        assert "uc_ramp_lim_2" in ids

    def test_uc_label_has_penalty(self):
        model = _build(_UC_PLANNING, subsystem="electrical")
        n = next(n for n in model.nodes if n.node_id == "uc_energy_cap_1")
        assert "5000" in n.label or "penalty" in n.label

    def test_uc_tooltip_has_expression(self):
        model = _build(_UC_PLANNING, subsystem="electrical")
        n = next(n for n in model.nodes if n.node_id == "uc_energy_cap_1")
        assert "gen(G1" in n.tooltip or "sum" in n.tooltip


# ---------------------------------------------------------------------------
# Commitment overlays
# ---------------------------------------------------------------------------

_COMMIT_PLANNING = {
    "system": {
        "name": "uc_overlay_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G_uc", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "G_suc", "bus": 1, "pmax": 50},
        ],
        "commitment_array": [
            {"uid": 1, "name": "cm_G_uc", "generator": 1},
        ],
        "simple_commitment_array": [
            {"uid": 1, "name": "scm_G_suc", "generator": 2, "must_run": True},
        ],
    }
}


class TestCommitmentDiagram:
    def test_commitment_node_created(self):
        model = _build(_COMMIT_PLANNING, subsystem="electrical")
        assert "cmt_cm_G_uc_1" in _node_ids(model)

    def test_commitment_to_generator_edge(self):
        model = _build(_COMMIT_PLANNING, subsystem="electrical")
        assert ("cmt_cm_G_uc_1", "gen_G_uc_1") in _pairs(model)

    def test_simple_commitment_node_created(self):
        model = _build(_COMMIT_PLANNING, subsystem="electrical")
        assert "scmt_scm_G_suc_1" in _node_ids(model)

    def test_simple_commitment_to_generator_edge(self):
        model = _build(_COMMIT_PLANNING, subsystem="electrical")
        assert ("scmt_scm_G_suc_1", "gen_G_suc_2") in _pairs(model)

    def test_kinds_distinct(self):
        model = _build(_COMMIT_PLANNING, subsystem="electrical")
        kinds = {n.kind for n in model.nodes}
        assert "commitment" in kinds
        assert "simple_commitment" in kinds


# ---------------------------------------------------------------------------
# CarrierConverter
# ---------------------------------------------------------------------------

_CC_PLANNING = {
    "system": {
        "name": "carrier_conv_test",
        "bus_array": [
            {"uid": 1, "name": "B_elec"},
        ],
        "hydrogen_node_array": [
            {"uid": 10, "name": "H2_node"},
        ],
        "carrier_converter_array": [
            {
                "uid": 1,
                "name": "Electrolyser",
                "type": "electrolyser",
                "from_carrier": "electric",
                "to_carrier": "hydrogen",
                "from_node": 1,
                "to_node": 10,
                "capacity": 100,
                "efficiency": 0.7,
            }
        ],
    }
}


class TestCarrierConverterDiagram:
    def test_cconv_node_created(self):
        model = _build(_CC_PLANNING, subsystem="electrical")
        assert "cconv_Electrolyser_1" in _node_ids(model)

    def test_cconv_label_has_type(self):
        model = _build(_CC_PLANNING, subsystem="electrical")
        n = next(n for n in model.nodes if n.node_id == "cconv_Electrolyser_1")
        assert "electrolyser" in n.label

    def test_cconv_from_edge(self):
        """from_node → carrier_converter edge must exist."""
        model = _build(_CC_PLANNING, subsystem="electrical")
        # B_elec is in bus_array (resolved via _bid)
        assert ("bus_B_elec_1", "cconv_Electrolyser_1") in _pairs(model)

    def test_cconv_to_edge(self):
        """carrier_converter → to_node edge must exist."""
        model = _build(_CC_PLANNING, subsystem="electrical")
        # H2_node lives in hydrogen_node_array → resolved through fallback chain
        # whose ID prefix matches `_bid` (bus_).
        assert ("cconv_Electrolyser_1", "bus_H2_node_10") in _pairs(model)


# ---------------------------------------------------------------------------
# Integration: count_elements includes the new arrays
# ---------------------------------------------------------------------------


class TestCountElements:
    """``_count_elements`` must include the newly-rendered arrays."""

    def test_fuel_counted(self):
        b = gd.TopologyBuilder(_FUEL_PLANNING, opts=gd.FilterOptions())
        assert b._count_elements() >= 2  # noqa: SLF001

    def test_emission_family_counted(self):
        b = gd.TopologyBuilder(_EMISSION_PLANNING, opts=gd.FilterOptions())
        # 1 generator + 1 bus + 1 emission + 1 zone + 1 source = 5
        assert b._count_elements() >= 5  # noqa: SLF001

    def test_uc_counted(self):
        b = gd.TopologyBuilder(_UC_PLANNING, opts=gd.FilterOptions())
        assert b._count_elements() >= 2  # noqa: SLF001

    def test_dv_counted(self):
        b = gd.TopologyBuilder(_DV_PLANNING, opts=gd.FilterOptions())
        assert b._count_elements() >= 2  # noqa: SLF001

    def test_commitment_counted(self):
        b = gd.TopologyBuilder(_COMMIT_PLANNING, opts=gd.FilterOptions())
        # 1 bus + 2 generators + 1 commit + 1 simple_commit = 5
        assert b._count_elements() >= 5  # noqa: SLF001

    def test_carrier_converter_counted(self):
        b = gd.TopologyBuilder(_CC_PLANNING, opts=gd.FilterOptions())
        # 1 bus + 1 cconv = 2 (hydrogen_node not yet in count list)
        assert b._count_elements() >= 1  # noqa: SLF001


# ---------------------------------------------------------------------------
# Plant clustering — explicit ``plant_array`` entries
# ---------------------------------------------------------------------------

_PLANT_PLANNING = {
    "system": {
        "name": "plant_explicit",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "ATA_CC_1_ConfTGA", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "ATA_CC_1_ConfTGB", "bus": 1, "pmax": 100},
            {"uid": 3, "name": "ATA_CC_1_ConfTV", "bus": 1, "pmax": 191},
            {"uid": 4, "name": "Standalone", "bus": 1, "pmax": 50},
        ],
        "plant_array": [
            {
                "uid": 1,
                "name": "ATA_CC_1",
                "generator_names": [
                    "ATA_CC_1_ConfTGA",
                    "ATA_CC_1_ConfTGB",
                    "ATA_CC_1_ConfTV",
                ],
                "pmax": 391.0,
                "n_units": 1,
                "uniq_mutex": True,
            }
        ],
    }
}


class TestPlantClustering:
    """Members of an explicit ``Plant`` share a ``subcluster`` tag."""

    def test_member_generators_get_subcluster(self):
        model = _build(_PLANT_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in (
            "gen_ATA_CC_1_ConfTGA_1",
            "gen_ATA_CC_1_ConfTGB_2",
            "gen_ATA_CC_1_ConfTV_3",
        ):
            assert by_id[nid].subcluster == "plant:ATA_CC_1", (
                f"{nid} missing plant subcluster"
            )

    def test_non_member_generator_has_no_subcluster(self):
        model = _build(_PLANT_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_Standalone_4"].subcluster == ""

    def test_plant_meta_label_includes_pmax_n_units_and_uniq(self):
        model = _build(_PLANT_PLANNING)
        plant_meta = getattr(model, "plant_meta", {})
        label = plant_meta.get("plant:ATA_CC_1", "")
        assert "ATA_CC_1" in label
        assert "pmax=391" in label
        assert "n_units=1" in label
        assert "uniq" in label

    def test_no_extra_nodes_for_plant(self):
        """Plants are pure metadata — they must NOT add nodes or edges."""
        model_with = _build(_PLANT_PLANNING)
        # Drop the plant_array and rebuild — node/edge counts should match.
        bare = {
            "system": {
                k: v for k, v in _PLANT_PLANNING["system"].items() if k != "plant_array"
            }
        }
        model_without = _build(bare)
        assert len(model_with.nodes) == len(model_without.nodes)
        assert len(model_with.edges) == len(model_without.edges)

    def test_plant_meta_omits_optional_fields_when_unset(self):
        bare_plant = {
            "system": {
                "name": "bare_plant",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "generator_array": [
                    {"uid": 1, "name": "Plain_A", "bus": 1},
                    {"uid": 2, "name": "Plain_B", "bus": 1},
                ],
                "plant_array": [
                    {
                        "uid": 1,
                        "name": "Plain",
                        "generator_names": ["Plain_A", "Plain_B"],
                    }
                ],
            }
        }
        model = _build(bare_plant)
        plant_meta = getattr(model, "plant_meta", {})
        label = plant_meta["plant:Plain"]
        assert label == "Plain"
        assert "pmax" not in label
        assert "n_units" not in label
        assert "uniq" not in label

    def test_empty_member_plant_emits_no_meta(self):
        """A Plant whose generators are all missing from generator_array
        must not produce a plant_meta entry (the subgraph would be empty).
        """
        ghost = {
            "system": {
                "name": "ghost",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "generator_array": [{"uid": 1, "name": "Real", "bus": 1}],
                "plant_array": [
                    {
                        "uid": 1,
                        "name": "Ghost",
                        "generator_names": ["Phantom_A", "Phantom_B"],
                    }
                ],
            }
        }
        model = _build(ghost)
        plant_meta = getattr(model, "plant_meta", {})
        assert "plant:Ghost" not in plant_meta


# ---------------------------------------------------------------------------
# Auto-detection of hydro plants by name stem
# ---------------------------------------------------------------------------


class TestHydroPlantStemHeuristic:
    """The ``_hydro_plant_stem`` helper recognises common suffix patterns."""

    @staticmethod
    def _stem(name):
        from gtopt_diagram._topology_hydro import TopologyHydroMixin  # noqa: PLC0415

        return TopologyHydroMixin._hydro_plant_stem(name)  # noqa: SLF001

    def test_unit_number_suffix(self):
        assert self._stem("COLBUN_1") == "COLBUN"
        assert self._stem("COLBUN_2") == "COLBUN"
        assert self._stem("COLBUN_12") == "COLBUN"

    def test_explicit_unit_prefix(self):
        assert self._stem("Antuco_U1") == "Antuco"
        assert self._stem("Antuco_U12") == "Antuco"

    def test_fuel_band_variant(self):
        assert self._stem("PLANT_GN_A") == "PLANT"
        assert self._stem("PLANT_GN_B1") == "PLANT"

    def test_combined_cycle_config_suffix(self):
        assert self._stem("ATA_CC_1_ConfTGA") == "ATA_CC_1"
        assert self._stem("ATA_CC_1_ConfTV") == "ATA_CC_1"

    def test_single_letter_variant(self):
        assert self._stem("Plant_A") == "Plant"
        assert self._stem("Plant_B") == "Plant"

    def test_no_recognised_suffix_returns_none(self):
        assert self._stem("Standalone") is None
        assert self._stem("ThermalGen") is None
        assert self._stem("foo_bar") is None  # _bar is not a recognised pattern


_AUTO_HYDRO_PLANNING = {
    "system": {
        "name": "auto_hydro",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "COLBUN_1", "bus": 1, "pmax": 200},
            {"uid": 2, "name": "COLBUN_2", "bus": 1, "pmax": 200},
            {"uid": 3, "name": "COLBUN_3", "bus": 1, "pmax": 200},
            {"uid": 4, "name": "ELTORO_1", "bus": 1, "pmax": 100},
            {"uid": 5, "name": "ELTORO_2", "bus": 1, "pmax": 100},
            {"uid": 6, "name": "Standalone", "bus": 1, "pmax": 50},
        ],
    }
}


class TestAutoHydroPlantDetection:
    """3 COLBUN_* / 2 ELTORO_* generators auto-group; Standalone stays loose."""

    def test_colbun_units_share_subcluster(self):
        model = _build(_AUTO_HYDRO_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in ("gen_COLBUN_1_1", "gen_COLBUN_2_2", "gen_COLBUN_3_3"):
            assert by_id[nid].subcluster == "plant:COLBUN", (
                f"{nid} missing auto-plant subcluster"
            )

    def test_eltoro_units_share_subcluster(self):
        model = _build(_AUTO_HYDRO_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in ("gen_ELTORO_1_4", "gen_ELTORO_2_5"):
            assert by_id[nid].subcluster == "plant:ELTORO"

    def test_standalone_generator_not_grouped(self):
        model = _build(_AUTO_HYDRO_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_Standalone_6"].subcluster == ""

    def test_auto_plant_meta_uses_bus_label(self):
        """All COLBUN_* are on bus B1 so the bus-based generation-plant
        detection wins; the cluster label cites the bus name."""
        model = _build(_AUTO_HYDRO_PLANNING)
        plant_meta = getattr(model, "plant_meta", {})
        colbun_label = plant_meta["plant:COLBUN"]
        assert "COLBUN" in colbun_label
        assert "3 units" in colbun_label
        assert "@ B1" in colbun_label

    def test_singleton_stem_not_grouped(self):
        """A stem with only one match must not produce a cluster."""
        singleton = {
            "system": {
                "name": "singleton",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "generator_array": [{"uid": 1, "name": "Lonely_1", "bus": 1}],
            }
        }
        model = _build(singleton)
        plant_meta = getattr(model, "plant_meta", {})
        assert "plant:Lonely" not in plant_meta

    def test_explicit_plant_wins_over_auto_detection(self):
        """Generators in an explicit Plant must not be re-tagged by auto-detect."""
        mixed = {
            "system": {
                "name": "mixed",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "generator_array": [
                    {"uid": 1, "name": "COLBUN_1", "bus": 1},
                    {"uid": 2, "name": "COLBUN_2", "bus": 1},
                    {"uid": 3, "name": "COLBUN_3", "bus": 1},
                ],
                "plant_array": [
                    {
                        "uid": 1,
                        "name": "ExplicitColbun",
                        "generator_names": ["COLBUN_1", "COLBUN_2", "COLBUN_3"],
                        "pmax": 600,
                    }
                ],
            }
        }
        model = _build(mixed)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in ("gen_COLBUN_1_1", "gen_COLBUN_2_2", "gen_COLBUN_3_3"):
            assert by_id[nid].subcluster == "plant:ExplicitColbun"
        plant_meta = getattr(model, "plant_meta", {})
        assert "plant:ExplicitColbun" in plant_meta
        assert "plant:COLBUN" not in plant_meta

    def test_auto_detect_skipped_under_aggregate_global(self):
        """Aggressive aggregation collapses generator identity — no auto-plants."""
        model = _build(_AUTO_HYDRO_PLANNING, aggregate="global")
        plant_meta = getattr(model, "plant_meta", {})
        assert not any(k.startswith("plant:") for k in plant_meta)


# ---------------------------------------------------------------------------
# Junction-based hydro plant detection
# ---------------------------------------------------------------------------

_JUNC_HYDRO = {
    "system": {
        "name": "junc_hydro",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "junction_array": [
            {"uid": 10, "name": "COLBUN_INTAKE"},
            {"uid": 20, "name": "LonelyIntake"},
        ],
        "generator_array": [
            {"uid": 1, "name": "COLBUN_1", "bus": 1, "pmax": 200},
            {"uid": 2, "name": "COLBUN_2", "bus": 1, "pmax": 200},
            {"uid": 3, "name": "COLBUN_3", "bus": 1, "pmax": 200},
            {"uid": 4, "name": "OneTurbine", "bus": 1, "pmax": 100},
        ],
        "turbine_array": [
            # Three turbines share an intake junction → hydro plant.
            {
                "uid": 1,
                "name": "T_COLBUN_1",
                "junction_a": 10,
                "generator": 1,
            },
            {
                "uid": 2,
                "name": "T_COLBUN_2",
                "junction_a": 10,
                "generator": 2,
            },
            {
                "uid": 3,
                "name": "T_COLBUN_3",
                "junction_a": 10,
                "generator": 3,
            },
            # Single turbine on its own intake — should not cluster.
            {
                "uid": 4,
                "name": "T_Lonely",
                "junction_a": 20,
                "generator": 4,
            },
        ],
    }
}


class TestAutoHydroPlantByJunction:
    """Turbines sharing an upstream junction cluster as one hydro plant."""

    def test_three_turbines_share_junction_get_subcluster(self):
        model = _build(_JUNC_HYDRO)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in ("gen_COLBUN_1_1", "gen_COLBUN_2_2", "gen_COLBUN_3_3"):
            # The cluster key prefers the shared name stem when one
            # exists; only when stems differ does it fall back to the
            # junction name.
            assert by_id[nid].subcluster == "plant:COLBUN", (
                f"{nid} expected plant:COLBUN, got {by_id[nid].subcluster!r}"
            )

    def test_single_turbine_intake_not_clustered(self):
        model = _build(_JUNC_HYDRO)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_OneTurbine_4"].subcluster == ""

    def test_meta_label_carries_junction_and_unit_count(self):
        model = _build(_JUNC_HYDRO)
        meta = getattr(model, "plant_meta", {})
        label = meta["plant:COLBUN"]
        assert "COLBUN" in label
        assert "3 units" in label
        assert "@ COLBUN_INTAKE" in label

    def test_mixed_name_stems_fall_back_to_junction_label(self):
        """When members have different name stems, the cluster label
        is the junction name (so the user sees the physical anchor)."""
        mixed = {
            "system": {
                "name": "mixed_stems",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "junction_array": [{"uid": 9, "name": "SharedIntake"}],
                "generator_array": [
                    {"uid": 1, "name": "PlantAlpha", "bus": 1},
                    {"uid": 2, "name": "PlantBeta", "bus": 1},
                ],
                "turbine_array": [
                    {"uid": 1, "name": "Ta", "junction_a": 9, "generator": 1},
                    {"uid": 2, "name": "Tb", "junction_a": 9, "generator": 2},
                ],
            }
        }
        model = _build(mixed)
        meta = getattr(model, "plant_meta", {})
        assert "plant:SharedIntake" in meta
        label = meta["plant:SharedIntake"]
        assert "@ SharedIntake" in label


# ---------------------------------------------------------------------------
# Substation detection
# ---------------------------------------------------------------------------


class TestSubstationPrefix:
    """``_substation_prefix`` strips voltage suffixes correctly."""

    @staticmethod
    def _pre(name):
        from gtopt_diagram._topology_hydro import TopologyHydroMixin  # noqa: PLC0415

        return TopologyHydroMixin._substation_prefix(name)  # noqa: SLF001

    def test_kv_suffix(self):
        assert self._pre("CHARRUA_500KV") == "CHARRUA"
        assert self._pre("CHARRUA_220KV") == "CHARRUA"
        assert self._pre("CHARRUA_13.8KV") == "CHARRUA"

    def test_v_prefix_suffix(self):
        assert self._pre("Crucero_V500") == "Crucero"

    def test_bare_voltage_number(self):
        assert self._pre("Polpaico_500") == "Polpaico"
        assert self._pre("Polpaico_220") == "Polpaico"

    def test_no_suffix_returns_none(self):
        assert self._pre("Charrua") is None
        assert self._pre("Unrecognised") is None


class TestLineIsIntraSubstation:
    """``_line_is_intra_substation`` flags transformers and short lines."""

    @staticmethod
    def _short(line):
        from gtopt_diagram._topology_hydro import TopologyHydroMixin  # noqa: PLC0415

        return TopologyHydroMixin._line_is_intra_substation(line)  # noqa: SLF001

    def test_transformer_type_flagged(self):
        assert self._short({"type": "transformer", "reactance": 0.1})
        assert self._short({"type": "Transformer", "reactance": 1.0})

    def test_small_rx_flagged(self):
        # r=0.001, x=0.02 — well below thresholds.
        assert self._short({"resistance": 0.001, "reactance": 0.02})

    def test_typical_transmission_line_not_flagged(self):
        # r=0.02, x=0.15 — above both thresholds.
        assert not self._short({"resistance": 0.02, "reactance": 0.15})

    def test_missing_rx_not_flagged(self):
        # No reactance means no signal — must not flag.
        assert not self._short({"bus_a": 1, "bus_b": 2})


_SUBSTATION_PLANNING = {
    "system": {
        "name": "substations",
        "bus_array": [
            {"uid": 1, "name": "CHARRUA_500KV", "voltage": 500},
            {"uid": 2, "name": "CHARRUA_220KV", "voltage": 220},
            {"uid": 3, "name": "POLPAICO_500KV", "voltage": 500},
            {"uid": 4, "name": "POLPAICO_220KV", "voltage": 220},
            {"uid": 5, "name": "RemoteBus", "voltage": 220},
        ],
        "line_array": [
            # CHARRUA transformer — short line with matching name prefix.
            {
                "uid": 1,
                "name": "TX_CHARRUA",
                "bus_a": 1,
                "bus_b": 2,
                "type": "transformer",
                "reactance": 0.08,
                "resistance": 0.002,
                "tmax_ab": 500,
                "tmax_ba": 500,
            },
            # POLPAICO transformer.
            {
                "uid": 2,
                "name": "TX_POLPAICO",
                "bus_a": 3,
                "bus_b": 4,
                "type": "transformer",
                "reactance": 0.07,
                "resistance": 0.002,
                "tmax_ab": 500,
                "tmax_ba": 500,
            },
            # Long inter-substation line — must NOT cluster CHARRUA with POLPAICO.
            {
                "uid": 3,
                "name": "TL_CHARRUA_POLPAICO",
                "bus_a": 1,
                "bus_b": 3,
                "type": "ac",
                "reactance": 0.4,
                "resistance": 0.04,
                "tmax_ab": 1500,
                "tmax_ba": 1500,
            },
            # Line touching RemoteBus — no substation prefix to match.
            {
                "uid": 4,
                "name": "TL_to_remote",
                "bus_a": 2,
                "bus_b": 5,
                "type": "ac",
                "reactance": 0.3,
                "resistance": 0.03,
                "tmax_ab": 100,
                "tmax_ba": 100,
            },
        ],
    }
}


class TestSubstationClustering:
    """End-to-end: matching name prefix + short line groups buses into a substation."""

    def test_charrua_buses_share_subcluster(self):
        model = _build(_SUBSTATION_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        ch500 = by_id["bus_CHARRUA_500KV_1"]
        ch220 = by_id["bus_CHARRUA_220KV_2"]
        assert ch500.subcluster == "substation:CHARRUA"
        assert ch220.subcluster == "substation:CHARRUA"

    def test_polpaico_buses_share_subcluster(self):
        model = _build(_SUBSTATION_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["bus_POLPAICO_500KV_3"].subcluster == "substation:POLPAICO"
        assert by_id["bus_POLPAICO_220KV_4"].subcluster == "substation:POLPAICO"

    def test_remote_bus_unclustered(self):
        model = _build(_SUBSTATION_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["bus_RemoteBus_5"].subcluster == ""

    def test_inter_substation_line_does_not_merge(self):
        """CHARRUA and POLPAICO must remain distinct clusters even
        though a 500 kV AC tie connects them — that line has nontrivial
        impedance AND mismatching name prefixes."""
        model = _build(_SUBSTATION_PLANNING)
        meta = getattr(model, "plant_meta", {})
        assert "substation:CHARRUA" in meta
        assert "substation:POLPAICO" in meta
        # Distinct labels — no accidental merge.
        assert meta["substation:CHARRUA"] != meta["substation:POLPAICO"]

    def test_substation_label_includes_bus_count(self):
        model = _build(_SUBSTATION_PLANNING)
        meta = getattr(model, "plant_meta", {})
        label = meta["substation:CHARRUA"]
        assert "CHARRUA" in label
        assert "substation" in label
        assert "2 buses" in label

    def test_short_line_without_name_match_does_not_cluster(self):
        """A short line whose endpoints have different name prefixes
        must not trigger substation clustering — protects against
        DC ties / coupling reactors between unrelated buses."""
        mismatched = {
            "system": {
                "name": "mismatched",
                "bus_array": [
                    {"uid": 1, "name": "ALPHA_220KV"},
                    {"uid": 2, "name": "BETA_220KV"},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "Short_link",
                        "bus_a": 1,
                        "bus_b": 2,
                        "type": "transformer",
                        "reactance": 0.05,
                    }
                ],
            }
        }
        model = _build(mismatched)
        meta = getattr(model, "plant_meta", {})
        assert not any(k.startswith("substation:") for k in meta)


# ---------------------------------------------------------------------------
# Generation plant detection — name stem + same-bus (combined-cycle, multi-unit)
# ---------------------------------------------------------------------------

_GEN_PLANT_PLANNING = {
    "system": {
        "name": "gen_plants",
        "bus_array": [
            {"uid": 1, "name": "ATACAMA220"},
            {"uid": 2, "name": "REMOTE220"},
        ],
        "generator_array": [
            # 3 units of the same physical plant on bus ATACAMA220.
            {"uid": 1, "name": "ATA_1", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "ATA_2", "bus": 1, "pmax": 100},
            {"uid": 3, "name": "ATA_3", "bus": 1, "pmax": 191},
            # Different physical plant (stem DUP) on bus REMOTE220.
            # Must NOT merge with the ATA cluster despite both stems
            # appearing in the same system.
            {"uid": 4, "name": "DUP_1", "bus": 2, "pmax": 50},
            {"uid": 5, "name": "DUP_2", "bus": 2, "pmax": 50},
            # Single generator on the same bus — no group.
            {"uid": 6, "name": "Standalone", "bus": 1, "pmax": 20},
        ],
    }
}


class TestGenerationPlantByBus:
    """Multi-unit generators on the same bus with matching name stems group."""

    def test_atacama_units_grouped(self):
        model = _build(_GEN_PLANT_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        # ATA_1 / ATA_2 / ATA_3 share stem "ATA" AND bus 1 → cluster:ATA.
        assert by_id["gen_ATA_1_1"].subcluster == "plant:ATA"
        assert by_id["gen_ATA_2_2"].subcluster == "plant:ATA"
        assert by_id["gen_ATA_3_3"].subcluster == "plant:ATA"

    def test_outlier_on_different_bus_grouped_separately(self):
        """DUP_1 / DUP_2 on bus 2 cluster as their own plant — NOT
        merged with the ATA cluster on bus 1."""
        model = _build(_GEN_PLANT_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        d1 = by_id["gen_DUP_1_4"].subcluster
        d2 = by_id["gen_DUP_2_5"].subcluster
        assert d1 == "plant:DUP"
        assert d2 == "plant:DUP"
        assert d1 != by_id["gen_ATA_1_1"].subcluster

    def test_standalone_not_grouped(self):
        model = _build(_GEN_PLANT_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_Standalone_6"].subcluster == ""

    def test_bus_label_in_meta(self):
        model = _build(_GEN_PLANT_PLANNING)
        meta = getattr(model, "plant_meta", {})
        assert "@ ATACAMA220" in meta["plant:ATA"]
        assert "@ REMOTE220" in meta["plant:DUP"]

    def test_same_stem_different_buses_does_not_merge(self):
        """Stem 'PUMP' on two different buses produces zero clusters
        because each per-bus group has only one member."""
        mixed_buses = {
            "system": {
                "name": "mixed_buses",
                "bus_array": [
                    {"uid": 1, "name": "BusA"},
                    {"uid": 2, "name": "BusB"},
                ],
                "generator_array": [
                    {"uid": 1, "name": "PUMP_1", "bus": 1},
                    {"uid": 2, "name": "PUMP_2", "bus": 2},
                ],
            }
        }
        model = _build(mixed_buses)
        meta = getattr(model, "plant_meta", {})
        assert "plant:PUMP" not in meta

    def test_no_bus_falls_back_to_pure_name_stem(self):
        """When ``bus`` is absent, the pure-name fallback path runs."""
        no_bus = {
            "system": {
                "name": "no_bus",
                "bus_array": [],
                "generator_array": [
                    {"uid": 1, "name": "FOO_1"},  # bus omitted
                    {"uid": 2, "name": "FOO_2"},
                ],
            }
        }
        model = _build(no_bus)
        meta = getattr(model, "plant_meta", {})
        label = meta.get("plant:FOO", "")
        assert "FOO" in label
        assert "auto" in label


# ---------------------------------------------------------------------------
# Priority invariant: hydro (junction) > generation (bus)
# ---------------------------------------------------------------------------

_HYDRO_OVER_BUS_PLANNING = {
    "system": {
        "name": "hydro_wins",
        "bus_array": [{"uid": 1, "name": "Bus1"}],
        "junction_array": [
            {"uid": 10, "name": "DamIntake"},
        ],
        "generator_array": [
            # Two units on the same bus — would otherwise be grouped by
            # the bus-based pass as "plant:HYDRO @ Bus1".
            {"uid": 1, "name": "HYDRO_1", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "HYDRO_2", "bus": 1, "pmax": 100},
        ],
        "turbine_array": [
            # But each unit has a turbine sharing DamIntake → the
            # junction-based hydro detection MUST claim them first.
            {"uid": 1, "name": "T1", "junction_a": 10, "generator": 1},
            {"uid": 2, "name": "T2", "junction_a": 10, "generator": 2},
        ],
    }
}


class TestHydroPriorityOverGenerationPlant:
    """A generator already in a hydro plant must NOT also get a gen-plant tag."""

    def test_hydro_wins_label_is_junction(self):
        model = _build(_HYDRO_OVER_BUS_PLANNING)
        meta = getattr(model, "plant_meta", {})
        label = meta["plant:HYDRO"]
        assert "@ DamIntake" in label
        assert "@ Bus1" not in label

    def test_no_duplicate_cluster_for_same_stem(self):
        """The same stem must not produce both a hydro and a gen cluster."""
        model = _build(_HYDRO_OVER_BUS_PLANNING)
        meta = getattr(model, "plant_meta", {})
        plant_keys = [k for k in meta if k == "plant:HYDRO"]
        assert len(plant_keys) == 1

    def test_each_generator_has_exactly_one_subcluster(self):
        model = _build(_HYDRO_OVER_BUS_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_HYDRO_1_1"].subcluster == "plant:HYDRO"
        assert by_id["gen_HYDRO_2_2"].subcluster == "plant:HYDRO"


# ---------------------------------------------------------------------------
# Intra-substation edges live inside the cluster's dot subgraph
# ---------------------------------------------------------------------------


class TestSubstationIntraClusterEdges:
    """The transformer/short line joining two substation buses must render
    **inside** the substation subgraph, not at the top level."""

    @staticmethod
    def _dot(planning):
        from gtopt_diagram._renderers import render_graphviz  # noqa: PLC0415

        model = gd.TopologyBuilder(planning, opts=gd.FilterOptions()).build()
        return render_graphviz(model, fmt="dot", use_clusters=False)

    @staticmethod
    def _cluster_body(dot_src: str, cluster_name: str) -> str:
        start = dot_src.index(f"{cluster_name} {{")
        depth = 0
        for i, ch in enumerate(dot_src[start:], start=start):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return dot_src[start:i]
        return dot_src[start:]

    def test_intra_substation_edge_inside_cluster(self):
        planning = {
            "system": {
                "name": "intra_edge",
                "bus_array": [
                    {"uid": 1, "name": "CHARRUA_500KV"},
                    {"uid": 2, "name": "CHARRUA_220KV"},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "TX_CHARRUA",
                        "bus_a": 1,
                        "bus_b": 2,
                        "type": "transformer",
                        "reactance": 0.08,
                        "resistance": 0.002,
                    }
                ],
            }
        }
        s = self._dot(planning)
        body = self._cluster_body(s, "cluster_substation_CHARRUA")
        assert "bus_CHARRUA_500KV_1 -- bus_CHARRUA_220KV_2" in body, (
            "intra-substation transformer edge not found inside cluster"
        )

    def test_inter_substation_edge_at_top_level(self):
        planning = {
            "system": {
                "name": "inter_edge",
                "bus_array": [
                    {"uid": 1, "name": "CHARRUA_500KV"},
                    {"uid": 2, "name": "CHARRUA_220KV"},
                    {"uid": 3, "name": "POLPAICO_500KV"},
                    {"uid": 4, "name": "POLPAICO_220KV"},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "TX_CHARRUA",
                        "bus_a": 1,
                        "bus_b": 2,
                        "type": "transformer",
                        "reactance": 0.08,
                    },
                    {
                        "uid": 2,
                        "name": "TX_POLPAICO",
                        "bus_a": 3,
                        "bus_b": 4,
                        "type": "transformer",
                        "reactance": 0.07,
                    },
                    {
                        "uid": 3,
                        "name": "TL_CHA_POL",
                        "bus_a": 1,
                        "bus_b": 3,
                        "type": "ac",
                        "reactance": 0.4,
                    },
                ],
            }
        }
        s = self._dot(planning)
        assert "TL_CHA_POL" in s
        body = self._cluster_body(s, "cluster_substation_CHARRUA")
        assert "TL_CHA_POL" not in body, (
            "inter-substation edge wrongly placed inside CHARRUA cluster"
        )


# ---------------------------------------------------------------------------
# A — Basin (hydro drainage cascade) super-cluster
# ---------------------------------------------------------------------------

_BASIN_PLANNING = {
    "system": {
        "name": "basin",
        "bus_array": [{"uid": 1, "name": "Bus1"}],
        "junction_array": [
            {"uid": 10, "name": "Lake_top"},
            {"uid": 11, "name": "Mid_pool"},
            {"uid": 12, "name": "Outlet"},
            # A second isolated junction in a different drainage area.
            {"uid": 20, "name": "Lonely"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "ww1", "junction_a": 10, "junction_b": 11},
            {"uid": 2, "name": "ww2", "junction_a": 11, "junction_b": 12},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "Lake", "junction": 10, "capacity": 1000},
        ],
        "generator_array": [
            {"uid": 1, "name": "G_cascade", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "G_lonely", "bus": 1, "pmax": 50},
        ],
        "turbine_array": [
            # Mode B turbine: drains Mid_pool to Outlet, drives G_cascade
            {
                "uid": 1,
                "name": "T_casc",
                "junction_a": 11,
                "junction_b": 12,
                "generator": 1,
            },
        ],
        "flow_array": [
            {"uid": 1, "name": "Inflow_top", "junction": 10, "discharge": 5.0},
        ],
    }
}


class TestBasinDetection:
    """Hydro drainage cascade becomes one super-cluster."""

    def test_basin_label_uses_reservoir_name(self):
        model = _build(_BASIN_PLANNING)
        meta = getattr(model, "plant_meta", {})
        assert "basin:Lake" in meta
        assert "Lake basin" in meta["basin:Lake"]
        assert "3 junctions" in meta["basin:Lake"]

    def test_basin_tags_three_junctions(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        for nid in ("junc_Lake_top_10", "junc_Mid_pool_11", "junc_Outlet_12"):
            assert by_id[nid].super_cluster == "basin:Lake", (
                f"{nid} expected basin:Lake, got {by_id[nid].super_cluster!r}"
            )

    def test_lonely_junction_not_in_any_basin(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["junc_Lonely_20"].super_cluster == ""

    def test_reservoir_in_basin(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["res_Lake_1"].super_cluster == "basin:Lake"

    def test_turbine_in_basin(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["turb_T_casc_1"].super_cluster == "basin:Lake"

    def test_turbine_generator_inherits_basin(self):
        """The generator driven by a basin turbine joins the basin too."""
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_G_cascade_1"].super_cluster == "basin:Lake"

    def test_unrelated_generator_not_in_basin(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_G_lonely_2"].super_cluster == ""

    def test_flow_source_in_basin(self):
        model = _build(_BASIN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["flow_Inflow_top_1"].super_cluster == "basin:Lake"


# ---------------------------------------------------------------------------
# B — Reserve zone subcluster
# ---------------------------------------------------------------------------

_RESERVE_PLANNING = {
    "system": {
        "name": "reserve",
        "bus_array": [{"uid": 1, "name": "Bus1"}],
        "generator_array": [
            {"uid": 1, "name": "Gen_AGC_1", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "Gen_AGC_2", "bus": 1, "pmax": 100},
            {"uid": 3, "name": "Gen_AGC_3", "bus": 1, "pmax": 50},
            {"uid": 4, "name": "Gen_NoReserve", "bus": 1, "pmax": 25},
        ],
        "reserve_zone_array": [
            {"uid": 1, "name": "AGC_North", "type": "spinning"},
        ],
        "reserve_provision_array": [
            {"uid": 1, "name": "P1", "generator": 1, "reserve_zones": [1]},
            {"uid": 2, "name": "P2", "generator": 2, "reserve_zones": [1]},
            {"uid": 3, "name": "P3", "generator": 3, "reserve_zones": [1]},
        ],
    }
}


class TestReserveZoneGrouping:
    """Generators providing reserve to the same ReserveZone cluster together."""

    def test_three_units_tagged(self):
        model = _build(_RESERVE_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        # The bus-based plant detection would also fire on Gen_AGC_*
        # (same stem "Gen_AGC" + same bus), winning before reserve zone.
        # Either way the three units share ONE subcluster.
        sub1 = by_id["gen_Gen_AGC_1_1"].subcluster
        sub2 = by_id["gen_Gen_AGC_2_2"].subcluster
        sub3 = by_id["gen_Gen_AGC_3_3"].subcluster
        assert sub1 != ""
        assert sub1 == sub2 == sub3

    def test_non_provider_not_grouped(self):
        model = _build(_RESERVE_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_Gen_NoReserve_4"].subcluster == ""

    def test_reserve_zone_label_when_no_plant_conflict(self):
        """Generators with NO name stem fall back to reserve-zone clustering."""
        no_stem = {
            "system": {
                "name": "no_stem",
                "bus_array": [{"uid": 1, "name": "Bus1"}],
                "generator_array": [
                    {"uid": 1, "name": "Alpha", "bus": 1},
                    {"uid": 2, "name": "Beta", "bus": 1},
                ],
                "reserve_zone_array": [{"uid": 1, "name": "MainZone"}],
                "reserve_provision_array": [
                    {"uid": 1, "name": "P1", "generator": 1, "reserve_zones": [1]},
                    {"uid": 2, "name": "P2", "generator": 2, "reserve_zones": [1]},
                ],
            }
        }
        model = _build(no_stem)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_Alpha_1"].subcluster == "reserve_zone:MainZone"
        assert by_id["gen_Beta_2"].subcluster == "reserve_zone:MainZone"
        meta = getattr(model, "plant_meta", {})
        label = meta["reserve_zone:MainZone"]
        assert "MainZone reserve zone" in label
        assert "2 units" in label


# ---------------------------------------------------------------------------
# C — Fuel sharing subcluster
# ---------------------------------------------------------------------------

_FUEL_PLANNING_C = {
    "system": {
        "name": "fuel_grp",
        "bus_array": [{"uid": 1, "name": "Bus1"}],
        "fuel_array": [
            {"uid": 1, "name": "GasNorte"},
            {"uid": 2, "name": "DieselSur"},
        ],
        "generator_array": [
            {"uid": 1, "name": "GenA", "bus": 1, "fuel": 1},
            {"uid": 2, "name": "GenB", "bus": 1, "fuel": 1},
            {"uid": 3, "name": "GenC", "bus": 1, "fuel": 1},
            {"uid": 4, "name": "GenD", "bus": 1, "fuel": 2},
            {"uid": 5, "name": "GenE", "bus": 1, "fuel": 2},
            # GenF has no fuel — must not cluster.
            {"uid": 6, "name": "GenF", "bus": 1},
        ],
    }
}


class TestFuelGrouping:
    """Generators sharing a Fuel cluster together (fallback below plant)."""

    def test_gas_norte_three_units(self):
        model = _build(_FUEL_PLANNING_C)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_GenA_1"].subcluster == "fuel:GasNorte"
        assert by_id["gen_GenB_2"].subcluster == "fuel:GasNorte"
        assert by_id["gen_GenC_3"].subcluster == "fuel:GasNorte"

    def test_diesel_sur_two_units(self):
        model = _build(_FUEL_PLANNING_C)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_GenD_4"].subcluster == "fuel:DieselSur"
        assert by_id["gen_GenE_5"].subcluster == "fuel:DieselSur"

    def test_unfueled_generator_not_grouped(self):
        model = _build(_FUEL_PLANNING_C)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_GenF_6"].subcluster == ""

    def test_singleton_fuel_consumer_not_grouped(self):
        """A fuel with only one consumer must not produce a cluster."""
        singleton = {
            "system": {
                "name": "single_fuel",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "fuel_array": [{"uid": 1, "name": "Lonely"}],
                "generator_array": [{"uid": 1, "name": "OnlyOne", "bus": 1, "fuel": 1}],
            }
        }
        model = _build(singleton)
        meta = getattr(model, "plant_meta", {})
        assert "fuel:Lonely" not in meta

    def test_plant_wins_over_fuel(self):
        """Generators in a plant cluster must not be re-tagged with fuel."""
        plant_then_fuel = {
            "system": {
                "name": "plant_wins",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "fuel_array": [{"uid": 1, "name": "Gas"}],
                "generator_array": [
                    {"uid": 1, "name": "Plant_1", "bus": 1, "fuel": 1},
                    {"uid": 2, "name": "Plant_2", "bus": 1, "fuel": 1},
                ],
            }
        }
        model = _build(plant_then_fuel)
        by_id = {n.node_id: n for n in model.nodes}
        # Bus+name plant detection wins (stem "Plant" + bus 1).
        assert by_id["gen_Plant_1_1"].subcluster == "plant:Plant"
        assert by_id["gen_Plant_2_2"].subcluster == "plant:Plant"
        meta = getattr(model, "plant_meta", {})
        assert "fuel:Gas" not in meta


# ---------------------------------------------------------------------------
# A — Inflow-anchored basin (Rapel-style: 1 junction, no reservoir)
# ---------------------------------------------------------------------------

_RAPEL_STYLE_BASIN = {
    "system": {
        "name": "rapel_style",
        "bus_array": [{"uid": 1, "name": "Bus1"}],
        "junction_array": [{"uid": 30, "name": "RAPEL"}],
        "flow_array": [
            {"uid": 1, "name": "Inflow_RAPEL", "junction": 30, "discharge": 250.0},
        ],
        "generator_array": [
            {"uid": 7, "name": "Rapel", "bus": 1, "pmax": 380},
        ],
        "turbine_array": [
            {
                "uid": 7,
                "name": "T_RAPEL",
                "junction_a": 30,
                "generator": 7,
                "production_factor": 1.5,
            },
        ],
    }
}


class TestInflowAnchoredBasin:
    """Junction with no reservoir but inflow + terminal turbine = basin."""

    def test_basin_emitted_for_single_junction(self):
        model = _build(_RAPEL_STYLE_BASIN)
        meta = getattr(model, "plant_meta", {})
        assert "basin:Rapel" in meta or "basin:RAPEL" in meta

    def test_junction_tagged(self):
        model = _build(_RAPEL_STYLE_BASIN)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["junc_RAPEL_30"].super_cluster.startswith("basin:")

    def test_turbine_in_basin(self):
        model = _build(_RAPEL_STYLE_BASIN)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["turb_T_RAPEL_7"].super_cluster.startswith("basin:")

    def test_inflow_in_basin(self):
        model = _build(_RAPEL_STYLE_BASIN)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["flow_Inflow_RAPEL_1"].super_cluster.startswith("basin:")

    def test_topology_only_junction_is_not_a_basin(self):
        bare = {
            "system": {
                "name": "bare_junction",
                "bus_array": [{"uid": 1, "name": "B1"}],
                "junction_array": [{"uid": 99, "name": "IsolatedNode"}],
            }
        }
        model = _build(bare)
        meta = getattr(model, "plant_meta", {})
        assert not any(k.startswith("basin:") for k in meta)


# ---------------------------------------------------------------------------
# N1 — bridges + articulation points (critical topology)
# ---------------------------------------------------------------------------

_BRIDGE_PLANNING = {
    "system": {
        "name": "bridge",
        "bus_array": [
            {"uid": 1, "name": "A"},
            {"uid": 2, "name": "B"},
            {"uid": 3, "name": "C"},
            {"uid": 4, "name": "D"},
        ],
        "line_array": [
            {"uid": 1, "name": "L_AB", "bus_a": 1, "bus_b": 2, "type": "ac"},
            {"uid": 2, "name": "L_BC", "bus_a": 2, "bus_b": 3, "type": "ac"},
            {"uid": 3, "name": "L_CA", "bus_a": 3, "bus_b": 1, "type": "ac"},
            # Pendant D-A — A becomes articulation, L_AD becomes a bridge.
            {"uid": 4, "name": "L_AD", "bus_a": 1, "bus_b": 4, "type": "ac"},
        ],
    }
}


class TestCriticalTopology:
    """N1: bridges and articulation points appear with is_critical=True."""

    def test_pendant_line_is_bridge(self):
        model = _build(_BRIDGE_PLANNING)
        bridges = [e for e in model.edges if e.is_critical]
        assert len(bridges) == 1
        names = {e.label.split(":")[0] for e in bridges}
        assert "L_AD" in names

    def test_articulation_point_marked(self):
        model = _build(_BRIDGE_PLANNING)
        crit_buses = [n for n in model.nodes if n.is_critical and n.kind == "bus"]
        assert len(crit_buses) == 1
        assert crit_buses[0].node_id == "bus_A_1"


_ISOLATED_PLANNING = {
    "system": {
        "name": "iso",
        "bus_array": [
            {"uid": 1, "name": "Connected1"},
            {"uid": 2, "name": "Connected2"},
            {"uid": 3, "name": "Lonely"},
        ],
        "line_array": [
            {"uid": 1, "name": "L_12", "bus_a": 1, "bus_b": 2, "type": "ac"},
        ],
    }
}


class TestIsolatedBuses:
    """Buses with no AC connection are flagged critical."""

    def test_lonely_bus_is_critical(self):
        model = _build(_ISOLATED_PLANNING)
        crit = [n for n in model.nodes if n.is_critical and n.kind == "bus"]
        assert len(crit) == 1
        assert crit[0].node_id == "bus_Lonely_3"
        assert "isolated" in crit[0].tooltip

    def test_connected_buses_not_critical(self):
        model = _build(_ISOLATED_PLANNING)
        for n in model.nodes:
            if n.node_id in ("bus_Connected1_1", "bus_Connected2_2"):
                assert not n.is_critical


_ORPHAN_PLANNING = {
    "system": {
        "name": "orphan",
        "bus_array": [
            {"uid": 1, "name": "GenIsland"},
            {"uid": 2, "name": "LoadIsland_A"},
            {"uid": 3, "name": "LoadIsland_B"},
        ],
        "line_array": [
            {"uid": 1, "name": "L_AB", "bus_a": 2, "bus_b": 3, "type": "ac"},
        ],
        "generator_array": [
            {"uid": 1, "name": "G_orphan", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "G_useful", "bus": 2, "pmax": 100},
        ],
        "demand_array": [
            {"uid": 1, "name": "D_main", "bus": 3, "lmax": 200},
        ],
    }
}


class TestOrphanGenerators:
    """N11: generators in a component with no demand are orphaned."""

    def test_orphan_marked_critical(self):
        model = _build(_ORPHAN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert by_id["gen_G_orphan_1"].is_critical
        assert "orphan" in by_id["gen_G_orphan_1"].tooltip

    def test_non_orphan_not_critical(self):
        model = _build(_ORPHAN_PLANNING)
        by_id = {n.node_id: n for n in model.nodes}
        assert not by_id["gen_G_useful_2"].is_critical


class TestElectricalLoops:
    """N10: cycle_basis stash exposes AC fundamental loops."""

    def test_triangle_yields_one_cycle(self):
        triangle = {
            "system": {
                "name": "tri",
                "bus_array": [
                    {"uid": 1, "name": "A"},
                    {"uid": 2, "name": "B"},
                    {"uid": 3, "name": "C"},
                ],
                "line_array": [
                    {"uid": 1, "name": "L_AB", "bus_a": 1, "bus_b": 2, "type": "ac"},
                    {"uid": 2, "name": "L_BC", "bus_a": 2, "bus_b": 3, "type": "ac"},
                    {"uid": 3, "name": "L_CA", "bus_a": 3, "bus_b": 1, "type": "ac"},
                ],
            }
        }
        model = _build(triangle)
        loops = getattr(model, "electrical_loops", []) or []
        assert len(loops) == 1
        assert set(loops[0]) == {"bus_A_1", "bus_B_2", "bus_C_3"}

    def test_radial_yields_no_cycles(self):
        radial = {
            "system": {
                "name": "radial",
                "bus_array": [
                    {"uid": 1, "name": "A"},
                    {"uid": 2, "name": "B"},
                    {"uid": 3, "name": "C"},
                ],
                "line_array": [
                    {"uid": 1, "name": "L_AB", "bus_a": 1, "bus_b": 2, "type": "ac"},
                    {"uid": 2, "name": "L_BC", "bus_a": 2, "bus_b": 3, "type": "ac"},
                ],
            }
        }
        model = _build(radial)
        loops = getattr(model, "electrical_loops", []) or []
        assert not loops


class TestEdgeBetweenness:
    def test_betweenness_stashed_on_large_case(self):
        chain = {
            "system": {
                "name": "chain",
                "bus_array": [{"uid": i, "name": f"B{i}"} for i in range(1, 6)],
                "line_array": [
                    {"uid": 1, "bus_a": 1, "bus_b": 2, "type": "ac"},
                    {"uid": 2, "bus_a": 2, "bus_b": 3, "type": "ac"},
                    {"uid": 3, "bus_a": 3, "bus_b": 4, "type": "ac"},
                    {"uid": 4, "bus_a": 4, "bus_b": 5, "type": "ac"},
                ],
            }
        }
        model = _build(chain)
        betw = getattr(model, "edge_betweenness", {}) or {}
        assert betw, "edge_betweenness not populated"
        scores = sorted(set(betw.values()), reverse=True)
        assert scores[0] > scores[-1]
