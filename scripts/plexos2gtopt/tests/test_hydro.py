"""Unit tests for the P4 hydro extractors + writer builders.

A self-contained synthetic XML carries: two Storages (upstream RES_A,
downstream RES_B), one Waterway joining them, one hydro Generator
attached to RES_A (Head Storage), one bus / one fuel-less generator
membership to exercise the topology. Inflow CSV pushes water into
RES_A only.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import (
    FlowSpec,
    JunctionSpec,
    ReservoirSpec,
    TurbineSpec,
    WaterwaySpec,
)
from plexos2gtopt.gtopt_writer import (
    build_flow_array,
    build_junction_array,
    build_reservoir_array,
    build_turbine_array,
    build_waterway_array,
)
from plexos2gtopt.parsers import (
    extract_flows,
    extract_junctions,
    extract_reservoirs,
    extract_turbines,
    extract_waterways,
)
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_HYDRO_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_class><class_id>9</class_id><name>Waterway</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>bus_a</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>HYDRO_GEN</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES_A</name></t_object>
  <t_object><object_id>31</object_id><class_id>8</class_id><name>RES_B</name></t_object>
  <t_object><object_id>40</object_id><class_id>9</class_id><name>WW_AB</name></t_object>
  <t_collection>
    <collection_id>12</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_collection>
    <collection_id>10</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Head Storage</name>
  </t_collection>
  <t_collection>
    <collection_id>104</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storage From</name>
  </t_collection>
  <t_collection>
    <collection_id>105</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storage To</name>
  </t_collection>
  <t_collection>
    <collection_id>93</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storages</name>
  </t_collection>
  <t_membership>
    <membership_id>500</membership_id>
    <collection_id>12</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>501</membership_id>
    <collection_id>10</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>510</membership_id>
    <collection_id>104</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>511</membership_id>
    <collection_id>105</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>520</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>521</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_property>
    <property_id>100</property_id>
    <collection_id>93</collection_id>
    <name>Max Volume</name>
  </t_property>
  <t_property>
    <property_id>101</property_id>
    <collection_id>93</collection_id>
    <name>Initial Volume</name>
  </t_property>
  <t_data>
    <data_id>1</data_id>
    <membership_id>520</membership_id>
    <property_id>100</property_id>
    <value>500</value>
  </t_data>
  <t_data>
    <data_id>2</data_id>
    <membership_id>520</membership_id>
    <property_id>101</property_id>
    <value>300</value>
  </t_data>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_HYDRO_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    return bundle, xml_path


def test_extract_reservoirs_with_t_data_fallback(tmp_path: Path) -> None:
    """RES_A's Max Volume / Initial Volume come from t_data."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    reservoirs = extract_reservoirs(db, bundle)
    assert {r.name for r in reservoirs} == {"RES_A", "RES_B"}
    res_a = next(r for r in reservoirs if r.name == "RES_A")
    assert res_a.emax == 500.0
    assert res_a.eini == 300.0
    # RES_B has no t_data → all bounds default to zero.
    res_b = next(r for r in reservoirs if r.name == "RES_B")
    assert res_b.emax == 0.0


def test_extract_waterways(tmp_path: Path) -> None:
    """WW_AB endpoints resolve from Storage From / Storage To."""
    _, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    waterways = extract_waterways(db)
    assert len(waterways) == 1
    ww = waterways[0]
    assert ww.name == "WW_AB"
    assert ww.storage_from == "RES_A"
    assert ww.storage_to == "RES_B"


# ---------------------------------------------------------------------------
# Vert_*-referenced-by-UC keep-as-waterway behaviour (data-driven, not a
# fixed allowlist).  See ``_vert_waterways_referenced_by_constraints`` and
# the gated collapse branch in ``extract_waterways``.
# ---------------------------------------------------------------------------

