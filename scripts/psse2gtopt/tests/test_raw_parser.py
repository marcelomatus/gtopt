"""Unit tests for the PSS/E RAW parser."""

from __future__ import annotations

from pathlib import Path

import pytest

from psse2gtopt.raw_parser import (
    _is_terminator,
    floor_reactance,
    parse_raw,
    rating_to_tmax,
)


def test_synthetic_section_counts(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    assert case.case.rev == 33
    assert case.case.sbase == pytest.approx(100.0)
    assert case.case.base_freq == pytest.approx(60.0)
    assert len(case.buses) == 6
    assert len(case.loads) == 6
    assert len(case.gens) == 4
    assert len(case.branches) == 4
    assert len(case.transformers) == 3


def test_synthetic_bus_types(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    by_num = {b.number: b for b in case.buses}
    assert by_num[1].type_tag == "slack"
    assert by_num[2].type_tag == "pv"
    assert by_num[3].type_tag == "pq"
    assert by_num[5].is_isolated
    assert by_num[3].base_kv == pytest.approx(138.0)


def test_synthetic_load_fields(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    # Bus 3 has two loads: an in-service 100 MW and an out-of-service 999 MW.
    bus3 = [load for load in case.loads if load.bus == 3]
    assert {load.ident for load in bus3} == {"1", "2"}
    in_service = next(load for load in bus3 if load.ident == "1")
    assert in_service.status == 1
    assert in_service.pl == pytest.approx(100.0)
    oos = next(load for load in bus3 if load.ident == "2")
    assert oos.status == 0
    assert oos.pl == pytest.approx(999.0)


def test_synthetic_generator_fields(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    g1 = next(g for g in case.gens if g.bus == 1)
    assert g1.pmax == pytest.approx(200.0)  # PT (index 16)
    assert g1.pmin == pytest.approx(0.0)  # PB (index 17)
    assert g1.status == 1  # STAT (index 14)
    g2b = next(g for g in case.gens if g.bus == 2 and g.ident == "2")
    assert g2b.status == 0  # out of service
    cond = next(g for g in case.gens if g.bus == 3)
    assert cond.pmax == pytest.approx(0.0)  # synchronous condenser


def test_synthetic_branch_fields(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    b1 = next(b for b in case.branches if b.ckt == "1")
    assert b1.x == pytest.approx(0.1)  # X (index 4)
    assert b1.rate_a == pytest.approx(200.0)  # RATEA (index 6)
    assert b1.status == 1  # ST (index 13)
    b2 = next(b for b in case.branches if b.ckt == "2")
    assert b2.status == 0  # out of service
    b3 = next(b for b in case.branches if b.ckt == "3")
    assert b3.x == pytest.approx(0.0)  # raw zero reactance (floored downstream)


def test_synthetic_transformer_cz_conversion(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    by_buses = {(t.bus_i, t.bus_j, t.bus_k): t for t in case.transformers}
    # CZ=1: x12 already on system base.
    cz1 = by_buses[(2, 3, 0)]
    assert cz1.windings == 2
    assert cz1.x12 == pytest.approx(0.05)
    # CZ=2: x12 = 0.1 on a 50 MVA winding base -> 0.1 * 100/50 = 0.2 system base.
    cz2 = by_buses[(3, 4, 0)]
    assert cz2.windings == 2
    assert cz2.x12 == pytest.approx(0.2)


def test_synthetic_three_winding(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    tr3 = next(t for t in case.transformers if t.windings == 3)
    assert (tr3.bus_i, tr3.bus_j, tr3.bus_k) == (1, 3, 6)
    assert tr3.x12 == pytest.approx(0.2)
    assert tr3.x23 == pytest.approx(0.3)
    assert tr3.x31 == pytest.approx(0.4)
    assert tr3.rate1 == pytest.approx(100.0)
    assert tr3.rate2 == pytest.approx(60.0)
    assert tr3.rate3 == pytest.approx(40.0)


def test_synthetic_branch_emergency_ratings(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    b1 = next(b for b in case.branches if b.ckt == "1")
    assert b1.rate_a == pytest.approx(200.0)
    assert b1.rate_b == pytest.approx(250.0)
    assert b1.rate_c == pytest.approx(300.0)
    assert b1.rating("A") == pytest.approx(200.0)
    assert b1.rating("B") == pytest.approx(250.0)
    assert b1.rating("C") == pytest.approx(300.0)
    # A branch whose RATEC is 0 falls back to RATEA.
    b3 = next(b for b in case.branches if b.ckt == "3")
    assert b3.rating("C") == pytest.approx(b3.rate_a)


def test_synthetic_transformer_winding_ratings(synthetic_raw: Path) -> None:
    case = parse_raw(synthetic_raw)
    cz1 = next(t for t in case.transformers if (t.bus_i, t.bus_j) == (2, 3))
    assert cz1.winding_rating(1, "A") == pytest.approx(180.0)
    assert cz1.winding_rating(1, "B") == pytest.approx(200.0)
    assert cz1.winding_rating(1, "C") == pytest.approx(220.0)


def test_is_terminator() -> None:
    assert _is_terminator("0 / END OF BUS DATA, BEGIN LOAD DATA")
    assert _is_terminator(" 0 /End of Load data, Begin Fixed shunt data")
    assert _is_terminator("0")
    assert not _is_terminator("   801,'CRU-230     ', 230.0000,1")
    assert not _is_terminator(" 0.00000E+0, 3.75200E-1,    37.50")
    assert not _is_terminator("1.00000,  10.000")
    assert not _is_terminator("")


def test_floor_reactance() -> None:
    assert floor_reactance(0.0) == pytest.approx(1.0e-5)
    assert floor_reactance(-0.0) == pytest.approx(1.0e-5)
    assert floor_reactance(1.0e-9) == pytest.approx(1.0e-5)
    assert floor_reactance(-1.0e-9) == pytest.approx(-1.0e-5)
    assert floor_reactance(0.05) == pytest.approx(0.05)
    assert floor_reactance(-0.05) == pytest.approx(-0.05)


def test_rating_to_tmax() -> None:
    assert rating_to_tmax(438.21) == pytest.approx(438.21)
    assert rating_to_tmax(0.0) == pytest.approx(99999.0)


def test_parse_raw_xz(synthetic_raw: Path, tmp_path: Path) -> None:
    """A .xz-compressed RAW reads transparently (and resolves from the plain path)."""
    import lzma  # pylint: disable=import-outside-toplevel

    xz = tmp_path / "synthetic.raw.xz"
    xz.write_bytes(lzma.compress(synthetic_raw.read_bytes()))
    case = parse_raw(xz)
    assert case.case.rev == 33 and len(case.buses) == 6
    # Passing the plain path resolves to the .xz sibling.
    case2 = parse_raw(tmp_path / "synthetic.raw")
    assert len(case2.buses) == 6


def test_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        parse_raw(tmp_path / "does_not_exist.raw")


def test_too_short_raises(tmp_path: Path) -> None:
    bad = tmp_path / "bad.raw"
    bad.write_text("0, 100.0, 33\n", encoding="latin-1")
    with pytest.raises(ValueError):
        parse_raw(bad)


def test_rejects_v35_rawx_header(tmp_path: Path) -> None:
    """A leading ``@!`` column-header comment marks the unsupported v35 layout."""
    v35 = tmp_path / "v35.raw"
    v35.write_text(
        "@!IC,SBASE,REV,XFRRAT,NXFRAT,BASFRQ\n"
        "0,  100.00, 35,     0,     1, 60.00     / PSS(R)E-35.6\n"
        "TITLE LINE 1\n"
        "TITLE LINE 2\n",
        encoding="latin-1",
    )
    with pytest.raises(ValueError, match="v34\\+/RAWX"):
        parse_raw(v35)


def test_rejects_rev34_plus(tmp_path: Path) -> None:
    """Even without the ``@!`` header, REV >= 34 is rejected."""
    raw = tmp_path / "rev34.raw"
    raw.write_text(
        "0,  100.00, 34,     0,     1, 60.00     / PSS(R)E-34\n"
        "TITLE 1\nTITLE 2\n 0 /End of Bus data\n",
        encoding="latin-1",
    )
    with pytest.raises(ValueError, match="not\\s+supported|v34"):
        parse_raw(raw)


def test_ieee14_rev32(ieee14_raw: Path) -> None:
    """Rev-32 bus records have 9 fields (no voltage limits) — must still parse."""
    case = parse_raw(ieee14_raw)
    assert case.case.rev == 32
    assert len(case.buses) == 14
    assert len(case.gens) == 5
    assert len(case.branches) == 16
    assert len(case.transformers) == 4
    total_pl = sum(load.pl for load in case.loads if load.status == 1)
    assert total_pl == pytest.approx(223.7, abs=0.1)


def test_ieee39_rev33(ieee39_raw: Path) -> None:
    case = parse_raw(ieee39_raw)
    assert case.case.rev == 33
    assert len(case.buses) == 39
    assert len(case.gens) == 14
    total_pl = sum(load.pl for load in case.loads if load.status == 1)
    assert total_pl == pytest.approx(5856.8, abs=0.1)
