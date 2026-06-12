"""Unit tests for ror_equivalence_parser module."""

from __future__ import annotations

from pathlib import Path

import pytest

from ..ror_equivalence_parser import (
    RorSpec,
    parse_ror_equivalence_file,
    parse_ror_selection,
    pasada_unscale_map,
    resolve_ror_reservoir_spec,
)


def _write(tmp_path: Path, content: str) -> Path:
    """Write *content* to a CSV inside *tmp_path* and return the path."""
    path = tmp_path / "ror_equivalence.csv"
    path.write_text(content, encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file — happy paths
# ---------------------------------------------------------------------------


def test_parse_happy_path_basic(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor\nCentralA,1.23,0.85\nCentralB,0.45,1.40\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec == {
        "CentralA": RorSpec(vmax_hm3=1.23, production_factor=0.85),
        "CentralB": RorSpec(vmax_hm3=0.45, production_factor=1.40),
    }
    # File order preserved.
    assert list(spec.keys()) == ["CentralA", "CentralB"]


def test_parse_ignores_unknown_columns(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,notes,extra\nCentralA,1.0,0.85,hello,world\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec == {"CentralA": RorSpec(vmax_hm3=1.0, production_factor=0.85)}


def test_parse_description_column_ignored(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,description\n"
        "CentralA,1.0,0.85,from operator ficha técnica\n",
    )
    assert parse_ror_equivalence_file(path) == {
        "CentralA": RorSpec(vmax_hm3=1.0, production_factor=0.85)
    }


def test_parse_comment_column_ignored_legacy(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,comment\n"
        "CentralA,1.0,0.85,legacy free-form note\n",
    )
    assert parse_ror_equivalence_file(path) == {
        "CentralA": RorSpec(vmax_hm3=1.0, production_factor=0.85)
    }


def test_parse_enabled_truthy_values(tmp_path: Path) -> None:
    content = (
        "name,vmax_hm3,production_factor,enabled\n"
        "A,1.0,0.10,true\n"
        "B,2.0,0.20,t\n"
        "C,3.0,0.30,yes\n"
        "D,4.0,0.40,y\n"
        "E,5.0,0.50,on\n"
        "F,6.0,0.60,1\n"
        "G,7.0,0.70,True\n"
    )
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {
        "A": RorSpec(vmax_hm3=1.0, production_factor=0.10),
        "B": RorSpec(vmax_hm3=2.0, production_factor=0.20),
        "C": RorSpec(vmax_hm3=3.0, production_factor=0.30),
        "D": RorSpec(vmax_hm3=4.0, production_factor=0.40),
        "E": RorSpec(vmax_hm3=5.0, production_factor=0.50),
        "F": RorSpec(vmax_hm3=6.0, production_factor=0.60),
        "G": RorSpec(vmax_hm3=7.0, production_factor=0.70),
    }


def test_parse_enabled_falsy_values_skip(tmp_path: Path) -> None:
    content = (
        "name,vmax_hm3,production_factor,enabled\n"
        "Keep,1.0,0.85,true\n"
        "SkipFalse,2.0,0.90,false\n"
        "SkipUpper,3.0,0.95,FALSE\n"
        "SkipNo,4.0,1.00,no\n"
        "SkipZero,5.0,1.05,0\n"
    )
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {"Keep": RorSpec(vmax_hm3=1.0, production_factor=0.85)}


def test_parse_enabled_empty_or_unset_is_enabled(tmp_path: Path) -> None:
    # Explicit empty string in the enabled column → treated as enabled.
    content = "name,vmax_hm3,production_factor,enabled\nA,1.0,0.10,\nB,2.0,0.20,   \n"
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {
        "A": RorSpec(vmax_hm3=1.0, production_factor=0.10),
        "B": RorSpec(vmax_hm3=2.0, production_factor=0.20),
    }


def test_parse_skipped_row_allows_bad_numbers(tmp_path: Path) -> None:
    # A disabled row is skipped before numeric validation runs.
    content = (
        "name,vmax_hm3,production_factor,enabled\n"
        "Kept,1.0,0.85,true\n"
        "Disabled,abc,xyz,false\n"
    )
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {"Kept": RorSpec(vmax_hm3=1.0, production_factor=0.85)}


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file — error paths
# ---------------------------------------------------------------------------


def test_parse_missing_file_raises(tmp_path: Path) -> None:
    missing = tmp_path / "does_not_exist.csv"
    with pytest.raises(FileNotFoundError, match="ROR equivalence file not found"):
        parse_ror_equivalence_file(missing)


def test_parse_missing_name_column(tmp_path: Path) -> None:
    path = _write(tmp_path, "foo,vmax_hm3,production_factor\nCentralA,1.0,0.85\n")
    with pytest.raises(ValueError, match=r"missing required column.*'name'"):
        parse_ror_equivalence_file(path)


def test_parse_missing_vmax_column(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,production_factor\nCentralA,0.85\n")
    with pytest.raises(ValueError, match=r"missing required column.*'vmax_hm3'"):
        parse_ror_equivalence_file(path)


def test_parse_missing_production_factor_column(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nCentralA,1.0\n")
    with pytest.raises(
        ValueError, match=r"missing required column.*'production_factor'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_empty_name_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\n,1.0,0.85\n")
    with pytest.raises(ValueError, match=r"line 2: empty name"):
        parse_ror_equivalence_file(path)


def test_parse_whitespace_only_name_raises(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor\nA,1.0,0.85\n   ,2.0,0.90\n",
    )
    with pytest.raises(ValueError, match=r"line 3: empty name"):
        parse_ror_equivalence_file(path)


def test_parse_empty_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,,0.85\n")
    with pytest.raises(
        ValueError, match=r"line 2: missing vmax_hm3 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_non_numeric_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,abc,0.85\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: non-numeric vmax_hm3 'abc' for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_zero_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,0,0.85\n")
    with pytest.raises(
        ValueError, match=r"line 2: vmax_hm3 must be > 0 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_negative_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,-1.5,0.85\n")
    with pytest.raises(
        ValueError, match=r"line 2: vmax_hm3 must be > 0 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_empty_production_factor_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,1.5,\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: missing production_factor for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_non_numeric_production_factor_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,1.5,bad\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: non-numeric production_factor 'bad' for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_zero_production_factor_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,1.5,0\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: production_factor must be > 0 for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_negative_production_factor_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3,production_factor\nCentralA,1.5,-0.1\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: production_factor must be > 0 for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_duplicate_name_raises(tmp_path: Path) -> None:
    content = (
        "name,vmax_hm3,production_factor\n"
        "CentralA,1.0,0.85\n"
        "CentralB,2.0,0.90\n"
        "CentralA,3.0,0.95\n"
    )
    path = _write(tmp_path, content)
    with pytest.raises(ValueError, match=r"line 4: duplicate central name 'CentralA'"):
        parse_ror_equivalence_file(path)


# ---------------------------------------------------------------------------
# parse_ror_selection
# ---------------------------------------------------------------------------


def test_selection_none_input_returns_none() -> None:
    assert parse_ror_selection(None) is None


@pytest.mark.parametrize("value", ["", "   ", "none", "NONE", "None", "  none  "])
def test_selection_none_like_returns_none(value: str) -> None:
    assert parse_ror_selection(value) is None


@pytest.mark.parametrize("value", ["all", "ALL", "All", "  all  "])
def test_selection_all_returns_empty_frozenset(value: str) -> None:
    result = parse_ror_selection(value)
    assert result == frozenset()
    assert isinstance(result, frozenset)


def test_selection_explicit_list() -> None:
    result = parse_ror_selection("A,B,C")
    assert result == frozenset({"A", "B", "C"})
    assert isinstance(result, frozenset)


def test_selection_trims_whitespace_and_drops_empty_tokens() -> None:
    result = parse_ror_selection("A, ,B")
    assert result == frozenset({"A", "B"})


def test_selection_trims_leading_trailing_whitespace() -> None:
    result = parse_ror_selection("  A , B  ,  C ")
    assert result == frozenset({"A", "B", "C"})


def test_selection_single_name() -> None:
    assert parse_ror_selection("Solo") == frozenset({"Solo"})


def test_selection_only_commas_raises() -> None:
    with pytest.raises(ValueError, match=r"--ror-as-reservoirs"):
        parse_ror_selection(",")


def test_selection_only_commas_and_whitespace_raises() -> None:
    with pytest.raises(ValueError, match=r"--ror-as-reservoirs"):
        parse_ror_selection(" , , ")


# ---------------------------------------------------------------------------
# resolve_ror_reservoir_spec
# ---------------------------------------------------------------------------


def _central(
    name: str,
    *,
    ctype: str = "pasada",
    bus: int = 1,
    efficiency: float = 1.0,
    pmax: float = 100.0,
) -> dict:
    """Minimal central dict accepted by resolve_ror_reservoir_spec."""
    return {
        "name": name,
        "type": ctype,
        "bus": bus,
        "efficiency": efficiency,
        "pmax": pmax,
    }


def _csv(tmp_path: Path, rows: list[tuple[str, float, float]]) -> Path:
    path = tmp_path / "ror_equiv.csv"
    body = "".join(f"{n},{v},{p}\n" for n, v, p in rows)
    path.write_text("name,vmax_hm3,production_factor\n" + body, encoding="utf-8")
    return path


def test_resolve_disabled_returns_empty() -> None:
    """No selection → empty dict, no CSV opened."""
    assert not resolve_ror_reservoir_spec({}, [_central("A")])


def test_resolve_none_short_circuits(tmp_path: Path) -> None:
    """Explicit ``none`` short-circuits before any CSV lookup."""
    result = resolve_ror_reservoir_spec(
        {
            "ror_as_reservoirs": "none",
            "ror_as_reservoirs_file": tmp_path / "does_not_exist.csv",
        },
        [_central("A")],
    )
    assert not result


def test_resolve_selection_without_csv_falls_back_to_packaged_default() -> None:
    """Omitting ``ror_as_reservoirs_file`` falls back to the packaged CSV.

    The packaged whitelist does not contain a central named ``A`` (the
    test fixture), so this must surface the downstream "central not in
    whitelist" error rather than a "missing --ror-as-reservoirs-file"
    one — proving the default path was discovered and loaded.
    """
    with pytest.raises(ValueError, match=r"not in whitelist CSV"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "A"},
            [_central("A")],
        )


def test_resolve_all_happy_path(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("A", 1.5, 0.42), ("B", 2.0, 0.85)])
    resolved = resolve_ror_reservoir_spec(
        {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
        [_central("A"), _central("B", ctype="serie")],
    )
    assert resolved == {
        "A": RorSpec(vmax_hm3=1.5, production_factor=0.42),
        "B": RorSpec(vmax_hm3=2.0, production_factor=0.85),
    }


def test_resolve_explicit_selection_narrows(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("A", 1.5, 0.42), ("B", 2.0, 0.85)])
    resolved = resolve_ror_reservoir_spec(
        {"ror_as_reservoirs": "A", "ror_as_reservoirs_file": path},
        [_central("A"), _central("B")],
    )
    assert list(resolved.keys()) == ["A"]


def test_resolve_unknown_name_raises(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("A", 1.5, 0.42)])
    with pytest.raises(ValueError, match="Ghost"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "Ghost", "ror_as_reservoirs_file": path},
            [_central("A")],
        )


def test_resolve_missing_from_case_raises(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("Ghost", 1.5, 0.42)])
    with pytest.raises(ValueError, match="Ghost"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A")],
        )


def test_resolve_rejects_embalse(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("Dam", 1.5, 0.42)])
    with pytest.raises(ValueError, match="type"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("Dam", ctype="embalse")],
        )