_VERT_KEEP_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_class><class_id>9</class_id><name>Waterway</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES_KEEP</name></t_object>
  <t_object><object_id>31</object_id><class_id>8</class_id><name>RES_DROP</name></t_object>
  <t_object><object_id>40</object_id><class_id>9</class_id><name>Vert_KEEP</name></t_object>
  <t_object><object_id>41</object_id><class_id>9</class_id><name>Vert_DROP</name></t_object>
  <t_object><object_id>50</object_id><class_id>70</class_id><name>KEEPmin</name></t_object>
  <t_collection>
    <collection_id>93</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storages</name>
  </t_collection>
  <t_collection>
    <collection_id>104</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storage From</name>
  </t_collection>
  <t_collection>
    <collection_id>106</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_membership>
    <membership_id>520</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>521</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>540</membership_id>
    <collection_id>104</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>541</membership_id>
    <collection_id>104</collection_id>
    <parent_object_id>41</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>560</membership_id>
    <collection_id>106</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>50</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def test_vert_kept_when_referenced_by_userconstraint(tmp_path: Path) -> None:
    """A ``Vert_*`` waterway referenced by some UserConstraint via a
    ``Constraints`` membership must stay emitted as a real Waterway
    (NOT collapsed onto Junction.drain), so the UC's
    ``waterway("Vert_<X>").flow`` term has an LP column to bind to.

    A second ``Vert_*`` with NO UC reference must still collapse onto
    ``junction_drain_configs_out`` — proving the gate is data-driven,
    not a fixed allowlist.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_VERT_KEEP_XML)
    db = load_xml(xml_path)

    junction_drain_configs: dict[str, dict[str, float | None]] = {}
    waterways = extract_waterways(
        db,
        junction_drain_configs_out=junction_drain_configs,
    )

    names = {w.name for w in waterways}
    # Vert_KEEP is UC-referenced → kept as Waterway, routed to ocean drain.
    assert "Vert_KEEP" in names
    kept = next(w for w in waterways if w.name == "Vert_KEEP")
    assert kept.storage_from == "RES_KEEP"
    assert kept.storage_to == "RES_KEEP_ocean"
    # Vert_DROP has NO UC reference → collapsed away, source picks up
    # a junction_drain_configs entry.
    assert "Vert_DROP" not in names
    assert "RES_DROP" in junction_drain_configs
    # And the kept arc's source did NOT get a junction-drain entry (would
    # be a double-drain bug).
    assert "RES_KEEP" not in junction_drain_configs


_VERT_CASCADE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_class><class_id>9</class_id><name>Waterway</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES_UP</name></t_object>
  <t_object><object_id>31</object_id><class_id>8</class_id><name>RES_DOWN</name></t_object>
  <t_object><object_id>40</object_id><class_id>9</class_id><name>Vert_CASC</name></t_object>
  <t_object><object_id>50</object_id><class_id>70</class_id><name>CASCmin</name></t_object>
  <t_collection>
    <collection_id>93</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storages</name>
  </t_collection>
  <t_collection>
    <collection_id>104</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storage From</name>
  </t_collection>
  <t_collection>
    <collection_id>105</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storage To</name>
  </t_collection>
  <t_collection>
    <collection_id>106</collection_id>
    <parent_class_id>9</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_membership>
    <membership_id>520</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>521</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>540</membership_id>
    <collection_id>104</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>541</membership_id>
    <collection_id>105</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>560</membership_id>
    <collection_id>106</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>50</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def test_vert_kept_arc_with_storage_to_routes_cascade(tmp_path: Path) -> None:
    """A UC-referenced ``Vert_*`` that has a PLEXOS ``Storage To`` must
    default to cascade routing (downstream reservoir), NOT ocean.

    This is the topology-faithful default for kept arcs: PLEXOS routes
    the spill into the next cascade reservoir (e.g.
    ``Vert_PANGUE → ANGOSTURA``, ``Vert_B_Maule → COLBUN``), and the UC
    physics depends on it (ocean-redirecting recoverable spill makes
    the LP over-turbine the upstream station to meet a min-flow floor).
    Sister terminal-arc behaviour is covered by
    ``test_vert_kept_when_referenced_by_userconstraint`` (no Storage
    To → falls back to ocean).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_VERT_CASCADE_XML)
    db = load_xml(xml_path)

    junction_drain_configs: dict[str, dict[str, float | None]] = {}
    waterways = extract_waterways(
        db,
        junction_drain_configs_out=junction_drain_configs,
    )
    casc = next(w for w in waterways if w.name == "Vert_CASC")
    # Cascade routing: keep PLEXOS-published Storage To, NOT _ocean.
    assert casc.storage_from == "RES_UP"
    assert casc.storage_to == "RES_DOWN"
    # And no spurious ocean sink for the source either.
    assert "RES_UP" not in junction_drain_configs


def test_vert_waterways_referenced_by_constraints_helper(tmp_path: Path) -> None:
    """``_vert_waterways_referenced_by_constraints`` returns exactly the
    set of ``Vert_*`` names that appear as a parent in a ``Constraints``
    membership."""
    from plexos2gtopt.parsers import _vert_waterways_referenced_by_constraints

    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_VERT_KEEP_XML)
    db = load_xml(xml_path)

    assert _vert_waterways_referenced_by_constraints(db) == frozenset({"Vert_KEEP"})


