"""Unit tests for the hydro ``<NAME>min``/``<NAME>max`` UC auto-promotion.

PLEXOS encodes per-plant turbine generation floors / caps as
``ANTUCOmin``/``ANTUCOmax``/``ELTOROmax`` UserConstraints whose LHS is a
single ``generator(<HYDRO>).generation`` term — gtopt models this
natively via ``Commitment.pmin`` + ``Generator.pmax``.  The converter
auto-promotes the matched UCs to ``CommitmentSpec``s and drops the
redundant base UC plus the soft ``discharge_<NAME>min/max`` mirror.
"""

from __future__ import annotations

from plexos2gtopt.entities import (
    CommitmentSpec,
    GeneratorSpec,
    UserConstraintSpec,
)
from plexos2gtopt.parsers import _auto_promote_hydro_min_max_to_commitments


def _hydro_gen(name: str, pmax: float = 200.0) -> GeneratorSpec:
    """Helper — a hydro generator (no fuels) on a dummy bus."""
    return GeneratorSpec(
        object_id=0,
        name=name,
        bus_name="b_a",
        pmax=pmax,
        fuel_names=(),
    )


def _thermal_gen(name: str, fuel: str = "Coal") -> GeneratorSpec:
    """Helper — a thermal gen with a fuel membership (NOT a hydro)."""
    return GeneratorSpec(
        object_id=0,
        name=name,
        bus_name="b_a",
        pmax=300.0,
        fuel_names=(fuel,),
    )


def test_auto_promote_min_only_creates_commitment_and_drops_uc() -> None:
    """``ANTUCOmin`` >= 20 with a single hydro term → Commitment(pmin=20)."""
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCOmin",
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(_hydro_gen("ANTUCO_U1", pmax=200.0),),
        existing_commitments=(),
    )
    assert len(new_commits) == 1
    c = new_commits[0]
    assert c.generator_name == "ANTUCO_U1"
    assert c.pmin == 20.0
    assert c.initial_status == 1.0
    # Base UC dropped.
    assert "ANTUCOmin" in dropped


def test_auto_promote_min_and_max_combines_into_one_commitment() -> None:
    """``ANTUCOmin``+``ANTUCOmax`` → single Commitment(pmin=20).

    ``Generator.pmax`` continues to enforce the upper bound — the
    auto-promoter just removes the now-redundant max UC.
    """
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCOmin",
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
        ),
        UserConstraintSpec(
            name="ANTUCOmax",
            expression='1.0 * generator("ANTUCO_U1").generation <= 200.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(_hydro_gen("ANTUCO_U1", pmax=200.0),),
        existing_commitments=(),
    )
    assert len(new_commits) == 1
    c = new_commits[0]
    assert c.generator_name == "ANTUCO_U1"
    assert c.pmin == 20.0
    assert c.initial_status == 1.0
    assert {"ANTUCOmin", "ANTUCOmax"} <= dropped


def test_auto_promote_also_drops_discharge_mirror() -> None:
    """``ANTUCOmin`` base UC also drops the ``discharge_ANTUCOmin`` mirror."""
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCOmin",
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
        ),
    )
    hydro_ucs = (
        UserConstraintSpec(
            name="discharge_ANTUCOmin",
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
            penalty=10.0,
        ),
        UserConstraintSpec(
            # Different stem — must NOT be dropped.
            name="discharge_OTHERmin",
            expression='1.0 * generator("OTHER_U1").generation >= 5.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=hydro_ucs,
        generators=(_hydro_gen("ANTUCO_U1"),),
        existing_commitments=(),
    )
    assert len(new_commits) == 1
    assert "ANTUCOmin" in dropped
    assert "discharge_ANTUCOmin" in dropped
    assert "discharge_OTHERmin" not in dropped


def test_auto_promote_skips_thermal() -> None:
    """A UC referencing a fuel-bearing (thermal) generator is NOT promoted."""
    base_ucs = (
        UserConstraintSpec(
            name="COALmin",
            expression='1.0 * generator("COAL_U1").generation >= 50.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(_thermal_gen("COAL_U1"),),
        existing_commitments=(),
    )
    assert not new_commits
    assert not dropped


def test_auto_promote_skips_when_commitment_exists() -> None:
    """Existing CommitmentSpec wins — auto-promotion stays out."""
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCOmin",
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(_hydro_gen("ANTUCO_U1"),),
        existing_commitments=(CommitmentSpec(generator_name="ANTUCO_U1"),),
    )
    assert not new_commits
    assert not dropped


def test_auto_promote_skips_multi_term_lhs() -> None:
    """Multi-term LHS (Σ over two gens) does NOT match the single-term rule."""
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCOmin",
            expression=(
                '1.0 * generator("ANTUCO_U1").generation'
                ' + 1.0 * generator("ANTUCO_U2").generation >= 40.0'
            ),
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(
            _hydro_gen("ANTUCO_U1"),
            _hydro_gen("ANTUCO_U2"),
        ),
        existing_commitments=(),
    )
    assert not new_commits
    assert not dropped


def test_auto_promote_no_match_returns_empty() -> None:
    """Empty inputs short-circuit to ``((), frozenset())``."""
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=(),
        hydro_ucs=(),
        generators=(),
        existing_commitments=(),
    )
    assert not new_commits
    assert not dropped


def test_auto_promote_ignores_non_min_max_names() -> None:
    """UC name not matching ``(min|max|maxramp)$`` is left alone."""
    base_ucs = (
        UserConstraintSpec(
            name="ANTUCO_floor",  # NOT a min/max name
            expression='1.0 * generator("ANTUCO_U1").generation >= 20.0',
        ),
    )
    new_commits, dropped = _auto_promote_hydro_min_max_to_commitments(
        base_ucs=base_ucs,
        hydro_ucs=(),
        generators=(_hydro_gen("ANTUCO_U1"),),
        existing_commitments=(),
    )
    assert not new_commits
    assert not dropped