def test_resolve_rejects_bus_le_zero(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("A", 1.5, 0.42)])
    with pytest.raises(ValueError, match="bus"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", bus=0)],
        )


def test_resolve_rejects_efficiency_le_zero(tmp_path: Path) -> None:
    path = _csv(tmp_path, [("A", 1.5, 0.42)])
    with pytest.raises(ValueError, match="efficiency"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", efficiency=0.0)],
        )


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file — optional pmax_mw column
# ---------------------------------------------------------------------------


def test_parse_pmax_mw_column_present(tmp_path: Path) -> None:
    """Optional pmax_mw column is parsed as float when provided."""
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,pmax_mw\nA,1.0,0.5,136.0\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec == {
        "A": RorSpec(vmax_hm3=1.0, production_factor=0.5, pmax_mw=136.0),
    }


def test_parse_pmax_mw_column_absent_defaults_none(tmp_path: Path) -> None:
    """When the column is missing entirely, pmax_mw is None."""
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor\nA,1.0,0.5\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec["A"].pmax_mw is None


def test_parse_pmax_mw_blank_is_none(tmp_path: Path) -> None:
    """A blank pmax_mw cell disables the cross-check for that row."""
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,pmax_mw\nA,1.0,0.5,\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec["A"].pmax_mw is None