def test_extract_junctions_mirror_reservoirs() -> None:
    """One Junction per Reservoir, same name."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="A"),
        ReservoirSpec(object_id=2, name="B"),
    )
    junctions = extract_junctions(reservoirs)
    assert [j.name for j in junctions] == ["A", "B"]


def test_extract_turbines(tmp_path: Path) -> None:
    """HYDRO_GEN's Head Storage = RES_A → one Turbine entry."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    turbines = extract_turbines(db, bundle)
    assert len(turbines) == 1
    t = turbines[0]
    assert t.generator_name == "HYDRO_GEN"
    assert t.reservoir_name == "RES_A"


def test_extract_flows_filters_unknown_junctions(tmp_path: Path) -> None:
    """A Flow column with no matching Junction is dropped, not orphaned."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # Inflow CSV ships data for RES_A (known) and Filt_X (unknown).
    (tmp_path / "Hydro_WaterFlows.csv").write_text(
        "YEAR,MONTH,DAY,PERIOD,RES_A,Filt_X\n"
        "2026,1,1,1,15.0,99.0\n"
        "2026,1,1,2,16.0,99.0\n"
    )
    flows = extract_flows(db, bundle, known_junctions=frozenset({"RES_A", "RES_B"}))
    assert {f.junction_name for f in flows} == {"RES_A"}
    f_a = next(f for f in flows if f.junction_name == "RES_A")
    assert f_a.discharge_profile[0] == 15.0
    assert f_a.discharge_profile[1] == 16.0


def test_writer_reservoir_uses_junction_ref() -> None:
    """build_reservoir_array attaches each reservoir to a same-named Junction."""
    reservoirs = (ReservoirSpec(object_id=1, name="A", emax=100.0, eini=50.0),)
    out = build_reservoir_array(reservoirs)
    assert out[0]["junction"] == "A"
    assert out[0]["emax"] == 100.0
    assert out[0]["eini"] == 50.0
    # emin / efin omitted when zero (no soft-cap floor).
    assert "emin" not in out[0]
    assert "efin" not in out[0]


def test_writer_reservoir_emits_efin_and_scost() -> None:
    """Non-zero ``efin`` and ``spill_penalty_per_mwh`` survive into the JSON."""
    reservoirs = (
        ReservoirSpec(
            object_id=2,
            name="B",
            emax=200.0,
            eini=100.0,
            efin=75.0,
            spill_penalty_per_mwh=5.5,
        ),
    )
    out = build_reservoir_array(reservoirs)
    assert out[0]["efin"] == 75.0
    # spillway_cost = spill_penalty × DEFAULT_FP_MED (=1.0 today).
    assert out[0]["spillway_cost"] == 5.5
    # No explicit spillway_capacity: gtopt's storage_lp.hpp defaults
    # the per-block drain upper bound to DblMax when the field is
    # unset — setting just the cost is enough to activate the drain.
    assert "spillway_capacity" not in out[0]


def test_writer_reservoir_default_internal_drain() -> None:
    """Reservoirs without a PLEXOS spill penalty get the PLEXOS-faithful
    default internal drain at ``spillway_cost = 0.0`` (free out-of-basin
    "spill-to-sea" valve).

    Cost is 0 because the drain spills OUT of the basin (water is gone, so
    a free drain cannot waste usable water) and a positive cost would push
    the reservoir's marginal water value artificially negative on spill.
    This replicates PLEXOS's per-storage spill column and matches
    plp2gtopt.  The companion ``Vert_*`` → ``<source>_ocean`` spill-out
    waterways are dropped in ``extract_waterways`` when the drain is on,
    so each reservoir has exactly ONE basin exit (no double escape path).
    ``spillway_capacity`` is left UNSET — ``storage_lp.hpp`` defaults the
    per-block drain upper bound to DblMax, so setting just the cost
    activates the drain.
    """
    reservoirs = (
        ReservoirSpec(
            object_id=2,
            name="B",
            emax=200.0,
            eini=100.0,
        ),
    )
    out = build_reservoir_array(reservoirs)
    assert out[0]["spillway_cost"] == 0.0
    assert "spillway_capacity" not in out[0]


def test_writer_reservoir_never_drain_disables_internal_drain() -> None:
    """``never_drain`` (ELTORO) keeps the drain OFF: ``spillway_cost``
    UNSET and ``spillway_capacity = 0`` — water leaves only via turbines.
    """
    reservoirs = (
        ReservoirSpec(
            object_id=3,
            name="ELTORO",
            emax=12000.0,
            eini=12000.0,
            never_drain=True,
        ),
    )
    out = build_reservoir_array(reservoirs)
    assert "spillway_cost" not in out[0]
    assert out[0]["spillway_capacity"] == 0.0


def test_writer_waterway_skips_unresolved_endpoints() -> None:
    """Waterways without both junctions resolved are silently dropped."""
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="orphan_from",
            storage_from=None,
            storage_to="B",
            fmax=10.0,
        ),
        WaterwaySpec(
            object_id=2,
            name="orphan_to",
            storage_from="A",
            storage_to=None,
            fmax=10.0,
        ),
        WaterwaySpec(
            object_id=3,
            name="good",
            storage_from="A",
            storage_to="B",
            fmin=1.0,
            fmax=10.0,
        ),
    )
    out = build_waterway_array(waterways)
    assert [w["name"] for w in out] == ["good"]
    assert out[0]["fmin"] == 1.0


def test_writer_junction_array_minimal() -> None:
    """Junctions emit just uid + name unless drain=True."""
    junctions = (
        JunctionSpec(name="A"),
        JunctionSpec(name="B", drain=True),
    )
    out = build_junction_array(junctions)
    assert out[0] == {"uid": 1, "name": "A"}
    assert out[1] == {"uid": 2, "name": "B", "drain": True}


def test_writer_waterway_array_maps_endpoints() -> None:
    """Waterway uses junction_a / junction_b refs (not storage names)."""
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="ww",
            storage_from="A",
            storage_to="B",
            fmax=50.0,
        ),
    )
    out = build_waterway_array(waterways)
    assert out[0]["junction_a"] == "A"
    assert out[0]["junction_b"] == "B"
    assert out[0]["fmax"] == 50.0


def test_writer_turbine_array_links_gen_and_reservoir() -> None:
    """Turbine carries generator + main_reservoir + waterway refs.

    The writer drops turbines whose reservoir has no draining Waterway
    (PLEXOS terminal hydro plants like CANUTILLAR; thermal pseudo-
    turbines on gas-storage Storage objects).  Pass a matching
    WaterwaySpec so the turbine survives.
    """
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="ww_RES_A",
            storage_from="RES_A",
            storage_to="SEA",
        ),
    )
    turbines = (
        TurbineSpec(
            generator_name="HYDRO_GEN",
            reservoir_name="RES_A",
            production_factor=1.2,
        ),
    )
    out = build_turbine_array(turbines, waterways)
    assert out[0]["generator"] == "HYDRO_GEN"
    assert out[0]["main_reservoir"] == "RES_A"
    assert out[0]["waterway"] == "ww_RES_A"
    assert out[0]["production_factor"] == 1.2


def test_writer_turbine_builtin_waterway_emits_junctions() -> None:
    """Built-in waterway mode (extra_waterways passed): the turbine carries
    its own flow arc via junction_a/junction_b — no separate penstock
    Waterway, no ``waterway`` field."""
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="Vert_RES_A",  # spillway, not a turbine route
            storage_from="RES_A",
            storage_to="RES_B",
        ),
    )
    turbines = (
        TurbineSpec(
            generator_name="HYDRO_GEN",
            reservoir_name="RES_A",
            production_factor=1.2,
            tail_reservoir_name="RES_B",
        ),
    )
    extra_ww: list[dict] = []
    out = build_turbine_array(turbines, waterways, extra_waterways=extra_ww)
    assert out[0]["junction_a"] == "RES_A"
    assert out[0]["junction_b"] == "RES_B"
    assert "waterway" not in out[0]
    assert out[0]["production_factor"] == 1.2
    # No penstock waterway clone is appended any more.
    assert not extra_ww


def test_writer_terminal_turbine_drains_no_junction_b() -> None:
    """A terminal turbine (no tail and no draining waterway) is emitted with
    junction_a only — junction_b unset means the turbined flow drains out of
    the system, with no synthesised ocean junction."""
    turbines = (
        TurbineSpec(
            generator_name="TERMINAL_GEN",
            reservoir_name="RES_SEA",
            production_factor=2.0,
        ),
    )
    out = build_turbine_array(turbines, (), extra_waterways=[])
    assert len(out) == 1
    assert out[0]["junction_a"] == "RES_SEA"
    assert "junction_b" not in out[0]
    assert "waterway" not in out[0]


def test_writer_flow_array_shape() -> None:
    """Flow ``discharge`` is a [[stage_blocks]] matrix for a single-stage day."""
    flows = (
        FlowSpec(
            name="inflow_X",
            junction_name="X",
            discharge_profile=tuple(range(24)),
        ),
    )
    out = build_flow_array(flows)
    assert out[0]["junction"] == "X"
    # outer scene = 1, then stage = 1, then 24 blocks.
    assert len(out[0]["discharge"]) == 1
    assert len(out[0]["discharge"][0]) == 1
    assert len(out[0]["discharge"][0][0]) == 24
    assert out[0]["discharge"][0][0][5] == 5