def test_parse_pmax_mw_non_numeric_raises(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,pmax_mw\nA,1.0,0.5,not-a-number\n",
    )
    with pytest.raises(ValueError, match="pmax_mw"):
        parse_ror_equivalence_file(path)


def test_parse_pmax_mw_non_positive_raises(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,production_factor,pmax_mw\nA,1.0,0.5,0\n",
    )
    with pytest.raises(ValueError, match="pmax_mw"):
        parse_ror_equivalence_file(path)


# ---------------------------------------------------------------------------
# resolve_ror_reservoir_spec — pmax cross-check
# ---------------------------------------------------------------------------


def _csv_with_pmax(
    tmp_path: Path, rows: list[tuple[str, float, float, float | None]]
) -> Path:
    path = tmp_path / "ror_equiv.csv"
    body = "".join(f"{n},{v},{p},{'' if px is None else px}\n" for n, v, p, px in rows)
    path.write_text(
        "name,vmax_hm3,production_factor,pmax_mw\n" + body, encoding="utf-8"
    )
    return path


def test_resolve_pmax_within_tolerance_silent(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Relative gap ≤ 5% → no warning is logged."""
    path = _csv_with_pmax(tmp_path, [("A", 1.5, 0.42, 100.0)])
    with caplog.at_level("WARNING", logger="plp2gtopt.ror_equivalence_parser"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", pmax=103.0)],  # 3% gap
        )
    assert not [r for r in caplog.records if "pmax" in r.getMessage()]


def test_resolve_pmax_mismatch_warns(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Relative gap > 5% → warning is logged but resolution still succeeds."""
    path = _csv_with_pmax(tmp_path, [("A", 1.5, 0.42, 100.0)])
    with caplog.at_level("WARNING", logger="plp2gtopt.ror_equivalence_parser"):
        resolved = resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", pmax=150.0)],  # 50% gap
        )
    assert "A" in resolved  # warning is informational only
    msgs = [r.getMessage() for r in caplog.records]
    assert any("pmax mismatch" in m and "'A'" in m for m in msgs), msgs


def test_resolve_pmax_plp_missing_warns(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """CSV has pmax_mw but PLP pmax is 0/missing → separate warning."""
    path = _csv_with_pmax(tmp_path, [("A", 1.5, 0.42, 100.0)])
    with caplog.at_level("WARNING", logger="plp2gtopt.ror_equivalence_parser"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", pmax=0.0)],
        )
    msgs = [r.getMessage() for r in caplog.records]
    assert any("cannot cross-check pmax" in m for m in msgs), msgs


def test_resolve_pmax_absent_no_check(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """No pmax_mw in CSV → never warn, regardless of PLP pmax value."""
    path = _csv(tmp_path, [("A", 1.5, 0.42)])
    with caplog.at_level("WARNING", logger="plp2gtopt.ror_equivalence_parser"):
        resolve_ror_reservoir_spec(
            {"ror_as_reservoirs": "all", "ror_as_reservoirs_file": path},
            [_central("A", pmax=0.0)],
        )
    assert not [r for r in caplog.records if "pmax" in r.getMessage()]


# ---------------------------------------------------------------------------
# pasada_unscale_map
# ---------------------------------------------------------------------------


def test_unscale_empty_when_no_promotions() -> None:
    assert not pasada_unscale_map({}, [_central("A")])


def test_unscale_only_pasadas_included() -> None:
    """Serie centrals already carry physical m³/s and must not be scaled."""
    resolved = {
        "Pasa": RorSpec(vmax_hm3=1.0, production_factor=0.5),
        "Ser": RorSpec(vmax_hm3=2.0, production_factor=0.8),
    }
    items = [
        _central("Pasa", ctype="pasada"),
        _central("Ser", ctype="serie"),
    ]
    result = pasada_unscale_map(resolved, items)
    assert result == {"Pasa": 2.0}  # 1.0 / 0.5


def test_unscale_multiple_pasadas() -> None:
    resolved = {
        "A": RorSpec(vmax_hm3=1.0, production_factor=0.25),
        "B": RorSpec(vmax_hm3=1.0, production_factor=1.25),
    }
    items = [_central("A"), _central("B")]
    result = pasada_unscale_map(resolved, items)
    assert result == pytest.approx({"A": 4.0, "B": 0.8})


def test_unscale_unknown_name_silently_dropped() -> None:
    """If a resolved name isn't in items (shouldn't happen), skip it."""
    resolved = {"Ghost": RorSpec(vmax_hm3=1.0, production_factor=0.5)}
    assert not pasada_unscale_map(resolved, [])


# ---------------------------------------------------------------------------
# Packaged-template discoverability
# ---------------------------------------------------------------------------


def test_packaged_ror_equivalence_csv_is_discoverable() -> None:
    """The default whitelist ships with the plp2gtopt wheel.

    Regression test for the `pip install .` case: the argparse default
    for ``--ror-as-reservoirs-file`` must resolve to a real file that
    contains the expected MACHICURA entry (which drives the Maule
    irrigation 'embalse' variant auto-detection in gtopt_writer).
    """
    from .._parsers import DEFAULT_ROR_RESERVOIRS_FILE

    # 1. Path resolves under the plp2gtopt package (editable OR wheel).
    assert DEFAULT_ROR_RESERVOIRS_FILE.is_file(), (
        f"packaged RoR whitelist missing at {DEFAULT_ROR_RESERVOIRS_FILE}"
    )
    assert DEFAULT_ROR_RESERVOIRS_FILE.name == "ror_equivalence.csv"
    assert DEFAULT_ROR_RESERVOIRS_FILE.parent.name == "templates"

    # 2. The file actually parses with the production parser.
    spec = parse_ror_equivalence_file(DEFAULT_ROR_RESERVOIRS_FILE)
    assert spec, "packaged whitelist is empty"

    # 3. MACHICURA is the load-bearing entry — its presence is what
    # flips gtopt_writer into the Maule 'embalse' agreement variant.
    assert "MACHICURA" in spec, (
        "MACHICURA missing from packaged RoR whitelist — Maule "
        "irrigation auto-detection will silently fall back to pasada"
    )
    machicura = spec["MACHICURA"]
    assert machicura.vmax_hm3 == pytest.approx(12.0)
    assert machicura.production_factor == pytest.approx(0.327)
    assert machicura.pmax_mw == pytest.approx(95.0)


def test_packaged_ror_template_is_the_argparse_default() -> None:
    """``plp2gtopt --help`` exposes the packaged CSV as the default."""
    import argparse

    from .._parsers import DEFAULT_ROR_RESERVOIRS_FILE, add_ror_arguments

    parser = argparse.ArgumentParser()
    add_ror_arguments(parser, {})
    namespace = parser.parse_args([])
    assert namespace.ror_as_reservoirs_file == DEFAULT_ROR_RESERVOIRS_FILE
    # When --ror-as-reservoirs is not given, resolve_ror_reservoir_spec
    # must still return {} (feature disabled) despite the non-None path.
    assert namespace.ror_as_reservoirs is None
